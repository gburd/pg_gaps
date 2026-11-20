/*-------------------------------------------------------------------------
 *
 * gx.c
 *	  Inference over a ground discrete Bayesian network: exact, or by Gibbs
 *	  sampling for a component too large to solve exactly.
 *
 * gx_infer works in four steps:
 *
 *	1. Split the missing variables into components.  Two missing variables
 *	   are in one component when some CPT mentions both.  Observed
 *	   variables block: given its value, a CPT that mentions it contributes
 *	   only a slice.  Components are independent given the evidence, so
 *	   each is solved on its own.
 *	2. For each component, build its factors: every CPT that mentions one
 *	   of its missing variables, reduced by the evidence.  CPTs over only
 *	   observed variables are constants that cancel in every posterior and
 *	   are skipped (except that a zero there makes the evidence impossible).
 *	3. Look the component up in a cache keyed by its canonical form (its
 *	   variables' cardinalities in order, and its reduced factors).  Equal
 *	   components have equal posteriors; this is BayesStore's first-order
 *	   sharing done on whole components (doc/DESIGN.md, "Inference").
 *	4. On a miss, run variable elimination with a min-fill order: once per
 *	   variable for its marginal (sum-product), and once max-product with
 *	   back-pointers for the MAP assignment.  Every factor product is
 *	   checked against env->max_cells before it is allocated.  When the
 *	   bounds stop exact inference and env->samples > 0, the component is
 *	   sampled instead (see "Gibbs sampling" below).
 *
 * The per-variable marginal elimination is O(n) eliminations per component
 * where a junction tree would share work; components are small in the
 * workloads Phase 2 targets, and the simpler algorithm is easier to trust.
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include "engine/gx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * A factor over an ordered set of variables, row-major with the last
 * variable varying fastest.  Variables are local indices within the
 * component being solved.
 *
 * Values are natural logs (-INFINITY for zero), from the reduced CPTs on.
 * Products of probabilities underflow a double after a few hundred terms,
 * and rescaling a message so its largest cell is 1 is not enough: when two
 * strong messages pull against each other, a message's small cells are
 * still far below its largest and round to zero long before the answer is
 * in doubt.  In logs the dynamic range is the exponent's, so only the
 * final marginal leaves the log domain.
 */
typedef struct factor
{
	int			nv;
	int		   *vars;
	size_t		ncells;
	double	   *vals;
} factor;

typedef struct ctx
{
	const gx_env *env;
	const gx_bn *bn;
	int			err;
	size_t		peak;
	size_t		held;			/* cells the current component holds outside
								 * solve(), charged before solve() starts */
	double		spread;			/* the last sampled component's chain
								 * disagreement, see gibbs_pool() */
} ctx;

static void *
xalloc(ctx *c, size_t size)
{
	void	   *p;

	if (c->err)
		return NULL;
	p = c->env->alloc(c->env->arg, size ? size : 1);
	if (p == NULL)
		c->err = GX_ENOMEM;
	return p;
}

static void
xfree(ctx *c, void *p)
{
	if (p != NULL)
		c->env->free(c->env->arg, p);
}

static void
interrupt(ctx *c)
{
	if (c->env->interrupt)
		c->env->interrupt(c->env->arg);
}

/*
 * Product of cardinalities, or SIZE_MAX if it would exceed limit.  Checking
 * before multiplying keeps the bound safe from overflow.
 */
static size_t
cells_bounded(const int *card, const int *vars, int nv, size_t limit)
{
	size_t		n = 1;

	for (int i = 0; i < nv; i++)
	{
		size_t		k = (size_t) card[vars[i]];

		if (n > limit / k)
			return SIZE_MAX;
		n *= k;
	}
	return n;
}

/* ----------------------------------------------------------------
 * Validation
 * ----------------------------------------------------------------
 */

/*
 * Check one CPT: child, parents and entries.  Sets *bad_var on failure.
 * mark[] is all-zero on entry and on return; it finds duplicate parents
 * in time linear in their number.
 */
static int
check_cpt(const gx_bn *bn, const gx_cpt *c, unsigned char *mark, int *bad_var)
{
	size_t		n = 1;
	int			rc = GX_EINVAL;

	if (c->child < 0 || c->child >= bn->nvars || c->nparents < 0 ||
		c->nparents > bn->nvars ||
		(c->nparents > 0 && c->parents == NULL) || c->vals == NULL)
	{
		*bad_var = (c->child >= 0 && c->child < bn->nvars) ? c->child : -1;
		return GX_EINVAL;
	}
	*bad_var = c->child;
	{
		int			j;

		for (j = 0; j < c->nparents; j++)
		{
			int			p = c->parents[j];

			if (p < 0 || p >= bn->nvars || p == c->child || mark[p])
				break;
			mark[p] = 1;
			/* The caller bounds table sizes; this only guards the scan below. */
			if (n > SIZE_MAX / (size_t) bn->card[p])
			{
				j++;
				break;
			}
			n *= (size_t) bn->card[p];
		}
		for (int k = 0; k < j; k++)
			if (c->parents[k] >= 0 && c->parents[k] < bn->nvars)
				mark[c->parents[k]] = 0;
		if (j < c->nparents)
			return rc;
	}
	if (n > SIZE_MAX / (size_t) bn->card[c->child])
		return rc;
	n *= (size_t) bn->card[c->child];
	for (size_t k = 0; k < n; k++)
		if (!isfinite(c->vals[k]) || c->vals[k] < 0.0)
			return rc;
	*bad_var = -1;
	return GX_OK;
}

/*
 * Directed cycles, by Kahn's algorithm: repeatedly remove a variable with
 * no unremoved parents.  Whatever remains lies on or behind a cycle.
 * Linear in variables plus edges.  On entry indeg[v] holds v's parent
 * count and kstart[u + 1] the number of u's children; queue is scratch.
 */
static int
check_acyclic(ctx *cp, const gx_bn *bn, int *indeg, int *queue, int *kstart,
			  size_t nedges, int *bad_var)
{
	ctx			c = *cp;		/* a copy: gx_check keeps its own error state */
	int		   *kids;
	int			head = 0,
				tail = 0,
				rc = GX_OK;

	/* Child lists, by a counting sort of the edges on their parent. */
	for (int v = 0; v < bn->nvars; v++)
		kstart[v + 1] += kstart[v];
	kids = xalloc(&c, sizeof(int) * (nedges ? nedges : 1));
	if (c.err)
		return c.err;
	{
		int		   *fill = queue;	/* reused as a cursor before the walk */

		for (int v = 0; v < bn->nvars; v++)
			fill[v] = kstart[v];
		for (int i = 0; i < bn->ncpts; i++)
			for (int j = 0; j < bn->cpts[i].nparents; j++)
				kids[fill[bn->cpts[i].parents[j]]++] = bn->cpts[i].child;
	}

	for (int v = 0; v < bn->nvars; v++)
		if (indeg[v] == 0)
			queue[tail++] = v;
	while (head < tail)
	{
		int			u = queue[head++];

		if ((head & 1023) == 0)
			interrupt(&c);
		for (int e = kstart[u]; e < kstart[u + 1]; e++)
			if (--indeg[kids[e]] == 0)
				queue[tail++] = kids[e];
	}
	if (tail < bn->nvars)
	{
		for (int v = 0; v < bn->nvars; v++)
			if (indeg[v] > 0)
			{
				*bad_var = v;
				break;
			}
		rc = GX_ECYCLE;
	}

	xfree(&c, kids);
	return rc;
}

int
gx_check(const gx_env *env, const gx_bn *bn, int *bad_var)
{
	ctx			c = {env, bn, GX_OK, 0, 0, 0.0};
	int		   *owner = NULL;	/* CPT index per variable */
	int		   *indeg = NULL;	/* unremoved parents per variable */
	int		   *queue = NULL;
	int		   *kstart = NULL;	/* child-list offsets, see check_acyclic */
	unsigned char *mark = NULL;
	size_t		nedges = 0;
	int			rc = GX_OK;

	*bad_var = -1;
	if (bn->nvars < 0 || bn->ncpts != bn->nvars)
		return GX_EINVAL;
	for (int v = 0; v < bn->nvars; v++)
		if (bn->card[v] < 1 || bn->evidence[v] < -1 ||
			bn->evidence[v] >= bn->card[v])
		{
			*bad_var = v;
			return GX_EINVAL;
		}

	owner = xalloc(&c, sizeof(int) * (size_t) (bn->nvars ? bn->nvars : 1));
	indeg = xalloc(&c, sizeof(int) * (size_t) (bn->nvars ? bn->nvars : 1));
	queue = xalloc(&c, sizeof(int) * (size_t) (bn->nvars ? bn->nvars : 1));
	kstart = xalloc(&c, sizeof(int) * (size_t) (bn->nvars + 1));
	mark = xalloc(&c, (size_t) (bn->nvars ? bn->nvars : 1));
	if (c.err)
	{
		rc = c.err;
		goto done;
	}
	for (int v = 0; v < bn->nvars; v++)
	{
		owner[v] = -1;
		mark[v] = 0;
		kstart[v] = 0;
	}
	kstart[bn->nvars] = 0;

	/*
	 * Exactly one well-formed CPT per variable.  Duplicate parents are found
	 * with a mark array, linear in the parent count.
	 */
	for (int i = 0; i < bn->ncpts; i++)
	{
		const gx_cpt *cpt = &bn->cpts[i];

		interrupt(&c);
		if ((rc = check_cpt(bn, cpt, mark, bad_var)) != GX_OK)
			goto done;
		if (owner[cpt->child] >= 0)
		{
			*bad_var = cpt->child;
			rc = GX_EINVAL;
			goto done;
		}
		owner[cpt->child] = i;
		indeg[cpt->child] = cpt->nparents;
		for (int j = 0; j < cpt->nparents; j++)
			kstart[cpt->parents[j] + 1]++;
		nedges += (size_t) cpt->nparents;
	}

	rc = check_acyclic(&c, bn, indeg, queue, kstart, nedges, bad_var);

done:
	xfree(&c, owner);
	xfree(&c, indeg);
	xfree(&c, queue);
	xfree(&c, kstart);
	xfree(&c, mark);
	return rc;
}

/* ----------------------------------------------------------------
 * Factors
 * ----------------------------------------------------------------
 */

static void
factor_free(ctx *c, factor *f)
{
	xfree(c, f->vars);
	xfree(c, f->vals);
	f->vars = NULL;
	f->vals = NULL;
}

/*
 * Allocate a factor over vars (local indices, card[] gives their sizes),
 * failing with GX_ETOOBIG if it would exceed max_cells.
 */
static int
factor_init(ctx *c, factor *f, const int *card, const int *vars, int nv)
{
	f->nv = nv;
	f->ncells = cells_bounded(card, vars, nv, c->env->max_cells);
	f->vars = NULL;
	f->vals = NULL;
	if (f->ncells == SIZE_MAX)
	{
		c->err = GX_ETOOBIG;
		return c->err;
	}
	f->vars = xalloc(c, sizeof(int) * (size_t) (nv ? nv : 1));
	f->vals = xalloc(c, sizeof(double) * f->ncells);
	if (c->err)
	{
		/* On failure nothing is left allocated, so callers need no cleanup. */
		xfree(c, f->vars);
		xfree(c, f->vals);
		f->vars = NULL;
		f->vals = NULL;
		return c->err;
	}
	memcpy(f->vars, vars, sizeof(int) * (size_t) nv);
	if (f->ncells > c->peak)
		c->peak = f->ncells;
	return GX_OK;
}

/*
 * Index into f of the assignment held in val[] (indexed by local variable).
 */
static size_t
factor_index(const factor *f, const int *card, const int *val)
{
	size_t		idx = 0;

	for (int i = 0; i < f->nv; i++)
		idx = idx * (size_t) card[f->vars[i]] + (size_t) val[f->vars[i]];
	return idx;
}

/*
 * Advance the assignment of vars[0..nv) in val[], last variable fastest.
 * Returns 0 after the final assignment.
 */
static int
odometer(const int *card, const int *vars, int nv, int *val)
{
	for (int i = nv - 1; i >= 0; i--)
	{
		if (++val[vars[i]] < card[vars[i]])
			return 1;
		val[vars[i]] = 0;
	}
	return 0;
}

/*
 * The variables of the product of fs[] into all[], and those of them not in
 * keep[] into elim[].  The scope is gathered from the factors themselves, so
 * the cost is proportional to their sizes, not to the component.
 * inscope[] is all-zero on entry and on return.
 */
static void
split_scope(factor *const *fs, int nf, const int *keep, int nkeep,
			unsigned char *inscope, int *all, int *nall, int *elim, int *nelim)
{
	*nall = *nelim = 0;
	for (int i = 0; i < nf; i++)
		for (int j = 0; j < fs[i]->nv; j++)
			if (!inscope[fs[i]->vars[j]])
			{
				inscope[fs[i]->vars[j]] = 1;
				all[(*nall)++] = fs[i]->vars[j];
			}
	for (int i = 0; i < nkeep; i++)
		inscope[keep[i]] = 2;
	for (int i = 0; i < *nall; i++)
		if (inscope[all[i]] == 1)
			elim[(*nelim)++] = all[i];
	for (int i = 0; i < *nall; i++)
		inscope[all[i]] = 0;
	for (int i = 0; i < nkeep; i++)
		inscope[keep[i]] = 0;
}

/*
 * Multiply factors fs[0..nf) and project the product onto the variables
 * keep[0..nkeep), by summing (maxprod = 0) or maximizing (maxprod = 1)
 * over the rest.  keep must be a subset of the product's scope.
 *
 * When argmax is non-NULL there must be exactly one variable x eliminated;
 * argmax then receives, per cell of out, the value of x that attains the
 * maximum.
 *
 * Factors hold logs (see struct factor), so a product is a sum.  Each
 * output cell's terms are combined with a running log-sum-exp (for sums)
 * or a running maximum (for max), and the output is shifted so its largest
 * cell is 0, with *logscale set to the shift.  Posteriors are ratios, so
 * only the scale is lost, and the caller tracks it.  An all-zero result
 * stays -INFINITY everywhere with *logscale = -INFINITY.
 *
 * The full product scope is bounded by max_cells before anything is
 * allocated, since that is the work done even though it is never stored.
 */
static int
product_project(ctx *c, const int *card, factor *const *fs, int nf,
				const int *keep, int nkeep, int maxprod,
				factor *out, factor *argmax, double *logscale,
				int *val, unsigned char *inscope)
{
	int		   *all,
			   *elim;
	int			nall = 0,
				nelim = 0;
	double		lmx = -INFINITY;
	size_t		work = 0;

	int			tot = 0;

	*logscale = 0.0;
	for (int i = 0; i < nf; i++)
		tot += fs[i]->nv;
	all = xalloc(c, sizeof(int) * (size_t) (tot ? tot : 1));
	elim = xalloc(c, sizeof(int) * (size_t) (tot ? tot : 1));
	if (c->err)
		goto done;

	split_scope(fs, nf, keep, nkeep, inscope, all, &nall, elim, &nelim);

	if (cells_bounded(card, all, nall, c->env->max_cells) == SIZE_MAX)
	{
		c->err = GX_ETOOBIG;
		goto done;
	}
	if (factor_init(c, out, card, keep, nkeep) != GX_OK)
		goto done;
	if (argmax && factor_init(c, argmax, card, keep, nkeep) != GX_OK)
		goto done;

	for (int i = 0; i < nall; i++)
		val[all[i]] = 0;
	for (size_t cell = 0; cell < out->ncells; cell++)
	{
		double		acc = -INFINITY;	/* log of the running sum or max */
		int			best = 0;
		int			have = 0;

		for (int i = 0; i < nelim; i++)
			val[elim[i]] = 0;
		do
		{
			double		lp = 0.0;

			/*
			 * Interrupt by work done, not by output cells: one output cell can
			 * sum over max_cells terms of many factors each.
			 */
			work += (size_t) nf + 1;
			if (work >= 65536)
			{
				interrupt(c);
				work = 0;
			}

			for (int i = 0; i < nf && lp > -INFINITY; i++)
				lp += fs[i]->vals[factor_index(fs[i], card, val)];
			if (maxprod)
			{
				/* First term wins ties, so a zero cell still gets a value. */
				if (!have || lp > acc)
				{
					acc = lp;
					best = nelim ? val[elim[0]] : 0;
				}
			}
			else if (lp > -INFINITY)
			{
				if (acc == -INFINITY)
					acc = lp;
				else if (lp > acc)
					acc = lp + log1p(exp(acc - lp));
				else
					acc = acc + log1p(exp(lp - acc));
			}
			have = 1;
		} while (odometer(card, elim, nelim, val));
		out->vals[cell] = acc;	/* a log for now; converted below */
		if (argmax)
			argmax->vals[cell] = (double) best;
		if (acc > lmx)
			lmx = acc;
		odometer(card, out->vars, out->nv, val);
	}

	/* Shift by the largest log, so the factor's largest cell is log 1 = 0. */
	if (lmx > -INFINITY)
		for (size_t cell = 0; cell < out->ncells; cell++)
			out->vals[cell] -= lmx;
	*logscale = lmx;

done:
	xfree(c, all);
	xfree(c, elim);
	return c->err;
}

/* ----------------------------------------------------------------
 * Variable elimination within one component
 * ----------------------------------------------------------------
 */

/*
 * Greedy min-fill elimination order over the component's nloc variables,
 * from the interaction graph of its factors.
 *
 * The graph is held as sorted adjacency arrays, so memory is proportional
 * to the edges, including the fill edges elimination adds; the total is
 * charged to max_total_cells.  Each live variable's fill count (the number
 * of its neighbour pairs not yet adjacent) is cached and recomputed only
 * where an elimination can change it: the eliminated variable's neighbours
 * and their neighbours.  Selection is a linear scan.  The graphs first-order
 * models ground to are sparse, so this is close to linear in practice; a
 * dense component is bounded by max_cells long before this cost matters.
 */
typedef struct adjlist
{
	int			n;
	int			cap;
	int			live;			/* neighbours not yet eliminated */
	int		   *v;				/* sorted, live and eliminated neighbours */
} adjlist;

static int
adj_has(const adjlist *a, int x)
{
	int			lo = 0,
				hi = a->n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (a->v[mid] == x)
			return 1;
		if (a->v[mid] < x)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0;
}

static int
adj_add(ctx *c, adjlist *a, int x, size_t *used)
{
	int			i;

	if (adj_has(a, x))
		return GX_OK;
	if (a->n == a->cap)
	{
		int			ncap = a->cap ? a->cap * 2 : 4;
		int		   *nv;

		*used += (size_t) (ncap - a->cap);
		if (*used > c->env->max_total_cells)
		{
			c->err = GX_ETOOBIG;
			return c->err;
		}
		nv = xalloc(c, sizeof(int) * (size_t) ncap);
		if (c->err)
			return c->err;
		if (a->n)
			memcpy(nv, a->v, sizeof(int) * (size_t) a->n);
		xfree(c, a->v);
		a->v = nv;
		a->cap = ncap;
	}
	for (i = a->n; i > 0 && a->v[i - 1] > x; i--)
		a->v[i] = a->v[i - 1];
	a->v[i] = x;
	a->n++;
	a->live++;
	return GX_OK;
}

static int
int_cmp(const void *a, const void *b)
{
	int			x = *(const int *) a,
				y = *(const int *) b;

	return (x > y) - (x < y);
}

/*
 * Append without keeping order or uniqueness; adj_finish sorts and
 * deduplicates.  Used to build the initial graph, where inserting in order
 * would be quadratic in a hub's degree.
 */
static int
adj_append(ctx *c, adjlist *a, int x, size_t *used)
{
	if (a->n == a->cap)
	{
		int			ncap = a->cap ? a->cap * 2 : 4;
		int		   *nv;

		*used += (size_t) (ncap - a->cap);
		if (*used > c->env->max_total_cells)
		{
			c->err = GX_ETOOBIG;
			return c->err;
		}
		nv = xalloc(c, sizeof(int) * (size_t) ncap);
		if (c->err)
			return c->err;
		if (a->n)
			memcpy(nv, a->v, sizeof(int) * (size_t) a->n);
		xfree(c, a->v);
		a->v = nv;
		a->cap = ncap;
	}
	a->v[a->n++] = x;
	return GX_OK;
}

static void
adj_finish(adjlist *a)
{
	int			k = 0;

	if (a->n > 1)
		qsort(a->v, (size_t) a->n, sizeof(int), int_cmp);
	for (int i = 0; i < a->n; i++)
		if (k == 0 || a->v[k - 1] != a->v[i])
			a->v[k++] = a->v[i];
	a->n = a->live = k;
}

/*
 * Live degree of u.  Kept in adjlist.live, which minfill_order decrements
 * for every neighbour of an eliminated variable and increments as fill
 * edges are added, so it costs O(1) where a scan of a hub would cost its
 * whole degree.
 */
static long
live_degree(const adjlist *g, const unsigned char *gone, int u)
{
	(void) gone;
	return g[u].live;
}

/* Live neighbours of u into nbr[]; returns how many. */
static int
live_nbrs(const adjlist *g, const unsigned char *gone, int u, int *nbr)
{
	int			nn = 0;

	for (int i = 0; i < g[u].n; i++)
		if (!gone[g[u].v[i]])
			nbr[nn++] = g[u].v[i];
	return nn;
}

/*
 * Above this many live neighbours a variable's fill is not counted but
 * bounded by nn(nn-1)/2, every pair.  Counting costs O(nn^2) and a hub
 * would be recounted after each neighbour's elimination, which is cubic on
 * a star.  The bound only ranks hubs late, where min-fill puts them anyway;
 * the order affects speed, never the answer, and max_cells still bounds
 * any factor a poor order would build.
 */
#define FILL_EXACT_DEGREE 64

static long
fill_of(const adjlist *g, const int *nbr, int nn)
{
	long		fill = 0;

	if (nn > FILL_EXACT_DEGREE)
		return (long) nn * (nn - 1) / 2;

	for (int i = 0; i < nn; i++)
		for (int j = i + 1; j < nn; j++)
			if (!adj_has(&g[nbr[i]], nbr[j]))
				fill++;
	return fill;
}

/*
 * A binary min-heap of (fill, variable) with lazy deletion: a variable's
 * fill changes by pushing a new entry, and pops skip entries whose fill no
 * longer matches.  Ties break on the variable number, so the order is
 * deterministic.
 */
typedef struct hent
{
	long		fill;
	int			v;
} hent;

static int
hent_less(hent a, hent b)
{
	return a.fill < b.fill || (a.fill == b.fill && a.v < b.v);
}

static int
heap_push(ctx *c, hent **h, int *n, int *cap, hent e, size_t *used)
{
	int			i;

	if (*n == *cap)
	{
		int			ncap = *cap ? *cap * 2 : 64;
		hent	   *nh;

		*used += (size_t) (ncap - *cap);
		if (*used > c->env->max_total_cells)
		{
			c->err = GX_ETOOBIG;
			return c->err;
		}
		nh = xalloc(c, sizeof(hent) * (size_t) ncap);
		if (c->err)
			return c->err;
		if (*n)
			memcpy(nh, *h, sizeof(hent) * (size_t) *n);
		xfree(c, *h);
		*h = nh;
		*cap = ncap;
	}
	for (i = (*n)++; i > 0 && hent_less(e, (*h)[(i - 1) / 2]); i = (i - 1) / 2)
		(*h)[i] = (*h)[(i - 1) / 2];
	(*h)[i] = e;
	return GX_OK;
}

static hent
heap_pop(hent *h, int *n)
{
	hent		top = h[0],
				last = h[--(*n)];
	int			i = 0;

	for (;;)
	{
		int			k = 2 * i + 1;

		if (k >= *n)
			break;
		if (k + 1 < *n && hent_less(h[k + 1], h[k]))
			k++;
		if (!hent_less(h[k], last))
			break;
		h[i] = h[k];
		i = k;
	}
	if (*n > 0)
		h[i] = last;
	return top;
}

/* State of minfill_order, allocated and freed together. */
typedef struct mfstate
{
	ctx		   *c;
	int			nloc;
	adjlist    *g;
	hent	   *heap;
	int			hn,
				hcap;
	unsigned char *gone;
	unsigned char *dirty;
	long	   *fill;
	int		   *nbr;
	int		   *nbr2;
	int		   *touched;
	size_t		used;
} mfstate;

/* Fill of u as minfill_order maintains it: exact, or bounded for a hub. */
static long
current_fill(mfstate *m, int u)
{
	long		d = live_degree(m->g, m->gone, u);

	if (d > FILL_EXACT_DEGREE)
		return d * (d - 1) / 2;
	return fill_of(m->g, m->nbr2, live_nbrs(m->g, m->gone, u, m->nbr2));
}

static int
mf_push(mfstate *m, int u)
{
	hent		e;

	e.fill = m->fill[u];
	e.v = u;
	return heap_push(m->c, &m->heap, &m->hn, &m->hcap, e, &m->used);
}

/* Build the interaction graph of the factors and seed the heap. */
static int
mf_build(mfstate *m, factor *fs, int nf)
{
	ctx		   *c = m->c;

	for (int i = 0; i < nf; i++)
	{
		interrupt(c);
		for (int a = 0; a < fs[i].nv; a++)
			for (int b = 0; b < fs[i].nv; b++)
				if (a != b &&
					adj_append(c, &m->g[fs[i].vars[a]], fs[i].vars[b], &m->used) != GX_OK)
					return c->err;
	}
	for (int u = 0; u < m->nloc; u++)
		adj_finish(&m->g[u]);
	for (int u = 0; u < m->nloc; u++)
	{
		interrupt(c);
		m->fill[u] = current_fill(m, u);
		if (mf_push(m, u) != GX_OK)
			return c->err;
	}
	return GX_OK;
}

/*
 * Eliminate best: connect its live neighbours pairwise, then refresh the
 * fills of its neighbours and their neighbours.  A hub (more than
 * FILL_EXACT_DEGREE neighbours) is refreshed from its degree alone, and
 * its own neighbours are not visited through it: their fills only change
 * when an edge among them is added, which happens when one of their
 * common neighbours is eliminated, and that elimination refreshes them
 * directly.
 */
static int
mf_eliminate(mfstate *m, int best)
{
	ctx		   *c = m->c;
	adjlist    *g = m->g;
	int			nn,
				nt = 0;

	m->gone[best] = 1;
	nn = live_nbrs(g, m->gone, best, m->nbr);
	for (int i = 0; i < nn; i++)
		g[m->nbr[i]].live--;
	for (int i = 0; i < nn; i++)
		for (int j = 0; j < nn; j++)
			if (i != j && adj_add(c, &g[m->nbr[i]], m->nbr[j], &m->used) != GX_OK)
				return c->err;

	for (int i = 0; i < nn; i++)
	{
		int			u = m->nbr[i],
					n2;

		if (!m->dirty[u])
		{
			m->dirty[u] = 1;
			m->touched[nt++] = u;
		}
		if (live_degree(g, m->gone, u) > FILL_EXACT_DEGREE)
			continue;
		n2 = live_nbrs(g, m->gone, u, m->nbr2);
		for (int j = 0; j < n2; j++)
			if (!m->dirty[m->nbr2[j]])
			{
				m->dirty[m->nbr2[j]] = 1;
				m->touched[nt++] = m->nbr2[j];
			}
	}
	for (int i = 0; i < nt; i++)
	{
		int			u = m->touched[i];
		long		f = current_fill(m, u);

		m->dirty[u] = 0;
		if (f == m->fill[u])
			continue;
		m->fill[u] = f;
		if (mf_push(m, u) != GX_OK)
			return c->err;
	}
	return GX_OK;
}

static int
minfill_order(ctx *c, int nloc, factor *fs, int nf, int *order)
{
	mfstate		m;

	memset(&m, 0, sizeof(m));
	m.c = c;
	m.nloc = nloc;
	m.used = c->held;			/* the component's factors, see do_component */
	m.g = xalloc(c, sizeof(adjlist) * (size_t) nloc);
	if (m.g)
		memset(m.g, 0, sizeof(adjlist) * (size_t) nloc);
	m.gone = xalloc(c, (size_t) nloc);
	m.dirty = xalloc(c, (size_t) nloc);
	m.fill = xalloc(c, sizeof(long) * (size_t) nloc);
	m.nbr = xalloc(c, sizeof(int) * (size_t) nloc);
	m.nbr2 = xalloc(c, sizeof(int) * (size_t) nloc);
	m.touched = xalloc(c, sizeof(int) * (size_t) nloc);
	if (!c->err)
	{
		memset(m.gone, 0, (size_t) nloc);
		memset(m.dirty, 0, (size_t) nloc);
	}

	if (!c->err && mf_build(&m, fs, nf) == GX_OK)
	{
		for (int k = 0; k < nloc && !c->err; k++)
		{
			int			best = -1;

			interrupt(c);

			/* Lazy deletion: skip entries whose fill has since changed. */
			while (m.hn > 0)
			{
				hent		e = heap_pop(m.heap, &m.hn);

				if (!m.gone[e.v] && e.fill == m.fill[e.v])
				{
					best = e.v;
					break;
				}
			}
			order[k] = best;
			mf_eliminate(&m, best);
		}
	}

	if (m.g)
		for (int u = 0; u < nloc; u++)
			xfree(c, m.g[u].v);
	xfree(c, m.g);
	xfree(c, m.heap);
	xfree(c, m.gone);
	xfree(c, m.dirty);
	xfree(c, m.fill);
	xfree(c, m.nbr);
	xfree(c, m.nbr2);
	xfree(c, m.touched);
	return c->err;
}

/*
 * Solve one component with a bucket tree, the junction tree that variable
 * elimination builds as it goes (Dechter's bucket-tree elimination).
 *
 * Upward pass: eliminate the variables in min-fill order.  Bucket k holds
 * the base factors whose first-eliminated variable is order[k], and the
 * messages sent to it.  Its outgoing message is the product of the
 * bucket's contents summed over order[k], and goes to the bucket of the
 * earliest-eliminated variable left in its scope, the bucket's parent.
 *
 * Downward pass, root first: a bucket's marginal is the product of its
 * contents and the message from its parent, summed onto its own variable.
 * The message to a child is the same product without that child's message,
 * summed onto the child message's scope.
 *
 * Every message is rescaled to a maximum of 1 (product_project).  The
 * marginals are normalized per variable and do not need the scales.  The
 * MAP pass is the upward pass again with max for sum, plus back-pointers;
 * its scale and the sum pass's give P(MAP | evidence) in log space, so
 * neither underflows.
 *
 * Every factor is bounded by max_cells, and all stored messages together by
 * max_total_cells (gx_env).
 */
typedef struct bucket
{
	int			nbase;
	factor	  **base;			/* borrowed */
	int			parent;			/* bucket index, or -1 at a root */
	factor		up;				/* message to the parent */
	factor		down;			/* message from the parent (nv 0 at a root) */
	int			nkids;
	int		   *kids;
} bucket;

static int
budget(ctx *c, size_t *used, const factor *f)
{
	*used += f->ncells;
	if (*used > c->env->max_total_cells)
		c->err = GX_ETOOBIG;
	return c->err;
}

/*
 * Scope of the product of fs[] minus variable x, into vars[].  inscope[] is
 * all-zero on entry and on return.
 */
static int
scope_minus(factor *const *fs, int nf, int x, int *vars,
			unsigned char *inscope)
{
	int			n = 0;

	inscope[x] = 1;
	for (int i = 0; i < nf; i++)
		for (int j = 0; j < fs[i]->nv; j++)
			if (!inscope[fs[i]->vars[j]])
			{
				inscope[fs[i]->vars[j]] = 1;
				vars[n++] = fs[i]->vars[j];
			}
	inscope[x] = 0;
	for (int i = 0; i < n; i++)
		inscope[vars[i]] = 0;
	return n;
}

/*
 * State shared by the phases of solve().  Everything here is allocated by
 * solve_init and released by solve_free, whatever happens in between.
 */
typedef struct solver
{
	ctx		   *c;
	int			nloc;
	const int  *card;
	factor	   *fs;
	int			nf;
	int		   *order;			/* elimination order */
	int		   *pos;			/* position of each variable in order */
	int		   *val;			/* assignment scratch, per variable */
	int		   *vars;			/* scope scratch */
	int		   *kidpool;
	int		   *first_kid;		/* upward-pass kid lists */
	int		   *next_kid;
	int		   *cvars;			/* cluster scope scratch, nloc + 1 */
	unsigned char *inscope;		/* all-zero between uses */
	factor	  **basepool;
	factor	  **work;			/* factor-list scratch, nf + nloc + 1 */
	bucket	   *b;
	factor	   *argmax;
	factor	   *maxmsg;
	size_t		used;			/* cells held, against max_total_cells */
	double		logz;			/* log P(evidence), up to the CPT constants */
} solver;

static int
solve_init(solver *sv)
{
	ctx		   *c = sv->c;
	int			nloc = sv->nloc;

	sv->used = c->held;			/* the component's factors, see do_component */
	sv->order = xalloc(c, sizeof(int) * (size_t) nloc);
	sv->pos = xalloc(c, sizeof(int) * (size_t) nloc);
	sv->val = xalloc(c, sizeof(int) * (size_t) nloc);
	sv->vars = xalloc(c, sizeof(int) * (size_t) nloc);
	sv->kidpool = xalloc(c, sizeof(int) * (size_t) nloc);
	sv->first_kid = xalloc(c, sizeof(int) * (size_t) nloc);
	sv->next_kid = xalloc(c, sizeof(int) * (size_t) nloc);
	sv->cvars = xalloc(c, sizeof(int) * (size_t) (nloc + 1));
	sv->inscope = xalloc(c, (size_t) nloc);
	sv->basepool = xalloc(c, sizeof(factor *) * (size_t) (sv->nf ? sv->nf : 1));
	sv->work = xalloc(c, sizeof(factor *) * (size_t) (sv->nf + nloc + 1));
	sv->b = xalloc(c, sizeof(bucket) * (size_t) nloc);
	if (sv->b)
		memset(sv->b, 0, sizeof(bucket) * (size_t) nloc);
	sv->argmax = xalloc(c, sizeof(factor) * (size_t) nloc);
	if (sv->argmax)
		memset(sv->argmax, 0, sizeof(factor) * (size_t) nloc);
	sv->maxmsg = xalloc(c, sizeof(factor) * (size_t) nloc);
	if (sv->maxmsg)
		memset(sv->maxmsg, 0, sizeof(factor) * (size_t) nloc);
	if (c->err)
		return c->err;
	memset(sv->inscope, 0, (size_t) nloc);
	return GX_OK;
}

static void
solve_free(solver *sv)
{
	ctx		   *c = sv->c;

	for (int k = 0; k < sv->nloc; k++)
	{
		if (sv->b)
		{
			factor_free(c, &sv->b[k].up);
			factor_free(c, &sv->b[k].down);
		}
		if (sv->argmax)
			factor_free(c, &sv->argmax[k]);
		if (sv->maxmsg)
			factor_free(c, &sv->maxmsg[k]);
	}
	xfree(c, sv->argmax);
	xfree(c, sv->maxmsg);
	xfree(c, sv->b);
	xfree(c, sv->work);
	xfree(c, sv->basepool);
	xfree(c, sv->inscope);
	xfree(c, sv->kidpool);
	xfree(c, sv->first_kid);
	xfree(c, sv->next_kid);
	xfree(c, sv->cvars);
	xfree(c, sv->vars);
	xfree(c, sv->val);
	xfree(c, sv->pos);
	xfree(c, sv->order);
}

/* Assign each base factor to the bucket of its first-eliminated variable. */
static int
solve_assign(solver *sv)
{
	ctx		   *c = sv->c;
	bucket	   *b = sv->b;
	int		   *first = xalloc(c, sizeof(int) * (size_t) (sv->nf ? sv->nf : 1));
	int			off = 0;

	if (c->err)
		return c->err;
	for (int i = 0; i < sv->nf; i++)
	{
		int			m = sv->nloc;

		for (int j = 0; j < sv->fs[i].nv; j++)
			if (sv->pos[sv->fs[i].vars[j]] < m)
				m = sv->pos[sv->fs[i].vars[j]];
		first[i] = m;			/* every component factor has a variable */
		b[m].nbase++;
	}
	for (int k = 0; k < sv->nloc; k++)
	{
		b[k].base = sv->basepool + off;
		off += b[k].nbase;
		b[k].nbase = 0;
	}
	for (int i = 0; i < sv->nf; i++)
		b[first[i]].base[b[first[i]].nbase++] = &sv->fs[i];
	xfree(c, first);
	return GX_OK;
}

/*
 * Upward pass.  A bucket's kids are all eliminated before it, so they are
 * known by the time it runs: each sent message is linked onto its parent's
 * list (next_kid), and the lists become kid arrays afterwards.
 */
static int
solve_upward(solver *sv)
{
	ctx		   *c = sv->c;
	bucket	   *b = sv->b;
	int			nloc = sv->nloc;
	int			off = 0;

	for (int k = 0; k < nloc; k++)
		sv->first_kid[k] = -1;
	sv->logz = 0.0;
	for (int k = 0; k < nloc; k++)
	{
		int			n = 0,
					ns;
		double		ls;

		interrupt(c);
		for (int i = 0; i < b[k].nbase; i++)
			sv->work[n++] = b[k].base[i];
		for (int j = sv->first_kid[k]; j >= 0; j = sv->next_kid[j])
			sv->work[n++] = &b[j].up;
		ns = scope_minus(sv->work, n, sv->order[k], sv->vars, sv->inscope);
		if (product_project(c, sv->card, sv->work, n, sv->vars, ns, 0, &b[k].up,
							NULL, &ls, sv->val, sv->inscope) != GX_OK ||
			budget(c, &sv->used, &b[k].up) != GX_OK)
			return c->err;
		sv->logz += ls;
		b[k].parent = -1;
		for (int i = 0; i < ns; i++)
			if (b[k].parent < 0 || sv->pos[sv->vars[i]] < b[k].parent)
				b[k].parent = sv->pos[sv->vars[i]];
		if (b[k].parent >= 0)
		{
			sv->next_kid[k] = sv->first_kid[b[k].parent];
			sv->first_kid[b[k].parent] = k;
		}
	}
	if (!isfinite(sv->logz))
	{
		c->err = GX_EZERO;
		return c->err;
	}

	for (int k = 0; k < nloc; k++)
		if (b[k].parent >= 0)
			b[b[k].parent].nkids++;
	for (int k = 0; k < nloc; k++)
	{
		b[k].kids = sv->kidpool + off;
		off += b[k].nkids;
		b[k].nkids = 0;
	}
	for (int k = 0; k < nloc; k++)
		if (b[k].parent >= 0)
			b[b[k].parent].kids[b[b[k].parent].nkids++] = k;
	return GX_OK;
}

/*
 * The downward pass for bucket k: its variable's marginal, and the message
 * from it to each kid.
 *
 * Bucket k's cluster is its variable plus its up message's scope; every
 * factor in the bucket lives inside it.  The message to kid i is the
 * product of everything in the bucket except kid i's own message, so it is
 * built from a running prefix (base factors, the down message, kids
 * 0..i-1) and a stored suffix (kids i+1..), which makes a bucket linear in
 * its number of kids instead of quadratic; a hub with many kids is the
 * common first-order shape.
 */
static int
solve_down_bucket(solver *sv, int k, double *marg)
{
	ctx		   *c = sv->c;
	bucket	   *b = sv->b;
	const int  *card = sv->card;
	int			n = 0,
				x = sv->order[k],
				nk = b[k].nkids,
				nc = b[k].up.nv + 1;
	factor		acc = {0},
				m = {0},
				tmp;
	factor	   *suf = NULL;
	factor	   *two[2];
	double		ls,
				s = 0.0;
	size_t		held = 0;

	interrupt(c);
	memcpy(sv->cvars, b[k].up.vars, sizeof(int) * (size_t) b[k].up.nv);
	sv->cvars[nc - 1] = x;

	for (int i = 0; i < b[k].nbase; i++)
		sv->work[n++] = b[k].base[i];
	if (b[k].parent >= 0)
		sv->work[n++] = &b[k].down;
	if (product_project(c, card, sv->work, n, sv->cvars, nc, 0, &acc, NULL, &ls,
						sv->val, sv->inscope) != GX_OK)
		goto done;

	/* Suffix products of the kids' up messages, last kid first. */
	suf = xalloc(c, sizeof(factor) * (size_t) (nk ? nk : 1));
	if (suf)
		memset(suf, 0, sizeof(factor) * (size_t) (nk ? nk : 1));
	if (c->err)
		goto done;
	for (int i = nk - 1; i >= 0; i--)
	{
		two[0] = &b[b[k].kids[i]].up;
		two[1] = &suf[i + 1 < nk ? i + 1 : i];
		interrupt(c);
		if (product_project(c, card, two, i + 1 < nk ? 2 : 1, sv->cvars, nc, 0,
							&suf[i], NULL, &ls, sv->val, sv->inscope) != GX_OK)
			goto done;
		held += suf[i].ncells;
		if (sv->used + held > c->env->max_total_cells)
		{
			c->err = GX_ETOOBIG;
			goto done;
		}
	}

	/* The bucket variable's marginal: everything, onto x. */
	two[0] = &acc;
	two[1] = &suf[0];
	if (product_project(c, card, two, nk ? 2 : 1, &x, 1, 0, &m, NULL, &ls,
						sv->val, sv->inscope) != GX_OK)
		goto done;
	for (int v = 0; v < card[x]; v++)
		s += exp(m.vals[v]);
	if (!(s > 0.0))
	{
		c->err = GX_EZERO;
		goto done;
	}
	for (int v = 0; v < card[x]; v++)
		marg[v] = exp(m.vals[v]) / s;

	/* Each kid's message: prefix times suffix, then fold the kid in. */
	for (int i = 0; i < nk; i++)
	{
		int			kid = b[k].kids[i];

		interrupt(c);
		two[0] = &acc;
		two[1] = &suf[i + 1 < nk ? i + 1 : i];
		if (product_project(c, card, two, i + 1 < nk ? 2 : 1,
							b[kid].up.vars, b[kid].up.nv, 0, &b[kid].down,
							NULL, &ls, sv->val, sv->inscope) != GX_OK ||
			budget(c, &sv->used, &b[kid].down) != GX_OK)
			goto done;
		two[0] = &acc;
		two[1] = &b[kid].up;
		if (product_project(c, card, two, 2, sv->cvars, nc, 0, &tmp, NULL, &ls,
							sv->val, sv->inscope) != GX_OK)
			goto done;
		factor_free(c, &acc);
		acc = tmp;
	}

done:
	factor_free(c, &acc);
	factor_free(c, &m);
	if (suf)
		for (int i = 0; i < nk; i++)
			factor_free(c, &suf[i]);
	xfree(c, suf);
	return c->err;
}

/*
 * MAP: the upward pass with max for sum, one back-pointer factor per
 * variable, then the traceback in reverse elimination order; each
 * back-pointer's scope holds only later-eliminated variables, which are
 * already assigned when it is read.
 */
static int
solve_map(solver *sv, int *map, double *map_prob)
{
	ctx		   *c = sv->c;
	bucket	   *b = sv->b;
	int		   *val = sv->val;
	double		logmax = 0.0;

	for (int k = 0; k < sv->nloc; k++)
	{
		int			n = 0,
					ns;
		double		ls;

		interrupt(c);
		for (int i = 0; i < b[k].nbase; i++)
			sv->work[n++] = b[k].base[i];
		for (int i = 0; i < b[k].nkids; i++)
			sv->work[n++] = &sv->maxmsg[b[k].kids[i]];
		ns = scope_minus(sv->work, n, sv->order[k], sv->vars, sv->inscope);
		if (product_project(c, sv->card, sv->work, n, sv->vars, ns, 1,
							&sv->maxmsg[k], &sv->argmax[k], &ls,
							val, sv->inscope) != GX_OK ||
			budget(c, &sv->used, &sv->maxmsg[k]) != GX_OK ||
			budget(c, &sv->used, &sv->argmax[k]) != GX_OK)
			return c->err;
		logmax += ls;
	}
	for (int v = 0; v < sv->nloc; v++)
		val[v] = 0;
	for (int k = sv->nloc - 1; k >= 0; k--)
		val[sv->order[k]] = (int) sv->argmax[k].vals[factor_index(&sv->argmax[k],
																  sv->card, val)];
	for (int v = 0; v < sv->nloc; v++)
		map[v] = val[v];
	*map_prob = exp(logmax - sv->logz);
	return GX_OK;
}

/*
 * Solve one component: its marginals into marg[t] (card[t] doubles each),
 * and its MAP assignment and that assignment's posterior probability.
 */
static int
solve(ctx *c, int nloc, const int *card, factor *fs, int nf,
	  double **marg, int *map, double *map_prob)
{
	solver		sv;

	memset(&sv, 0, sizeof(sv));
	sv.c = c;
	sv.nloc = nloc;
	sv.card = card;
	sv.fs = fs;
	sv.nf = nf;
	if (solve_init(&sv) == GX_OK &&
		minfill_order(c, nloc, fs, nf, sv.order) == GX_OK)
	{
		for (int k = 0; k < nloc; k++)
			sv.pos[sv.order[k]] = k;
		if (solve_assign(&sv) == GX_OK && solve_upward(&sv) == GX_OK)
		{
			for (int k = nloc - 1; k >= 0 && !c->err; k--)
				solve_down_bucket(&sv, k, marg[sv.order[k]]);
			if (!c->err)
				solve_map(&sv, map, map_prob);
		}
	}
	solve_free(&sv);
	return c->err;
}

/* ----------------------------------------------------------------
 * Gibbs sampling, for a component too large to solve exactly
 * ----------------------------------------------------------------
 *
 * The chain runs over the component's missing variables with the evidence
 * clamped (it is already folded into the reduced factors).  One sweep
 * visits every variable in local order and redraws it from its full
 * conditional: the sum, in logs, of every factor that mentions it, which
 * is its own CPT row and each child's CPT row, the Markov blanket.  After
 * a burn-in of samples / 10 sweeps, samples sweeps each add every
 * variable's full conditional (not its drawn value) to an accumulator;
 * the average is the Rao-Blackwellized estimate of the marginal, which
 * has lower variance than counting draws and is a proper distribution
 * after every sweep.
 *
 * GX_CHAINS chains run, one after another, from dispersed starts (all
 * first values, all last values, then uniform draws), and the answer is
 * their pooled average.  Single-site Gibbs can stay in one mode of a
 * multimodal posterior for longer than any affordable run (a hub with
 * many agreeing children, a strongly coupled grid), and then reports a
 * confident wrong marginal.  So the chains' own estimates are compared:
 * if any probability differs by more than GX_MIX_SPREAD between two
 * chains, the component is refused with GX_ENOMIX.  That catches chains
 * stuck in different modes; it cannot catch chains that are all stuck in
 * the same one, so agreement is evidence of mixing, not proof.
 *
 * Gibbs sampling is correct only when the chain can reach every world of
 * positive probability (thesis T5736-5739: constraints must not break the
 * distribution into disconnected regions).  A sufficient condition, and
 * the one checked, is that every entry of every reduced factor is
 * positive: then every world has positive probability, and every full
 * conditional puts mass on every value.  Reduction keeps only the entries
 * consistent with the evidence, so a zero the evidence rules out does not
 * count.  A component that fails the check is refused with GX_ENOTPOS,
 * not sampled.
 *
 * Randomness is xoshiro256** seeded through splitmix64, so the draws
 * depend on the seed alone, not on the C library.  The sampling decisions
 * also go through exp(), which the C standard does not require to be
 * correctly rounded, so a draw that lands within an ulp of a boundary
 * could in principle differ between C libraries; on one platform a run is
 * bit-reproducible.
 */

typedef struct xoshiro
{
	uint64_t	s[4];
} xoshiro;

static uint64_t
splitmix64(uint64_t *x)
{
	uint64_t	z = (*x += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static uint64_t
rotl(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static uint64_t
xoshiro_next(xoshiro *r)
{
	uint64_t	result = rotl(r->s[1] * 5, 7) * 9;
	uint64_t	t = r->s[1] << 17;

	r->s[2] ^= r->s[0];
	r->s[3] ^= r->s[1];
	r->s[1] ^= r->s[2];
	r->s[0] ^= r->s[3];
	r->s[2] ^= t;
	r->s[3] = rotl(r->s[3], 45);
	return result;
}

/* A uniform double in [0, 1), from the top 53 bits. */
static double
xoshiro_unit(xoshiro *r)
{
	return (double) (xoshiro_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

/*
 * The chain's seed for a component: the caller's seed mixed with the hash
 * of the component's canonical key, so equal components get the same
 * chain wherever they sit in the network and whether or not they are
 * shared, and different components get different chains.
 */
static uint64_t
gibbs_seed(uint64_t seed, uint64_t keyhash)
{
	uint64_t	x = seed ^ rotl(keyhash, 32);

	return splitmix64(&x);
}

/*
 * The state of one chain.  Every array is allocated by gibbs_init and
 * released by gibbs_free.  The blanket of local variable t is the factor
 * list bfac[boff[t]..boff[t+1]), and bstride[] holds t's stride in each of
 * those factors, so moving t by one value moves the factor's index by the
 * stride.
 */
typedef struct gchain
{
	ctx		   *c;
	int			nloc;
	const int  *card;
	factor	   *fs;
	int			nf;
	int		   *boff;			/* nloc + 1 */
	int		   *bfac;
	size_t	   *bstride;
	int		   *val;			/* the current world */
	double	   *cond;			/* one full conditional, max card entries */
	double	   *accs;			/* GX_CHAINS accumulators of total entries */
	double	   *acc;			/* the running chain's, inside accs */
	size_t	   *aoff;			/* per variable, into acc */
	size_t		total;			/* entries per accumulator */
	xoshiro		rng;
	size_t		work;			/* since the last interrupt */
} gchain;

static void
gibbs_free(gchain *g)
{
	ctx		   *c = g->c;

	xfree(c, g->boff);
	xfree(c, g->bfac);
	xfree(c, g->bstride);
	xfree(c, g->val);
	xfree(c, g->cond);
	xfree(c, g->accs);
	xfree(c, g->aoff);
}

/*
 * Check positivity, then build the blanket lists with a counting sort over
 * the factors' variables, and one accumulator per chain.  Everything here
 * is linear in the factors' scopes, and is charged against
 * max_total_cells on top of the reduced factors the component already
 * holds.
 */
static int
gibbs_init(gchain *g)
{
	ctx		   *c = g->c;
	size_t		nent = 0,
				total = 0;
	int			maxcard = 1;

	for (int i = 0; i < g->nf; i++)
	{
		interrupt(c);
		for (size_t k = 0; k < g->fs[i].ncells; k++)
			if (!(g->fs[i].vals[k] > -INFINITY))
			{
				c->err = GX_ENOTPOS;
				return c->err;
			}
		nent += (size_t) g->fs[i].nv;
	}
	for (int t = 0; t < g->nloc; t++)
	{
		total += (size_t) g->card[t];
		if (g->card[t] > maxcard)
			maxcard = g->card[t];
	}
	if (c->held + 2 * nent + (GX_CHAINS + 1) * total + 4 * (size_t) g->nloc >
		c->env->max_total_cells)
	{
		c->err = GX_ETOOBIG;
		return c->err;
	}

	g->boff = xalloc(c, sizeof(int) * (size_t) (g->nloc + 1));
	g->bfac = xalloc(c, sizeof(int) * (nent ? nent : 1));
	g->bstride = xalloc(c, sizeof(size_t) * (nent ? nent : 1));
	g->val = xalloc(c, sizeof(int) * (size_t) g->nloc);
	g->cond = xalloc(c, sizeof(double) * (size_t) maxcard);
	g->accs = xalloc(c, sizeof(double) * total * GX_CHAINS);
	g->aoff = xalloc(c, sizeof(size_t) * (size_t) g->nloc);
	if (c->err)
		return c->err;

	/* Count each variable's factors, then place them. */
	memset(g->boff, 0, sizeof(int) * (size_t) (g->nloc + 1));
	for (int i = 0; i < g->nf; i++)
		for (int j = 0; j < g->fs[i].nv; j++)
			g->boff[g->fs[i].vars[j] + 1]++;
	for (int t = 0; t < g->nloc; t++)
		g->boff[t + 1] += g->boff[t];
	for (int t = 0; t < g->nloc; t++)
		g->val[t] = g->boff[t];	/* placement cursor, for now */
	for (int i = 0; i < g->nf; i++)
	{
		size_t		stride = 1;

		for (int j = g->fs[i].nv - 1; j >= 0; j--)
		{
			int			t = g->fs[i].vars[j];

			g->bfac[g->val[t]] = i;
			g->bstride[g->val[t]++] = stride;
			stride *= (size_t) g->card[t];
		}
	}

	total = 0;
	for (int t = 0; t < g->nloc; t++)
	{
		g->aoff[t] = total;
		total += (size_t) g->card[t];
	}
	g->total = total;
	memset(g->accs, 0, sizeof(double) * total * GX_CHAINS);
	return GX_OK;
}

/*
 * Redraw variable t from its full conditional, and add the conditional to
 * t's accumulator when keep is set.  Each blanket factor's index is found
 * once with t at 0; each value of t then adds its stride.
 */
static void
gibbs_step(gchain *g, int t, int keep)
{
	int			k = g->card[t];
	double		mx = -INFINITY,
				sum = 0.0,
				u;
	int			x;

	for (x = 0; x < k; x++)
		g->cond[x] = 0.0;
	g->val[t] = 0;
	for (int b = g->boff[t]; b < g->boff[t + 1]; b++)
	{
		const factor *f = &g->fs[g->bfac[b]];
		size_t		idx = factor_index(f, g->card, g->val);

		for (x = 0; x < k; x++)
			g->cond[x] += f->vals[idx + (size_t) x * g->bstride[b]];
		g->work += (size_t) f->nv + (size_t) k;
	}
	for (x = 0; x < k; x++)
		if (g->cond[x] > mx)
			mx = g->cond[x];
	for (x = 0; x < k; x++)
	{
		g->cond[x] = exp(g->cond[x] - mx);
		sum += g->cond[x];
	}

	/* Inverse CDF; the last value takes whatever rounding leaves over. */
	u = xoshiro_unit(&g->rng) * sum;
	for (x = 0; x < k - 1; x++)
	{
		if (u < g->cond[x])
			break;
		u -= g->cond[x];
	}
	g->val[t] = x;
	if (keep)
		for (x = 0; x < k; x++)
			g->acc[g->aoff[t] + (size_t) x] += g->cond[x] / sum;
	if (g->work >= 65536)
	{
		interrupt(g->c);
		g->work = 0;
	}
}

/*
 * Set the starting world of chain k.  The first two chains start at
 * opposite corners of the state space (every variable at its first value,
 * then at its last), the others at worlds drawn uniformly, so that chains
 * stuck in different modes disagree and gibbs_pool() can see it.
 */
static void
gibbs_start(gchain *g, int k)
{
	for (int t = 0; t < g->nloc; t++)
	{
		if (k == 0)
			g->val[t] = 0;
		else if (k == 1)
			g->val[t] = g->card[t] - 1;
		else
			g->val[t] = (int) (xoshiro_unit(&g->rng) * g->card[t]);
	}
}

/*
 * Pool the chains: write each variable's marginal (the sum of all chains'
 * accumulators, normalized) into marg and its argmax into map, and return
 * the convergence diagnostic, the largest spread (highest minus lowest)
 * between the chains' own estimates of any one probability.
 */
static double
gibbs_pool(gchain *g, double *marg, int *map)
{
	double		spread = 0.0;

	for (int t = 0; t < g->nloc; t++)
	{
		double		s = 0.0,
					sk[GX_CHAINS] = {0};

		for (int k = 0; k < GX_CHAINS; k++)
			for (int x = 0; x < g->card[t]; x++)
				sk[k] += g->accs[k * g->total + g->aoff[t] + (size_t) x];
		for (int k = 0; k < GX_CHAINS; k++)
			s += sk[k];
		map[t] = 0;
		for (int x = 0; x < g->card[t]; x++)
		{
			size_t		i = g->aoff[t] + (size_t) x;
			double		lo = 1.0,
						hi = 0.0,
						a = 0.0;

			for (int k = 0; k < GX_CHAINS; k++)
			{
				double		e = g->accs[k * g->total + i] / sk[k];

				lo = e < lo ? e : lo;
				hi = e > hi ? e : hi;
				a += g->accs[k * g->total + i];
			}
			marg[i] = a / s;
			if (marg[i] > marg[g->aoff[t] + (size_t) map[t]])
				map[t] = x;
			if (hi - lo > spread)
				spread = hi - lo;
		}
	}
	return spread;
}

/*
 * Sample one component: nloc variables with cardinalities card[], reduced
 * factors fs[0..nf).  Runs GX_CHAINS chains in turn, each with a burn-in
 * of env->samples / 10 sweeps and then env->samples sweeps.  Writes the
 * pooled marginals, flattened in local order, into marg, each variable's
 * most probable pooled value into map, and the chains' disagreement into
 * c->spread.  Returns GX_ENOTPOS if a factor has a zero entry, GX_ETOOBIG
 * if the chains' arrays would exceed max_total_cells, and GX_ENOMIX if
 * the disagreement exceeds GX_MIX_SPREAD.
 */
static int
gibbs(ctx *c, int nloc, const int *card, factor *fs, int nf, uint64_t seed,
	  double *marg, int *map)
{
	gchain		g;
	int			burnin = c->env->samples / 10;

	memset(&g, 0, sizeof(g));
	g.c = c;
	g.nloc = nloc;
	g.card = card;
	g.fs = fs;
	g.nf = nf;

	if (gibbs_init(&g) == GX_OK)
	{
		/* Each chain: its own stream, start, burn-in and accumulator. */
		for (int k = 0; k < GX_CHAINS; k++)
		{
			for (int i = 0; i < 4; i++)
				g.rng.s[i] = splitmix64(&seed);
			g.acc = g.accs + (size_t) k * g.total;
			gibbs_start(&g, k);
			for (int sweep = 0; sweep < burnin + c->env->samples; sweep++)
			{
				interrupt(c);
				for (int t = 0; t < nloc; t++)
					gibbs_step(&g, t, sweep >= burnin);
			}
		}

		c->spread = gibbs_pool(&g, marg, map);
		if (!(c->spread <= GX_MIX_SPREAD))
			c->err = GX_ENOMIX;
	}
	gibbs_free(&g);
	return c->err;
}

/* ----------------------------------------------------------------
 * Components, reduction by evidence, and the sharing cache
 * ----------------------------------------------------------------
 */

static int
uf_find(int *parent, int x)
{
	while (parent[x] != x)
	{
		parent[x] = parent[parent[x]];
		x = parent[x];
	}
	return x;
}

/*
 * Reduce CPT cpt by the evidence into a factor over its missing variables,
 * in local indices (loc[] maps global to local).  The factor's variables
 * keep the CPT's order: parents, then the child.
 */
static int
reduce_cpt(ctx *c, const gx_cpt *cpt, const int *loc, const int *lcard,
		   factor *f, int *gval)
{
	const gx_bn *bn = c->bn;
	int			nall = cpt->nparents + 1;
	int		   *vars,
			   *globals;
	int			nv = 0;

	vars = xalloc(c, sizeof(int) * (size_t) nall);
	globals = xalloc(c, sizeof(int) * (size_t) nall);
	if (c->err)
		goto done;
	for (int j = 0; j < nall; j++)
	{
		int			g = j < cpt->nparents ? cpt->parents[j] : cpt->child;

		gval[g] = bn->evidence[g] >= 0 ? bn->evidence[g] : 0;
		if (bn->evidence[g] < 0)
		{
			globals[nv] = g;
			vars[nv++] = loc[g];
		}
	}
	if (factor_init(c, f, lcard, vars, nv) != GX_OK)
		goto done;

	/* Walk the factor's cells; observed variables stay at their values. */
	for (size_t cell = 0; cell < f->ncells; cell++)
	{
		size_t		idx = 0;

		for (int j = 0; j < nall; j++)
		{
			int			g = j < cpt->nparents ? cpt->parents[j] : cpt->child;

			idx = idx * (size_t) bn->card[g] + (size_t) gval[g];
		}
		f->vals[cell] = log(cpt->vals[idx]);
		for (int i = nv - 1; i >= 0; i--)
		{
			if (++gval[globals[i]] < bn->card[globals[i]])
				break;
			gval[globals[i]] = 0;
		}
	}
done:
	xfree(c, vars);
	xfree(c, globals);
	return c->err;
}

/* Value of a CPT whose variables are all observed. */
static double
observed_value(const gx_bn *bn, const gx_cpt *cpt)
{
	size_t		idx = 0;

	for (int j = 0; j < cpt->nparents; j++)
		idx = idx * (size_t) bn->card[cpt->parents[j]] +
			(size_t) bn->evidence[cpt->parents[j]];
	idx = idx * (size_t) bn->card[cpt->child] + (size_t) bn->evidence[cpt->child];
	return cpt->vals[idx];
}

/* Serialize a component canonically: nloc, cards, then sorted factors. */
typedef struct keybuf
{
	unsigned char *b;
	size_t		len;
	size_t		cap;
} keybuf;

static void
key_put(ctx *c, keybuf *k, const void *p, size_t n)
{
	if (c->err)
		return;
	if (k->len + n > k->cap)
	{
		size_t		ncap = (k->cap ? k->cap * 2 : 256);
		unsigned char *nb;

		while (ncap < k->len + n)
			ncap *= 2;
		nb = xalloc(c, ncap);
		if (c->err)
			return;
		if (k->len)
			memcpy(nb, k->b, k->len);
		xfree(c, k->b);
		k->b = nb;
		k->cap = ncap;
	}
	memcpy(k->b + k->len, p, n);
	k->len += n;
}

static int
factor_cmp(const factor *a, const factor *b)
{
	if (a->nv != b->nv)
		return a->nv < b->nv ? -1 : 1;
	for (int i = 0; i < a->nv; i++)
		if (a->vars[i] != b->vars[i])
			return a->vars[i] < b->vars[i] ? -1 : 1;
	if (a->ncells != b->ncells)
		return a->ncells < b->ncells ? -1 : 1;
	return memcmp(a->vals, b->vals, sizeof(double) * a->ncells);
}

static int
factor_qcmp(const void *a, const void *b)
{
	return factor_cmp((const factor *) a, (const factor *) b);
}

/*
 * Sort a component's factors into canonical order for the cache key.  A
 * hub component has as many factors as rows, so this must not be
 * quadratic.  qsort is not stable, but factor_cmp is a total order on
 * distinct factors, and equal factors are interchangeable in the key.
 */
static void
factors_sort(factor *fs, int nf)
{
	if (nf > 1)
		qsort(fs, (size_t) nf, sizeof(factor), factor_qcmp);
}

static uint64_t
hash_bytes(ctx *c, const unsigned char *b, size_t n)
{
	uint64_t	h = 0xcbf29ce484222325ULL;	/* FNV-1a */

	for (size_t i = 0; i < n; i++)
	{
		if ((i & 0xfffff) == 0xfffff)
			interrupt(c);
		h ^= b[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

typedef struct centry
{
	struct centry *next;
	uint64_t	hash;
	unsigned char *key;
	size_t		keylen;
	int			nloc;
	double	   *marg;			/* flattened per local variable */
	int		   *map;
	double		map_prob;
	int			samples;		/* Gibbs samples, 0 if solved exactly */
} centry;

#define NBUCKETS 1024

static void
cache_free(ctx *c, centry **buckets)
{
	for (int b = 0; b < NBUCKETS; b++)
	{
		centry	   *e = buckets[b];

		while (e)
		{
			centry	   *next = e->next;

			xfree(c, e->key);
			xfree(c, e->marg);
			xfree(c, e->map);
			xfree(c, e);
			e = next;
		}
	}
}

/*
 * The canonical key of a component: its local cardinalities, then its
 * factors in sorted order.  Equal keys mean equal components.
 */
static int
build_key(ctx *c, int nloc, const int *lcard, factor *fs, int nf, keybuf *key)
{
	factors_sort(fs, nf);
	key_put(c, key, &nloc, sizeof(nloc));
	key_put(c, key, lcard, sizeof(int) * (size_t) nloc);
	for (int i = 0; i < nf; i++)
	{
		interrupt(c);
		key_put(c, key, &fs[i].nv, sizeof(int));
		key_put(c, key, fs[i].vars, sizeof(int) * (size_t) fs[i].nv);
		key_put(c, key, fs[i].vals, sizeof(double) * fs[i].ncells);
	}
	return c->err;
}

/* Look a key up in the cache.  memcmp is bounded like the key itself. */
static centry *
cache_find(ctx *c, centry **buckets, uint64_t h, const keybuf *key)
{
	for (centry *e = buckets[h % NBUCKETS]; e; e = e->next)
	{
		interrupt(c);
		if (e->hash == h && e->keylen == key->len &&
			memcmp(e->key, key->b, key->len) == 0)
			return e;
	}
	return NULL;
}

/*
 * Solve a component that missed the cache, and return a new cache entry
 * for it (taking ownership of the key), or NULL on error.  h is the
 * bucket hash and keyhash the key's real hash, which seeds a Gibbs chain
 * (the two differ only in the hash-collision test build).
 */
static centry *
cache_fill(ctx *c, int nloc, const int *lcard, factor *fs, int nf,
		   uint64_t h, uint64_t keyhash, keybuf *key)
{
	size_t		total = 0;
	double	  **marg;
	centry	   *e;

	for (int t = 0; t < nloc; t++)
		total += (size_t) lcard[t];
	e = xalloc(c, sizeof(centry));
	if (e)
		memset(e, 0, sizeof(*e));
	marg = xalloc(c, sizeof(double *) * (size_t) nloc);
	if (e)
	{
		e->marg = xalloc(c, sizeof(double) * total);
		e->map = xalloc(c, sizeof(int) * (size_t) nloc);
	}
	if (!c->err)
	{
		const gx_env *env = c->env;

		total = 0;
		for (int t = 0; t < nloc; t++)
		{
			marg[t] = e->marg + total;
			total += (size_t) lcard[t];
		}

		/*
		 * Exact first.  solve() frees everything it built when a bound
		 * stops it, so the component can then be sampled from a clean
		 * slate; the work exact did before giving up is bounded by the
		 * same limits.
		 */
		if (!(env->sample_always && env->samples > 0))
			solve(c, nloc, lcard, fs, nf, marg, e->map, &e->map_prob);
		if (env->samples > 0 &&
			(env->sample_always || c->err == GX_ETOOBIG))
		{
			c->err = GX_OK;
			if (gibbs(c, nloc, lcard, fs, nf, gibbs_seed(env->seed, keyhash),
					  e->marg, e->map) == GX_OK)
			{
				e->map_prob = NAN;
				e->samples = env->samples;
			}
		}
	}
	xfree(c, marg);
	if (c->err)
	{
		if (e)
		{
			xfree(c, e->marg);
			xfree(c, e->map);
		}
		xfree(c, e);
		return NULL;
	}
	e->nloc = nloc;
	e->hash = h;
	e->key = key->b;
	e->keylen = key->len;
	key->b = NULL;
	return e;
}

/*
 * Solve component cid, whose missing variables are members[0..nloc) in
 * increasing global order and whose CPTs are cptlist[0..ncpt): build its
 * factors, consult the cache, and fill the result arrays.
 */
static int
do_component(ctx *c, int cid, const int *members, int nloc,
			 const int *cptlist, int ncpt,
			 int *loc, int *gval, centry **buckets, gx_result *out)
{
	const gx_bn *bn = c->bn;
	int		   *lcard;
	factor	   *fs;
	int			nf = 0;
	keybuf		key = {0};
	uint64_t	h,
				keyhash;
	centry	   *e;
	size_t		held = 0;
	size_t		off = 0;

	lcard = xalloc(c, sizeof(int) * (size_t) nloc);
	fs = xalloc(c, sizeof(factor) * (size_t) (ncpt ? ncpt : 1));
	if (c->err)
		goto done;
	for (int t = 0; t < nloc; t++)
	{
		loc[members[t]] = t;
		lcard[t] = bn->card[members[t]];
	}

	/*
	 * The reduced factors and the cache key (which copies them) are held for
	 * the whole component, so both count against max_total_cells, along with
	 * everything solve() holds.
	 */
	for (int i = 0; i < ncpt; i++)
	{
		interrupt(c);
		if (reduce_cpt(c, &bn->cpts[cptlist[i]], loc, lcard, &fs[nf], gval) != GX_OK)
			goto done;
		held += 2 * fs[nf].ncells + (size_t) fs[nf].nv;
		nf++;
		if (held > c->env->max_total_cells)
		{
			c->err = GX_ETOOBIG;
			goto done;
		}
	}
	c->held = held;

	if (build_key(c, nloc, lcard, fs, nf, &key) != GX_OK)
		goto done;
	keyhash = h = hash_bytes(c, key.b, key.len);
#ifdef GX_TEST_HASH_COLLIDE
	h = 0;						/* test build: every key collides */
#endif
	e = c->env->no_sharing ? NULL : cache_find(c, buckets, h, &key);
	if (e)
		out->cache_hits++;
	else
	{
		e = cache_fill(c, nloc, lcard, fs, nf, h, keyhash, &key);
		if (e == NULL)
			goto done;
		e->next = buckets[h % NBUCKETS];
		buckets[h % NBUCKETS] = e;
	}

	/* Copy the (possibly shared) answer out to the component's variables. */
	for (int t = 0; t < nloc; t++)
	{
		int			g = members[t];

		memcpy(out->marg + out->marg_off[g], e->marg + off,
			   sizeof(double) * (size_t) lcard[t]);
		off += (size_t) lcard[t];
		out->map[g] = e->map[t];
	}
	out->comp_map_prob[cid] = e->map_prob;
	out->comp_samples[cid] = e->samples;

done:
	if (fs)
		for (int i = 0; i < nf; i++)
			factor_free(c, &fs[i]);
	xfree(c, fs);
	xfree(c, lcard);
	xfree(c, key.b);
	return c->err;
}

static void
result_free(ctx *c, gx_result *out)
{
	xfree(c, out->marg);
	xfree(c, out->marg_off);
	xfree(c, out->comp);
	xfree(c, out->map);
	xfree(c, out->comp_map_prob);
	xfree(c, out->comp_size);
	xfree(c, out->comp_samples);
	out->comp_samples = NULL;
	out->marg = NULL;
	out->marg_off = NULL;
	out->comp = out->map = out->comp_size = NULL;
	out->comp_map_prob = NULL;
}

/*
 * Working arrays of gx_infer, allocated together and freed together.
 */
typedef struct inferwork
{
	int		   *uf;				/* union-find parent per variable */
	int		   *cursor;			/* per-component scratch */
	int		   *cptcomp;		/* component of each CPT, or -1 */
	int		   *members;		/* missing variables grouped by component */
	int		   *start;			/* members of k: members[start[k]..start[k+1]) */
	int		   *cptsorted;		/* CPTs grouped by component */
	int		   *cstart;			/* CPTs of k: cptsorted[cstart[k]..cstart[k+1]) */
	int		   *loc;			/* global to local index, per component */
	int		   *gval;			/* reduce_cpt scratch */
	centry	  **buckets;		/* the sharing cache */
} inferwork;

/*
 * Step 1: union the missing variables of each CPT into components, and
 * number the components in order of their smallest variable.  A CPT with
 * no missing variable is a constant; a zero there means the evidence is
 * impossible.
 */
static int
find_components(ctx *c, inferwork *w, gx_result *out)
{
	const gx_bn *bn = c->bn;

	for (int v = 0; v < bn->nvars; v++)
		w->uf[v] = v;
	for (int i = 0; i < bn->ncpts; i++)
	{
		const gx_cpt *cpt = &bn->cpts[i];
		int			first = bn->evidence[cpt->child] < 0 ? cpt->child : -1;

		for (int j = 0; j < cpt->nparents; j++)
		{
			int			p = cpt->parents[j];

			if (bn->evidence[p] >= 0)
				continue;
			if (first < 0)
				first = p;
			else
				w->uf[uf_find(w->uf, p)] = uf_find(w->uf, first);
		}
		if (first < 0 && observed_value(bn, cpt) == 0.0)
		{
			out->bad_var = cpt->child;
			c->err = GX_EZERO;
			return c->err;
		}
	}

	for (int v = 0; v < bn->nvars; v++)
		w->cursor[v] = -1;
	for (int v = 0; v < bn->nvars; v++)
	{
		int			r;

		if (bn->evidence[v] >= 0)
			continue;
		r = uf_find(w->uf, v);
		if (w->cursor[r] < 0)
			w->cursor[r] = out->ncomp++;
		out->comp[v] = w->cursor[r];
	}
	for (int i = 0; i < bn->ncpts; i++)
	{
		const gx_cpt *cpt = &bn->cpts[i];
		int			m = bn->evidence[cpt->child] < 0 ? cpt->child : -1;

		for (int j = 0; m < 0 && j < cpt->nparents; j++)
			if (bn->evidence[cpt->parents[j]] < 0)
				m = cpt->parents[j];
		w->cptcomp[i] = m < 0 ? -1 : out->comp[m];
	}
	return GX_OK;
}

/*
 * Group the missing variables, and the CPTs, by component, each with a
 * counting sort, so each component sees only its own; scanning everything
 * per component is quadratic in the number of independent rows, the
 * common case.  Variables stay in increasing global order within a group.
 */
static int
group_components(ctx *c, inferwork *w, gx_result *out)
{
	const gx_bn *bn = c->bn;
	int			nc = out->ncomp;

	out->comp_size = xalloc(c, sizeof(int) * (size_t) (nc ? nc : 1));
	out->comp_map_prob = xalloc(c, sizeof(double) * (size_t) (nc ? nc : 1));
	out->comp_samples = xalloc(c, sizeof(int) * (size_t) (nc ? nc : 1));
	w->cstart = xalloc(c, sizeof(int) * (size_t) (nc + 1));
	w->cptsorted = xalloc(c, sizeof(int) * (size_t) (bn->ncpts ? bn->ncpts : 1));
	if (c->err)
		return c->err;

	for (int k = 0; k <= nc; k++)
		w->start[k] = w->cstart[k] = 0;
	for (int v = 0; v < bn->nvars; v++)
		if (out->comp[v] >= 0)
			w->start[out->comp[v] + 1]++;
	for (int i = 0; i < bn->ncpts; i++)
		if (w->cptcomp[i] >= 0)
			w->cstart[w->cptcomp[i] + 1]++;
	for (int k = 0; k < nc; k++)
	{
		out->comp_size[k] = w->start[k + 1];
		w->start[k + 1] += w->start[k];
		w->cstart[k + 1] += w->cstart[k];
	}

	for (int k = 0; k < nc; k++)
		w->cursor[k] = w->start[k];
	for (int v = 0; v < bn->nvars; v++)
		if (out->comp[v] >= 0)
			w->members[w->cursor[out->comp[v]]++] = v;
	for (int k = 0; k < nc; k++)
		w->cursor[k] = w->cstart[k];
	for (int i = 0; i < bn->ncpts; i++)
		if (w->cptcomp[i] >= 0)
			w->cptsorted[w->cursor[w->cptcomp[i]]++] = i;
	return GX_OK;
}

static int
result_init(ctx *c, gx_result *out)
{
	const gx_bn *bn = c->bn;
	size_t		nv = (size_t) (bn->nvars ? bn->nvars : 1);
	size_t		total = 0;

	out->comp = xalloc(c, sizeof(int) * nv);
	out->map = xalloc(c, sizeof(int) * nv);
	out->marg_off = xalloc(c, sizeof(size_t) * nv);
	if (c->err)
		return c->err;
	for (int v = 0; v < bn->nvars; v++)
	{
		out->comp[v] = -1;
		out->map[v] = -1;
		out->marg_off[v] = total;
		if (bn->evidence[v] < 0)
			total += (size_t) bn->card[v];
	}
	out->marg = xalloc(c, sizeof(double) * (total ? total : 1));
	return c->err;
}

int
gx_infer(const gx_env *env, const gx_bn *bn, gx_result *out)
{
	ctx			c = {env, bn, GX_OK, 0, 0, 0.0};
	size_t		nv = (size_t) (bn->nvars ? bn->nvars : 1);
	inferwork	w;

	memset(out, 0, sizeof(*out));
	memset(&w, 0, sizeof(w));
	out->bad_var = -1;
	w.uf = xalloc(&c, sizeof(int) * nv);
	w.cursor = xalloc(&c, sizeof(int) * nv);
	w.cptcomp = xalloc(&c, sizeof(int) * (size_t) (bn->ncpts ? bn->ncpts : 1));
	w.members = xalloc(&c, sizeof(int) * nv);
	w.start = xalloc(&c, sizeof(int) * (nv + 1));
	w.loc = xalloc(&c, sizeof(int) * nv);
	w.gval = xalloc(&c, sizeof(int) * nv);
	w.buckets = xalloc(&c, sizeof(centry *) * NBUCKETS);
	if (w.buckets)
		memset(w.buckets, 0, sizeof(centry *) * NBUCKETS);

	/* Steps 2-4 (doc at the top of this file), one component at a time. */
	if (!c.err && result_init(&c, out) == GX_OK &&
		find_components(&c, &w, out) == GX_OK &&
		group_components(&c, &w, out) == GX_OK)
	{
		for (int k = 0; k < out->ncomp; k++)
		{
			interrupt(&c);
			if (do_component(&c, k, w.members + w.start[k], out->comp_size[k],
							 w.cptsorted + w.cstart[k], w.cstart[k + 1] - w.cstart[k],
							 w.loc, w.gval, w.buckets, out) != GX_OK)
			{
				out->bad_var = w.members[w.start[k]];
				out->bad_size = out->comp_size[k];
				out->spread = c.spread;
				break;
			}
		}
	}

	out->peak_cells = c.peak;
	if (w.buckets)
		cache_free(&c, w.buckets);
	xfree(&c, w.uf);
	xfree(&c, w.cursor);
	xfree(&c, w.cptcomp);
	xfree(&c, w.members);
	xfree(&c, w.start);
	xfree(&c, w.loc);
	xfree(&c, w.gval);
	xfree(&c, w.buckets);
	xfree(&c, w.cstart);
	xfree(&c, w.cptsorted);
	if (c.err)
	{
		int			bad = out->bad_var;

		result_free(&c, out);
		out->bad_var = bad;
	}
	return c.err;
}
