/*-------------------------------------------------------------------------
 *
 * gx_param.c
 *	  The CPTs of one ground instance of a parameterised factor (gx_param.h).
 *
 * Three steps per instance:
 *
 *	1. Fold the observed parents into the parameters: for logistic, add
 *	   their weights to each class's score; for noisy-OR, multiply their
 *	   (1 - p) into the probability that the child is off.
 *	2. Build the auxiliary chains over the missing members of aggregating
 *	   slots (the OR chain for noisy-OR, a count chain per slot for
 *	   logistic).
 *	3. Build the child's CPT over its missing non-aggregating parents and
 *	   the chains' last variables.
 *
 * Every table is checked against env->max_cells before it is allocated.
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
 * cells = (product of the parents' cardinalities) x card cells, which the
 * caller has checked against env->max_cells.  Returns it, or NULL with
 * c->err set.
 */
static gx_pcpt *
new_cpt(pctx *c, int child, int card, int nparents, size_t cells)
{
	gx_pcpt    *t = &c->out[c->nout];

	t->child = child;
	t->card = card;
	t->nparents = nparents;
	t->ncells = cells;
	t->parents = pa_alloc(c, sizeof(int) * (size_t) (nparents ? nparents : 1));
	t->vals = pa_alloc(c, sizeof(double) * cells);
	if (c->err)
	{
		if (t->parents)
			c->env->free(c->env->arg, t->parents);
		if (t->vals)
			c->env->free(c->env->arg, t->vals);
		return NULL;
	}
	c->nout++;
	return t;
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
 * Check instance in against f: slot ranges in order, one member in every
 * slot that does not aggregate, values in range.
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
			if (in->value[i] < -1 || in->value[i] >= f->slot_card[s])
				return GX_EINVAL;
	}
	return GX_OK;
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
			size_t		cells = mul_bounded((size_t) np * 2, (size_t) yc,
											c->env->max_cells);
			gx_pcpt    *t;

			if (in->value[i] >= 0)
				continue;
			if (cells == SIZE_MAX)
			{
				c->err = GX_ETOOBIG;
				break;
			}
			if ((t = new_cpt(c, aux, 2, np, cells)) == NULL)
				break;
			/* Parents (z_prev, y), y fastest; z_prev on forces on. */
			if (z >= 0)
				t->parents[0] = z;
			t->parents[np - 1] = in->var[i];
			for (int prev = 0; prev < np; prev++)
				for (int y = 0; y < yc; y++)
				{
					double		on = (np == 2 && prev == 1) ? 1.0 : (y > 0 ? p : 0.0);
					size_t		cell = ((size_t) prev * (size_t) yc + (size_t) y) * 2;

					t->vals[cell] = 1.0 - on;
					t->vals[cell + 1] = on;
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
		size_t		cells = mul_bounded(mul_bounded(prevn, (size_t) v, c->env->max_cells),
										nextn, c->env->max_cells);
		gx_pcpt    *t;

		if (in->value[i] >= 0)
			continue;
		if (nextn == SIZE_MAX || cells == SIZE_MAX || nextn > INT32_MAX)
		{
			c->err = GX_ETOOBIG;
			break;
		}
		if ((t = new_cpt(c, *aux, (int) nextn, np, cells)) == NULL)
			break;
		if (cvar >= 0)
			t->parents[0] = cvar;
		t->parents[np - 1] = in->var[i];
		memset(t->vals, 0, sizeof(double) * cells);

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
				t->vals[(r * (size_t) v + (size_t) y) * nextn + to] = 1.0;
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
 * logistic factor: per parent row, the softmax of base[k - 2] (class k's
 * score from the bias and the observed members) plus each parent value's
 * contribution (score_tables).  Returns GX_OK, or GX_EINVAL when a score
 * is not finite.
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
	memset(val, 0, sizeof(int) * (size_t) (np ? np : 1));
	for (size_t r = 0; r < rows && rc == GX_OK; r++)
	{
		double		mx = 0.0,
					sum = 0.0;

		if ((r & 4095) == 4095 && c->env->interrupt)
			c->env->interrupt(c->env->arg);
		sc[0] = 0.0;
		for (size_t kk = 0; kk < k1; kk++)
		{
			double		s_k = base[kk];

			for (int p = 0; p < np; p++)
				s_k += add[p][(size_t) val[p] * k1 + kk];
			if (!isfinite(s_k))
				rc = GX_EINVAL;
			sc[kk + 1] = s_k;
			if (s_k > mx)
				mx = s_k;
		}
		for (int kk = 0; kk < k; kk++)
		{
			sc[kk] = exp(sc[kk] - mx);
			sum += sc[kk];
		}
		for (int kk = 0; kk < k; kk++)
			t->vals[r * (size_t) k + (size_t) kk] = sc[kk] / sum;

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
 * Fill the child's CPT t for a noisy-OR factor.  off is P(child off) from
 * the leak and the observed members; the last parent is the OR chain's end
 * when has_z.
 */
static void
fill_noisy_or(pctx *c, gx_pcpt *t, double off, const int *slot_d, int has_z,
			  const int *pcard)
{
	const gx_pfactor *f = c->f;
	int			np = t->nparents;
	size_t		rows = t->ncells / 2;
	int		   *val = pa_alloc(c, sizeof(int) * (size_t) (np ? np : 1));

	if (val == NULL)
		return;
	memset(val, 0, sizeof(int) * (size_t) (np ? np : 1));
	for (size_t r = 0; r < rows; r++)
	{
		double		q = off;

		if ((r & 4095) == 4095 && c->env->interrupt)
			c->env->interrupt(c->env->arg);
		for (int s = 0; s < f->nslots; s++)
			if (slot_d[s] >= 0 && val[slot_d[s]] > 0)
				q *= 1.0 - f->params[1 + s];
		if (has_z && val[np - 1] == 1)
			q = 0.0;
		t->vals[r * 2] = q;
		t->vals[r * 2 + 1] = 1.0 - q;
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
 * Step 1: the observed members' contribution.  For logistic, base[k - 2]
 * is class k's bias plus the weights of the observed members' values; for
 * noisy-OR, *off is (1 - leak) times (1 - p) per active observed member.
 */
static void
fold_observed(const gx_pfactor *f, const gx_pinst *in, const int *slot_off,
			  double *base, double *off)
{
	size_t		stride = (size_t) slot_off[f->nslots] + 1;

	if (f->kind == GX_NOISY_OR)
		*off = 1.0 - f->params[0];
	for (int kk = 2; f->kind == GX_LOGISTIC && kk <= f->card; kk++)
		base[kk - 2] = f->params[(size_t) (kk - 2) * stride];
	for (int s = 0; s < f->nslots; s++)
		for (int i = in->off[s]; i < in->off[s + 1]; i++)
		{
			int			y = in->value[i];

			if (y < 0)
				continue;
			if (f->kind == GX_NOISY_OR && y > 0)
				*off *= 1.0 - f->params[1 + s];
			for (int kk = 2; f->kind == GX_LOGISTIC && kk <= f->card; kk++)
				base[kk - 2] += weight(f, slot_off, kk, s, y);
		}
}

/*
 * Steps 2 and 3 once the scratch arrays exist: the chains, then the
 * child's CPT.  Returns GX_OK or an error, with c->err also set for
 * allocation failures.
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
	double		base[1] = {0.0};
	double	   *basep = base;
	double		off = 1.0;
	int			np;
	int			has_z = 0;
	size_t		cells;
	gx_pcpt    *t;
	int			rc;

	slot_off[0] = 0;
	for (int s = 0; s < ns; s++)
		slot_off[s + 1] = slot_off[s] + f->slot_card[s];
	if (f->kind == GX_LOGISTIC && f->card > 1)
	{
		basep = pa_alloc(c, sizeof(double) * (size_t) (f->card - 1));
		if (basep == NULL)
			return c->err;
	}
	fold_observed(f, c->in, slot_off, basep, &off);
	np = distinct_missing(f, c->in, pvar, pcard, slot_d);

	/* Step 2: the chains; their ends follow the direct parents. */
	for (int s = 0; s < ns; s++)
		chain_pos[s] = -1;
	if (f->kind == GX_NOISY_OR)
	{
		int			z = or_chain(c);

		if (z >= 0)
		{
			pvar[np] = z;
			pcard[np++] = 2;
			has_z = 1;
		}
	}
	else
	{
		int			aux = c->in->first_aux;

		for (int s = 0; s < ns && !c->err; s++)
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
	}
	if (c->err)
		goto out;

	/* Step 3: the child's CPT. */
	cells = (size_t) f->card;
	for (int p = 0; p < np; p++)
		cells = mul_bounded(cells, (size_t) pcard[p], c->env->max_cells);
	if (cells == SIZE_MAX)
	{
		c->err = GX_ETOOBIG;
		goto out;
	}
	if ((t = new_cpt(c, c->in->child, f->card, np, cells)) == NULL)
		goto out;
	memcpy(t->parents, pvar, sizeof(int) * (size_t) np);
	if (f->kind == GX_NOISY_OR)
		fill_noisy_or(c, t, off, slot_d, has_z, pcard);
	else if ((rc = fill_logistic(c, t, basep, slot_d, chain_pos, slot_off, pcard)) != GX_OK
			 && !c->err)
		c->err = rc;
out:
	if (basep != base)
		c->env->free(c->env->arg, basep);
	return c->err;
}

int
gx_param_ground(const gx_env *env, const gx_pfactor *f, const gx_pinst *in,
				gx_pcpt *out)
{
	pctx		c = {env, f, in, out, 0, GX_OK};
	int		   *ws;
	int			rc;

	if (gx_param_check(f) != GX_OK || check_inst(f, in) != GX_OK)
		return GX_EINVAL;
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
	}
}
