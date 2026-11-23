/*-------------------------------------------------------------------------
 *
 * gx_param.c
 *	  The CPTs of one ground instance of a parameterised factor (gx_param.h).
 *
 * Three steps per instance, after gx_param_size has checked every bound:
 *
 *	1. Fold the observed parents into the parameters, in logs: for
 *	   logistic, add their weights to each class's score; for noisy-OR, add
 *	   their log(1 - p) to the log of the probability that the child is
 *	   off.  Both sums are compensated (Neumaier), so a thousand equal
 *	   terms sum to within a few units in the last place.
 *	2. Build the auxiliary chains over the missing members of aggregating
 *	   slots (the OR chain for noisy-OR, a count chain per slot for
 *	   logistic).
 *	3. Build the child's CPT over its missing non-aggregating parents and
 *	   the chains' last variables, keeping the folded part, which is the
 *	   same in every row, in the column offsets (lscale), and then shifting
 *	   each column so that its largest entry is 0.
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include "engine/gx_param.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* What one call needs besides its arguments. */
typedef struct pctx
{
	const gx_env *env;
	const gx_pfactor *f;
	const gx_pinst *in;
	gx_pcpt    *out;
	int			nout;			/* CPTs built so far */
	int			err;
} pctx;

/* log(DBL_MIN): a probability below it is subnormal. */
#define LOG_DBL_MIN (-708.39641853226410622)

/*
 * A compensated (Neumaier) sum.  A term of -INFINITY (log 0) makes the sum
 * -INFINITY for good, where the compensation would turn it into NaN.
 */
typedef struct csum
{
	double		s;
	double		c;
} csum;

static void
csum_add(csum *a, double x)
{
	double		t = a->s + x;

	if (!isfinite(t))
	{
		a->s = t;
		a->c = 0.0;
		return;
	}
	if (fabs(a->s) >= fabs(x))
		a->c += (a->s - t) + x;
	else
		a->c += (x - t) + a->s;
	a->s = t;
}

static double
csum_get(const csum *a)
{
	return isfinite(a->s) ? a->s + a->c : a->s;
}

/*
 * log(1 - exp(x)) for x <= 0, accurate at both ends (Maechler's
 * log1mexp): -INFINITY at x = 0 and 0 at x = -INFINITY.
 */
static double
log1mexp(double x)
{
	return x > -0.693147180559945309417 ? log(-expm1(x)) : log1p(-exp(x));
}

static void *
pa_alloc(pctx *c, size_t size)
{
	void	   *p;

	if (c->err)
		return NULL;
	p = c->env->alloc(c->env->arg, size ? size : 1);
	if (p == NULL)
		c->err = GX_ENOMEM;
	return p;
}

/*
 * Start CPT number c->nout for child, with nparents parents and a table of
 * cells = (product of the parents' cardinalities) x card cells, which
 * gx_param_size has checked against the bounds, and card log offsets, all
 * 0.  Returns it, or NULL with c->err set.
 */
static gx_pcpt *
new_cpt(pctx *c, int child, int card, int nparents, size_t cells)
{
	gx_pcpt    *t = &c->out[c->nout];

	if (c->err)
		return NULL;
	t->child = child;
	t->card = card;
	t->nparents = nparents;
	t->ncells = cells;
	t->parents = pa_alloc(c, sizeof(int) * (size_t) (nparents ? nparents : 1));
	t->vals = pa_alloc(c, sizeof(double) * cells);
	t->lscale = pa_alloc(c, sizeof(double) * (size_t) card);
	if (c->err)
	{
		if (t->parents)
			c->env->free(c->env->arg, t->parents);
		if (t->vals)
			c->env->free(c->env->arg, t->vals);
		if (t->lscale)
			c->env->free(c->env->arg, t->lscale);
		return NULL;
	}
	for (int x = 0; x < card; x++)
		t->lscale[x] = 0.0;
	c->nout++;
	return t;
}

/*
 * Shift each column x of the child's table t so that its largest entry is
 * 0, moving the shift into t->lscale[x].  A column of zeros (-INFINITY
 * everywhere) is left as it is.
 */
static void
shift_columns(gx_pcpt *t)
{
	size_t		k = (size_t) t->card;
	size_t		rows = t->ncells / k;

	for (size_t x = 0; x < k; x++)
	{
		double		mx = -INFINITY;

		for (size_t r = 0; r < rows; r++)
			if (t->vals[r * k + x] > mx)
				mx = t->vals[r * k + x];
		if (mx == -INFINITY)
			continue;
		for (size_t r = 0; r < rows; r++)
			t->vals[r * k + x] -= mx;
		t->lscale[x] += mx;
	}
}

/*
 * n x k, or SIZE_MAX when it exceeds limit.  Every size goes through here
 * before it is used.
 */
static size_t
mul_bounded(size_t n, size_t k, size_t limit)
{
	if (n == SIZE_MAX || k == 0 || n > limit / k)
		return SIZE_MAX;
	return n * k;
}

/*
 * The number of count vectors over v values summing to j, C(j + v - 1,
 * v - 1), or SIZE_MAX when it exceeds limit.  Each step's partial product
 * is itself a binomial coefficient, so the division is exact.
 */
static size_t
ncounts(int j, int v, size_t limit)
{
	size_t		r = 1;

	for (int i = 1; i < v; i++)
	{
		size_t		num = (size_t) j + (size_t) i;

		if (r > limit / num)
			return SIZE_MAX;
		r = r * num / (size_t) i;
		if (r > limit)
			return SIZE_MAX;
	}
	return r;
}

/*
 * The index of count vector cnt[0..v-1] (summing to j) among all such
 * vectors in lexicographic order.  The vectors with a smaller first count
 * t come first, ncounts(j - t, v - 1) of them for each t, and so on.
 */
static size_t
count_rank(const int *cnt, int v, int j)
{
	size_t		r = 0;
	int			rem = j;

	for (int i = 0; i < v - 1; i++)
	{
		for (int t = 0; t < cnt[i]; t++)
			r += ncounts(rem - t, v - i - 1, SIZE_MAX - 1);
		rem -= cnt[i];
	}
	return r;
}

/*
 * Advance cnt[0..v-1] to the next count vector with the same sum in
 * count_rank's order: find the rightmost position before the end with some
 * count after it, increment it, and put the rest of those counts, less
 * one, in the last position.  (0, 0, 2) -> (0, 1, 1) -> (0, 2, 0) ->
 * (1, 0, 1) -> (1, 1, 0) -> (2, 0, 0).
 */
static void
count_next(int *cnt, int v)
{
	int			tail = 0;

	for (int i = v - 1; i >= 1; i--)
	{
		tail += cnt[i];
		if (tail > 0)
		{
			cnt[i - 1]++;
			for (int x = i; x < v; x++)
				cnt[x] = 0;
			cnt[v - 1] = tail - 1;
			return;
		}
	}
}

/* The weight of value x of slot s for class k (1-based, k >= 2). */
static double
weight(const gx_pfactor *f, const int *slot_off, int k, int s, int x)
{
	size_t		stride = (size_t) slot_off[f->nslots] + 1;

	return f->params[(size_t) (k - 2) * stride + 1 + (size_t) slot_off[s] + (size_t) x];
}

int
gx_param_check(const gx_pfactor *f)
{
	long		need;

	if (f->card < 1 || f->nslots < 0 || f->nparams < 0 ||
		(f->nparams > 0 && f->params == NULL) ||
		(f->nslots > 0 && (f->slot_card == NULL || f->slot_agg == NULL)))
		return GX_EINVAL;
	need = 1;
	for (int s = 0; s < f->nslots; s++)
	{
		if (f->slot_card[s] < 1 || f->slot_card[s] > INT32_MAX / 2)
			return GX_EINVAL;
		need += f->slot_card[s];
		if (need > INT32_MAX)
			return GX_EINVAL;
	}
	if (f->kind == GX_LOGISTIC)
	{
		if ((long) f->nparams != need * (long) (f->card - 1))
			return GX_EINVAL;
	}
	else if (f->kind == GX_NOISY_OR)
	{
		if (f->card != 2 || f->nparams != 1 + f->nslots)
			return GX_EINVAL;
	}
	else
		return GX_EINVAL;
	for (int i = 0; i < f->nparams; i++)
	{
		if (!isfinite(f->params[i]))
			return GX_EINVAL;
		if (f->kind == GX_NOISY_OR && (f->params[i] < 0.0 || f->params[i] > 1.0))
			return GX_EINVAL;
	}
	return GX_OK;
}

/* The number of missing members of slot s of in. */
static int
missing_in_slot(const gx_pinst *in, int s)
{
	int			n = 0;

	for (int i = in->off[s]; i < in->off[s + 1]; i++)
		if (in->value[i] < 0)
			n++;
	return n;
}

int
gx_param_naux(const gx_pfactor *f, const gx_pinst *in)
{
	int			n = 0;

	for (int s = 0; s < f->nslots; s++)
		if (f->slot_agg[s])
			n += missing_in_slot(in, s);
	return n;
}

/*
 * A chain over m missing members has m links, the first with one parent
 * (the member) and each later one two (the previous link and the
 * member).  The child's parents are its distinct missing direct parents
 * (as distinct_missing counts them) and the end of each chain: one OR
 * chain over every aggregating slot for noisy-OR, one count chain per
 * aggregating slot with a missing member for logistic.
 */
int
gx_param_nlinks(const gx_pfactor *f, const gx_pinst *in)
{
	int			links = 0;
	int			ends = 0;
	int			or_members = 0;

	for (int s = 0; s < f->nslots; s++)
	{
		int			i = in->off[s];
		int			seen = 0;
		int			m;

		if (f->slot_agg[s])
		{
			m = missing_in_slot(in, s);
			if (f->kind == GX_NOISY_OR)
				or_members += m;
			else if (m > 0)
			{
				links += 2 * m - 1;
				ends++;
			}
			continue;
		}
		if (in->value[i] >= 0)
			continue;
		for (int s2 = 0; s2 < s && !seen; s2++)
			seen = !f->slot_agg[s2] && in->value[in->off[s2]] < 0 &&
				in->var[in->off[s2]] == in->var[i];
		if (!seen)
			links++;
	}
	if (or_members > 0)
	{
		links += 2 * or_members - 1;
		ends++;
	}
	return links + ends;
}

/*
 * Check instance in against f: slot ranges in order, one member in every
 * slot that does not aggregate, values in range, and the variables of an
 * aggregating slot strictly increasing.  A variable twice in one slot
 * would be two independent parents with one value, which is no model.
 */
static int
check_inst(const gx_pfactor *f, const gx_pinst *in)
{
	if (in->off == NULL || in->off[0] != 0)
		return GX_EINVAL;
	for (int s = 0; s < f->nslots; s++)
	{
		int			n = in->off[s + 1] - in->off[s];

		if (n < 0 || (!f->slot_agg[s] && n != 1))
			return GX_EINVAL;
		for (int i = in->off[s]; i < in->off[s + 1]; i++)
		{
			if (in->value[i] < -1 || in->value[i] >= f->slot_card[s])
				return GX_EINVAL;
			if (i > in->off[s] && in->var[i] <= in->var[i - 1])
				return GX_EINVAL;
		}
	}
	return GX_OK;
}

/*
 * The product of the cardinalities of the distinct missing variables of
 * the slots that do not aggregate, times k, or SIZE_MAX when it exceeds
 * limit.  Like distinct_missing, without its arrays; slots are few.
 */
static size_t
direct_cells(const gx_pfactor *f, const gx_pinst *in, size_t k, size_t limit)
{
	size_t		cells = k;

	for (int s = 0; s < f->nslots; s++)
	{
		int			i = in->off[s];
		int			seen = 0;

		if (f->slot_agg[s] || in->value[i] >= 0)
			continue;
		for (int s2 = 0; s2 < s && !seen; s2++)
			seen = !f->slot_agg[s2] && in->value[in->off[s2]] < 0 &&
				in->var[in->off[s2]] == in->var[i];
		if (!seen)
			cells = mul_bounded(cells, (size_t) f->slot_card[s], limit);
	}
	return cells;
}

/*
 * The chains' tables of instance in: their cells (vals and lscale) are
 * added to *cells, their auxiliary variables' values to *values, and the
 * product of the cardinalities of the chain ends to *ends.  GX_ETOOBIG
 * when one table is over env->max_cells.
 */
static int
chain_size(const gx_env *env, const gx_pfactor *f, const gx_pinst *in, size_t *cells,
		   size_t *values, size_t *ends)
{
	size_t		lim = env->max_cells;
	int			links = 0;

	for (int s = 0; s < f->nslots; s++)
	{
		int			v = f->slot_card[s];
		int			m = missing_in_slot(in, s);

		if (!f->slot_agg[s] || m == 0)
			continue;
		if (f->kind == GX_NOISY_OR)
		{
			/* One chain over every slot: its first link has one parent. */
			for (int j = 0; j < m; j++)
			{
				size_t		t = mul_bounded((size_t) (links++ == 0 ? 2 : 4), (size_t) v,
											lim);

				if (t == SIZE_MAX)
					return GX_ETOOBIG;
				*cells += t + 2;
				*values += 2;
			}
			*ends = 2;
			continue;
		}
		for (int j = 0; j < m; j++)
		{
			size_t		prevn = ncounts(j, v, lim);
			size_t		nextn = ncounts(j + 1, v, lim);
			size_t		t = mul_bounded(mul_bounded(prevn, (size_t) v, lim), nextn, lim);

			if (nextn == SIZE_MAX || t == SIZE_MAX || nextn > INT32_MAX)
				return GX_ETOOBIG;
			*cells += t + nextn;
			*values += nextn;
		}
		*ends = mul_bounded(*ends, ncounts(m, v, lim), lim);
		if (*ends == SIZE_MAX)
			return GX_ETOOBIG;
	}
	return GX_OK;
}

int
gx_param_size(const gx_env *env, const gx_pfactor *f, const gx_pinst *in, size_t *cells,
			  size_t *hold)
{
	size_t		values = 0,
				ends = 1,
				child;
	int			rc;

	*cells = 0;
	*hold = SIZE_MAX;
	if (gx_param_check(f) != GX_OK || check_inst(f, in) != GX_OK)
		return GX_EINVAL;
	if ((rc = chain_size(env, f, in, cells, &values, &ends)) != GX_OK)
		return rc;
	child = mul_bounded(direct_cells(f, in, (size_t) f->card, env->max_cells), ends,
						env->max_cells);
	if (child == SIZE_MAX)
		return GX_ETOOBIG;
	*cells += child + (size_t) f->card;

	/*
	 * Each table is at most max_cells, which the GUCs keep below 2^31, and
	 * there are at most INT32_MAX of them, so none of this overflows.  An
	 * instance with no chain may be sampled instead of solved, which holds
	 * its tables and their copy in the cache key and little more.
	 */
	if (env->samples > 0 && values == 0)
		*hold = 2 * *cells;
	else
		*hold = GX_PARAM_HOLD * *cells + GX_PARAM_HOLD_VALUE * values + GX_PARAM_HOLD_BASE;
	return *hold > env->max_total_cells ? GX_ETOOBIG : GX_OK;
}

/*
 * The OR chain of a noisy-OR instance: z_j = z_(j-1) OR (y_j fires), over
 * the missing members of every aggregating slot in slot order.  A member
 * fires with its slot's probability when its value is active (not its
 * first value).  Returns the last z, or -1 for no chain.
 */
static int
or_chain(pctx *c)
{
	const gx_pfactor *f = c->f;
	const gx_pinst *in = c->in;
	int			z = -1;
	int			aux = in->first_aux;

	for (int s = 0; s < f->nslots && !c->err; s++)
	{
		double		p = f->params[1 + s];
		int			yc = f->slot_card[s];

		if (!f->slot_agg[s])
			continue;
		for (int i = in->off[s]; i < in->off[s + 1] && !c->err; i++)
		{
			int			np = z < 0 ? 1 : 2;
			size_t		cells = (size_t) np * 2 * (size_t) yc;
			gx_pcpt    *t;

			if (in->value[i] >= 0)
				continue;
			if ((t = new_cpt(c, aux, 2, np, cells)) == NULL)
				break;
			/* Parents (z_prev, y), y fastest; z_prev on forces on. */
			if (z >= 0)
				t->parents[0] = z;
			t->parents[np - 1] = in->var[i];
			for (int prev = 0; prev < np; prev++)
				for (int y = 0; y < yc; y++)
				{
					size_t		cell = ((size_t) prev * (size_t) yc + (size_t) y) * 2;

					if (np == 2 && prev == 1)
					{
						t->vals[cell] = -INFINITY;
						t->vals[cell + 1] = 0.0;
					}
					else
					{
						t->vals[cell] = y > 0 ? log1p(-p) : 0.0;
						t->vals[cell + 1] = y > 0 ? log(p) : -INFINITY;
					}
				}
			z = aux++;
		}
	}
	return z;
}

/*
 * The count chain of aggregating slot s of a logistic instance, whose
 * auxiliary variables start at *aux: c_j is the vector of the counts of
 * each value among the slot's first j missing members, as an index into
 * the vectors summing to j in lexicographic order (count_rank), and is a
 * deterministic function of c_(j-1) and y_j.  Returns the last c, or -1
 * for no chain; *ncard is its number of values.
 */
static int
count_chain(pctx *c, int s, int *aux, size_t *ncard)
{
	const gx_pinst *in = c->in;
	int			v = c->f->slot_card[s];
	int			cvar = -1;
	int			j = 0;
	int		   *cnt;

	*ncard = 1;
	cnt = pa_alloc(c, sizeof(int) * (size_t) v);
	if (cnt == NULL)
		return -1;
	for (int i = in->off[s]; i < in->off[s + 1] && !c->err; i++)
	{
		size_t		prevn = *ncard;
		size_t		nextn = ncounts(j + 1, v, c->env->max_cells);
		int			np = cvar < 0 ? 1 : 2;
		size_t		cells = prevn * (size_t) v * nextn;
		gx_pcpt    *t;

		if (in->value[i] >= 0)
			continue;
		if ((t = new_cpt(c, *aux, (int) nextn, np, cells)) == NULL)
			break;
		if (cvar >= 0)
			t->parents[0] = cvar;
		t->parents[np - 1] = in->var[i];
		for (size_t x = 0; x < cells; x++)
			t->vals[x] = -INFINITY;

		/* Walk the vectors of c_(j-1) in rank order; add each value y. */
		memset(cnt, 0, sizeof(int) * (size_t) v);
		cnt[v - 1] = j;
		for (size_t r = 0; r < prevn; r++)
		{
			for (int y = 0; y < v; y++)
			{
				size_t		to;

				cnt[y]++;
				to = count_rank(cnt, v, j + 1);
				cnt[y]--;
				t->vals[(r * (size_t) v + (size_t) y) * nextn + to] = 0.0;
			}
			if (r + 1 < prevn)
				count_next(cnt, v);
		}
		cvar = (*aux)++;
		*ncard = nextn;
		j++;
	}
	c->env->free(c->env->arg, cnt);
	return cvar;
}

/*
 * The distinct missing variables of the slots that do not aggregate, in
 * order of first appearance, into dvar[] and their cardinalities into
 * dcard[]; slot_d[s] is the index in dvar of slot s's member, or -1 when
 * it is observed or the slot aggregates.  Returns the count.  A variable
 * in two slots is one parent of the child's CPT.
 */
static int
distinct_missing(const gx_pfactor *f, const gx_pinst *in, int *dvar, int *dcard, int *slot_d)
{
	int			nd = 0;

	for (int s = 0; s < f->nslots; s++)
	{
		int			i = in->off[s];

		slot_d[s] = -1;
		if (f->slot_agg[s] || in->value[i] >= 0)
			continue;
		for (int d = 0; d < nd && slot_d[s] < 0; d++)
			if (dvar[d] == in->var[i])
				slot_d[s] = d;
		if (slot_d[s] < 0)
		{
			dvar[nd] = in->var[i];
			dcard[nd] = f->slot_card[s];
			slot_d[s] = nd++;
		}
	}
	return nd;
}

/*
 * Each parent's contribution to the scores of a logistic child: add[p] has
 * pcard[p] x (card - 1) entries, the score of class k >= 2 that value x of
 * parent p adds at x * (card - 1) + k - 2.  A direct parent adds its
 * weight in every slot it fills (one variable may fill two); a chain end
 * (chain_pos[s] = p) adds, for the count vector of each rank, the sum over
 * values y of count(y) times y's weight.  Returns the tables in one
 * allocation, or NULL with c->err set.
 */
static double **
score_tables(pctx *c, int np, const int *pcard, const int *slot_d, const int *chain_pos,
			 const int *slot_off)
{
	const gx_pfactor *f = c->f;
	size_t		k1 = (size_t) (f->card - 1);
	size_t		total = 0;
	double	  **add;
	double	   *cur;
	int		   *cnt;

	for (int p = 0; p < np; p++)
		total += (size_t) pcard[p] * k1;
	add = pa_alloc(c, sizeof(double *) * (size_t) (np ? np : 1) + sizeof(double) * total);
	cnt = pa_alloc(c, sizeof(int) * (size_t) (slot_off[f->nslots] + 1));
	if (c->err)
		return NULL;
	cur = (double *) (add + (np ? np : 1));
	memset(cur, 0, sizeof(double) * total);
	for (int p = 0; p < np; p++)
	{
		add[p] = cur;
		cur += (size_t) pcard[p] * k1;
	}
	for (int s = 0; s < f->nslots; s++)
	{
		int			v = f->slot_card[s];

		if (slot_d[s] >= 0)
		{
			for (int x = 0; x < v; x++)
				for (int kk = 2; kk <= f->card; kk++)
					add[slot_d[s]][(size_t) x * k1 + (size_t) (kk - 2)] +=
						weight(f, slot_off, kk, s, x);
			continue;
		}
		if (chain_pos[s] < 0)
			continue;
		memset(cnt, 0, sizeof(int) * (size_t) v);
		cnt[v - 1] = missing_in_slot(c->in, s);
		for (int r = 0; r < pcard[chain_pos[s]]; r++)
		{
			for (int y = 0; y < v; y++)
				for (int kk = 2; kk <= f->card; kk++)
					add[chain_pos[s]][(size_t) r * k1 + (size_t) (kk - 2)] +=
						cnt[y] * weight(f, slot_off, kk, s, y);
			count_next(cnt, v);
		}
	}
	c->env->free(c->env->arg, cnt);
	return add;
}

/*
 * Fill the child's CPT t (its parents' cardinalities in pcard) for a
 * logistic factor, in logs: per parent row r, log P(k | r) = b_k + s_k(r)
 * - log(sum_j exp(b_j + s_j(r))), where b_k is class k's score from the
 * bias and the observed members (the same in every row, b_1 = 0) and
 * s_k(r) the row's contribution (score_tables).  With B the largest b_j,
 * this is (b_k - B) + s_k(r) - L(r), L(r) = log(sum_j exp(b_j - B +
 * s_j(r))).  The first term goes into t->lscale[k]; the rest is computed
 * from quantities the size of the row contributions, so the differences
 * between rows, all the posterior of the missing parents depends on when
 * the child is observed, keep their precision however large B is.
 * Returns GX_OK, or GX_EINVAL when a score is not finite.
 */
static int
fill_logistic(pctx *c, gx_pcpt *t, const double *base, const int *slot_d,
			  const int *chain_pos, const int *slot_off, const int *pcard)
{
	int			np = t->nparents;
	int			k = c->f->card;
	size_t		k1 = (size_t) (k - 1);
	size_t		rows = t->ncells / (size_t) k;
	double	  **add;
	int		   *val;
	double	   *sc;
	int			rc = GX_OK;

	add = score_tables(c, np, pcard, slot_d, chain_pos, slot_off);
	val = pa_alloc(c, sizeof(int) * (size_t) (np ? np : 1));
	sc = pa_alloc(c, sizeof(double) * (size_t) k);
	if (c->err)
		goto done;
	t->lscale[0] = 0.0;
	for (size_t kk = 0; kk < k1; kk++)
	{
		if (!isfinite(base[kk]))
			rc = GX_EINVAL;
		t->lscale[kk + 1] = base[kk];
	}
	{
		double		bmax = 0.0;

		for (int kk = 0; kk < k; kk++)
			bmax = fmax(bmax, t->lscale[kk]);
		for (int kk = 0; kk < k; kk++)
			t->lscale[kk] -= bmax;
	}
	memset(val, 0, sizeof(int) * (size_t) (np ? np : 1));
	for (size_t r = 0; r < rows && rc == GX_OK; r++)
	{
		double		mx = -INFINITY,
					sum = 0.0,
					lse;

		if ((r & 4095) == 4095 && c->env->interrupt)
			c->env->interrupt(c->env->arg);
		sc[0] = 0.0;
		for (size_t kk = 0; kk < k1; kk++)
		{
			double		s_k = 0.0;

			for (int p = 0; p < np; p++)
				s_k += add[p][(size_t) val[p] * k1 + kk];
			if (!isfinite(s_k) || !isfinite(s_k + t->lscale[kk + 1]))
				rc = GX_EINVAL;
			sc[kk + 1] = s_k;
		}
		for (int kk = 0; kk < k; kk++)
			mx = fmax(mx, sc[kk] + t->lscale[kk]);
		for (int kk = 0; kk < k; kk++)
			sum += exp(sc[kk] + t->lscale[kk] - mx);
		lse = mx + log(sum);
		for (int kk = 0; kk < k; kk++)
			t->vals[r * (size_t) k + (size_t) kk] = sc[kk] - lse;

		/* Next parent combination, the last parent fastest. */
		for (int p = np - 1; p >= 0; p--)
		{
			if (++val[p] < pcard[p])
				break;
			val[p] = 0;
		}
	}
done:
	if (add)
		c->env->free(c->env->arg, add);
	if (val)
		c->env->free(c->env->arg, val);
	if (sc)
		c->env->free(c->env->arg, sc);
	return c->err ? c->err : rc;
}

/*
 * Fill the child's CPT t for a noisy-OR factor, in logs.  loff is the log
 * of P(child off) from the leak and the observed members; the last parent
 * is the OR chain's end when has_z.  The off column's entries are loff
 * plus the row's log(1 - p) terms, with loff kept in t->lscale[0]; the on
 * column's are log(1 - P(off)), which needs loff and goes in whole.
 */
static void
fill_noisy_or(pctx *c, gx_pcpt *t, double loff, const int *slot_d, int has_z,
			  const int *pcard)
{
	const gx_pfactor *f = c->f;
	int			np = t->nparents;
	size_t		rows = t->ncells / 2;
	int		   *val = pa_alloc(c, sizeof(int) * (size_t) (np ? np : 1));

	if (val == NULL)
		return;
	/* P(off) = 0 exactly (sure_off) is a column of zeros, with offset 0. */
	t->lscale[0] = loff == -INFINITY ? 0.0 : loff;
	memset(val, 0, sizeof(int) * (size_t) (np ? np : 1));
	for (size_t r = 0; r < rows; r++)
	{
		csum		q = {0.0, 0.0};
		double		lq;

		if ((r & 4095) == 4095 && c->env->interrupt)
			c->env->interrupt(c->env->arg);
		for (int s = 0; s < f->nslots; s++)
			if (slot_d[s] >= 0 && val[slot_d[s]] > 0)
				csum_add(&q, log1p(-f->params[1 + s]));
		lq = (has_z && val[np - 1] == 1) || loff == -INFINITY ? -INFINITY : csum_get(&q);
		t->vals[r * 2] = lq;
		t->vals[r * 2 + 1] = log1mexp(loff + lq);
		for (int p = np - 1; p >= 0; p--)
		{
			if (++val[p] < pcard[p])
				break;
			val[p] = 0;
		}
	}
	c->env->free(c->env->arg, val);
}

/*
 * Step 1: the observed members' contribution, in logs.  For logistic,
 * base[k - 2] is class k's bias plus the weights of the observed members'
 * values; for noisy-OR, base[0] is log(1 - leak) plus log(1 - p) per active
 * observed member.  Both are compensated sums, in acc (one per entry of
 * base).  For noisy-OR, *sure_off is set when some term is log(0) (P(off)
 * is exactly 0) and *maybe_on cleared when every term is log(1) (P(off)
 * is exactly 1).
 */
static void
fold_observed(const gx_pfactor *f, const gx_pinst *in, const int *slot_off,
			  double *base, csum *acc, int *sure_off, int *maybe_on)
{
	size_t		stride = (size_t) slot_off[f->nslots] + 1;
	int			nk = f->kind == GX_LOGISTIC ? f->card - 1 : 1;

	for (int kk = 0; kk < nk; kk++)
	{
		acc[kk].s = f->kind == GX_LOGISTIC ? f->params[(size_t) kk * stride]
			: log1p(-f->params[0]);
		acc[kk].c = 0.0;
	}
	*sure_off = f->kind == GX_NOISY_OR && f->params[0] == 1.0;
	*maybe_on = f->kind == GX_NOISY_OR && f->params[0] > 0.0;
	for (int s = 0; s < f->nslots; s++)
		for (int i = in->off[s]; i < in->off[s + 1]; i++)
		{
			int			y = in->value[i];

			if (y < 0)
				continue;
			if (f->kind == GX_NOISY_OR && y > 0)
			{
				csum_add(&acc[0], log1p(-f->params[1 + s]));
				*sure_off |= f->params[1 + s] == 1.0;
				*maybe_on |= f->params[1 + s] > 0.0;
			}
			for (int kk = 0; f->kind == GX_LOGISTIC && kk < nk; kk++)
				csum_add(&acc[kk], weight(f, slot_off, kk + 2, s, y));
		}
	for (int kk = 0; kk < nk; kk++)
		base[kk] = csum_get(&acc[kk]);
}

/*
 * The guard on the child's table t: an entry is -INFINITY (zero) only
 * where its exact value is zero, and nothing is NaN.  Every logistic entry
 * is positive.  A noisy-OR row's off entry is zero exactly when some term
 * of it is log(0): sure_off from the leak and the observed members, a
 * missing direct parent that is active with p = 1, or the OR chain's end
 * on; its on entry is zero exactly when every term is log(1): maybe_on
 * clear, no active direct parent with p > 0, and the chain's end off.
 * Returns GX_OK or GX_ELOST.
 */
static int
guard_child(pctx *c, const gx_pcpt *t, const int *slot_d, int has_z, const int *pcard,
			int sure_off, int maybe_on)
{
	const gx_pfactor *f = c->f;
	int			np = t->nparents;
	size_t		k = (size_t) t->card;
	size_t		rows = t->ncells / k;
	int		   *val = pa_alloc(c, sizeof(int) * (size_t) (np ? np : 1));
	int			rc = GX_OK;

	if (val == NULL)
		return c->err;
	memset(val, 0, sizeof(int) * (size_t) (np ? np : 1));
	for (size_t r = 0; r < rows && rc == GX_OK; r++)
	{
		int			zero_off = sure_off,
					on = maybe_on;

		for (int s = 0; f->kind == GX_NOISY_OR && s < f->nslots; s++)
			if (slot_d[s] >= 0 && val[slot_d[s]] > 0)
			{
				zero_off |= f->params[1 + s] == 1.0;
				on |= f->params[1 + s] > 0.0;
			}
		if (has_z && val[np - 1] == 1)
			zero_off = on = 1;
		for (size_t x = 0; x < k; x++)
		{
			double		v = t->vals[r * k + x];
			int			may_be_zero = f->kind == GX_NOISY_OR && (x == 0 ? zero_off : !on);

			if (isnan(v) || (v == -INFINITY && !may_be_zero))
				rc = GX_ELOST;

			/*
			 * The on entry is the one computed from a probability, 1 -
			 * P(off): below DBL_MIN that probability was subnormal and
			 * had lost bits.
			 */
			if (f->kind == GX_NOISY_OR && x == 1 && v > -INFINITY &&
				v + t->lscale[1] < LOG_DBL_MIN)
				rc = GX_ELOST;
		}
		for (int p = np - 1; p >= 0; p--)
		{
			if (++val[p] < pcard[p])
				break;
			val[p] = 0;
		}
	}
	for (size_t x = 0; x < k; x++)
		if (!isfinite(t->lscale[x]))
			rc = GX_ELOST;
	c->env->free(c->env->arg, val);
	return rc;
}

/*
 * Step 2: the chains, whose ends follow the direct parents in pvar and
 * pcard (np of them on entry); returns the new count of parents, with
 * *has_z set for an OR chain, or -1 with c->err set.
 */
static int
build_chains(pctx *c, int np, int *pvar, int *pcard, int *chain_pos, int *has_z)
{
	const gx_pfactor *f = c->f;
	int			aux = c->in->first_aux;

	for (int s = 0; s < f->nslots; s++)
		chain_pos[s] = -1;
	*has_z = 0;
	if (f->kind == GX_NOISY_OR)
	{
		int			z = or_chain(c);

		if (z >= 0)
		{
			pvar[np] = z;
			pcard[np++] = 2;
			*has_z = 1;
		}
		return c->err ? -1 : np;
	}
	for (int s = 0; s < f->nslots && !c->err; s++)
	{
		size_t		ncard;
		int			cv;

		if (!f->slot_agg[s])
			continue;
		cv = count_chain(c, s, &aux, &ncard);
		if (cv >= 0)
		{
			chain_pos[s] = np;
			pvar[np] = cv;
			pcard[np++] = (int) ncard;
		}
	}
	return c->err ? -1 : np;
}

/*
 * Steps 1 to 3 once the scratch arrays exist.  Returns GX_OK or an error,
 * with c->err also set for allocation failures.
 */
static int
ground_inst(pctx *c, int *ws)
{
	const gx_pfactor *f = c->f;
	int			ns = f->nslots;
	int		   *slot_off = ws;	/* ns + 1 */
	int		   *slot_d = slot_off + ns + 1;	/* ns */
	int		   *chain_pos = slot_d + ns;	/* ns */
	int		   *pvar = chain_pos + ns;	/* ns + 1 */
	int		   *pcard = pvar + ns + 1;	/* ns + 1 */
	int			nk = f->kind == GX_LOGISTIC ? f->card - 1 : 1;
	double	   *base;
	csum	   *acc;
	int			np,
				has_z,
				sure_off,
				maybe_on;
	size_t		cells;
	gx_pcpt    *t;
	int			rc = GX_OK;

	slot_off[0] = 0;
	for (int s = 0; s < ns; s++)
		slot_off[s + 1] = slot_off[s] + f->slot_card[s];
	base = pa_alloc(c, sizeof(double) * (size_t) (nk ? nk : 1));
	acc = pa_alloc(c, sizeof(csum) * (size_t) (nk ? nk : 1));
	if (c->err)
		goto out;
	fold_observed(f, c->in, slot_off, base, acc, &sure_off, &maybe_on);
	np = distinct_missing(f, c->in, pvar, pcard, slot_d);
	if ((np = build_chains(c, np, pvar, pcard, chain_pos, &has_z)) < 0)
		goto out;

	/* Step 3: the child's CPT; gx_param_size has bounded its size. */
	cells = (size_t) f->card;
	for (int p = 0; p < np; p++)
		cells *= (size_t) pcard[p];
	if ((t = new_cpt(c, c->in->child, f->card, np, cells)) == NULL)
		goto out;
	memcpy(t->parents, pvar, sizeof(int) * (size_t) np);
	if (f->kind == GX_NOISY_OR)
		fill_noisy_or(c, t, sure_off ? -INFINITY : base[0], slot_d, has_z, pcard);
	else
		rc = fill_logistic(c, t, base, slot_d, chain_pos, slot_off, pcard);
	if (rc == GX_OK && !c->err)
	{
		shift_columns(t);
		rc = guard_child(c, t, slot_d, has_z, pcard, sure_off, maybe_on);
	}
	if (rc != GX_OK && !c->err)
		c->err = rc;
out:
	if (base)
		c->env->free(c->env->arg, base);
	if (acc)
		c->env->free(c->env->arg, acc);
	return c->err;
}

int
gx_param_ground(const gx_env *env, const gx_pfactor *f, const gx_pinst *in,
				gx_pcpt *out)
{
	pctx		c = {env, f, in, out, 0, GX_OK};
	size_t		cells,
				hold;
	int		   *ws;
	int			rc;

	if ((rc = gx_param_size(env, f, in, &cells, &hold)) != GX_OK)
		return rc;
	ws = pa_alloc(&c, sizeof(int) * (size_t) (5 * f->nslots + 3));
	if (ws == NULL)
		return GX_ENOMEM;
	rc = ground_inst(&c, ws);
	env->free(env->arg, ws);
	if (rc != GX_OK)
		gx_param_free(env, out, c.nout);
	return rc;
}

void
gx_param_free(const gx_env *env, gx_pcpt *out, int n)
{
	for (int i = 0; i < n; i++)
	{
		env->free(env->arg, out[i].parents);
		env->free(env->arg, out[i].vals);
		env->free(env->arg, out[i].lscale);
	}
}
