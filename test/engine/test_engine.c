/*
 * test/engine/test_engine.c
 *	  Check the inference engine against brute-force enumeration.
 *
 * Usage: test_engine [seed [cases]]
 *
 * Generates random small Bayesian networks with random evidence and checks,
 * for every case:
 *   - every marginal the engine returns equals the oracle's to 1e-12;
 *   - every marginal sums to 1 and has no negative entry (AGENTS.md 14);
 *   - each component's MAP assignment has the maximum joint probability,
 *     and the reported probability equals joint / P(evidence);
 *   - the components partition the missing variables, and no CPT links
 *     two missing variables in different components;
 *   - evidence of probability zero is reported as GX_EZERO.
 * It also runs fixed cases for the error paths and the size bound, and
 * checks Gibbs sampling against exact inference (check_gibbs_case).
 * Exits non-zero on the first failure and prints the case's seed.
 */
#include "engine/gx.h"
#include "oracle.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOL 1e-12

/* Marginal of variable v, value x. */
#define M(r, v, x) ((r)->marg[(r)->marg_off[v] + (size_t) (x)])
#define MAXV 12

static long nchecks;

static void *
t_alloc(void *arg, size_t size)
{
	(void) arg;
	return malloc(size);
}

static void
t_free(void *arg, void *ptr)
{
	(void) arg;
	free(ptr);
}

static gx_env env = {t_alloc, t_free, NULL, NULL, (size_t) 1 << 20, (size_t) 1 << 26, 0, 0, 0, 0};

/*
 * Interrupt accounting for the scale tests: the engine's work between two
 * interrupt calls must stay small, or a query cannot be cancelled.  Work is
 * measured in allocations, which is deterministic where time is not; the
 * engine allocates per message, so a long silent stretch shows up as many
 * allocations between interrupts.
 */
static long allocs_since_interrupt,
			max_allocs_between;

static void *
c_alloc(void *arg, size_t size)
{
	(void) arg;
	if (++allocs_since_interrupt > max_allocs_between)
		max_allocs_between = allocs_since_interrupt;
	return malloc(size);
}

static void
c_interrupt(void *arg)
{
	(void) arg;
	allocs_since_interrupt = 0;
}

static gx_env cenv = {c_alloc, t_free, c_interrupt, NULL, (size_t) 1 << 20, (size_t) 1 << 26,
0, 0, 0, 0};

/* splitmix64: a small seeded PRNG, so failures reproduce from the seed. */
static uint64_t rng;

static uint64_t
next_u64(void)
{
	uint64_t	z = (rng += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static int
rnd(int n)
{
	return (int) (next_u64() % (uint64_t) n);
}

#define FAIL(...) \
	do { \
		fprintf(stderr, "FAIL (case seed %llu): ", (unsigned long long) cseed); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
		exit(1); \
	} while (0)

typedef struct net
{
	int			nvars;
	int			card[MAXV];
	int			ev[MAXV];
	int			npar[MAXV];
	int			par[MAXV][3];
	double		vals[MAXV][8 * 8 * 8 * 8];
	gx_cpt		cpts[MAXV];
	gx_bn		bn;
} net;

/*
 * Random DAG in topological order 0..n-1, each variable with up to three
 * parents among earlier ones; then a random permutation of the CPT order so
 * the engine cannot rely on it.  zero_frac is the chance a CPT entry is 0.
 */
static void
gen(net *n, double zero_frac, double ev_frac)
{
	int			order[MAXV];

	memset(n, 0, sizeof(*n));
	n->nvars = 1 + rnd(MAXV);
	for (int v = 0; v < n->nvars; v++)
	{
		size_t		rows = 1;

		n->card[v] = 1 + rnd(4);
		n->npar[v] = v == 0 ? 0 : rnd(v < 3 ? v + 1 : 4);
		for (int j = 0; j < n->npar[v]; j++)
		{
			int			p,
						dup;

			do
			{
				p = rnd(v);
				dup = 0;
				for (int k = 0; k < j; k++)
					dup |= n->par[v][k] == p;
			} while (dup);
			n->par[v][j] = p;
			rows *= (size_t) n->card[p];
		}
		for (size_t r = 0; r < rows; r++)
		{
			double		s = 0;
			double	   *row = &n->vals[v][r * n->card[v]];

			for (int x = 0; x < n->card[v]; x++)
			{
				row[x] = (rnd(1000) < zero_frac * 1000) ? 0.0 : 0.01 + rnd(1000) / 1000.0;
				s += row[x];
			}
			if (s == 0)
			{
				row[rnd(n->card[v])] = 1.0;
				s = 1.0;
			}
			for (int x = 0; x < n->card[v]; x++)
				row[x] /= s;
		}
		n->ev[v] = rnd(1000) < ev_frac * 1000 ? rnd(n->card[v]) : -1;
	}
	for (int i = 0; i < n->nvars; i++)
		order[i] = i;
	for (int i = n->nvars - 1; i > 0; i--)
	{
		int			j = rnd(i + 1),
					t = order[i];

		order[i] = order[j];
		order[j] = t;
	}
	for (int i = 0; i < n->nvars; i++)
	{
		int			v = order[i];

		n->cpts[i].child = v;
		n->cpts[i].nparents = n->npar[v];
		n->cpts[i].parents = n->par[v];
		n->cpts[i].vals = n->vals[v];
	}
	n->bn.nvars = n->nvars;
	n->bn.card = n->card;
	n->bn.evidence = n->ev;
	n->bn.ncpts = n->nvars;
	n->bn.cpts = n->cpts;
}

static void
free_result(const gx_bn *bn, gx_result *r)
{
	(void) bn;
	free(r->marg);
	free(r->marg_off);
	free(r->comp);
	free(r->map);
	free(r->comp_map_prob);
	free(r->comp_size);
	free(r->comp_samples);
}

static void
check_case(net *n, uint64_t cseed)
{
	const gx_bn *bn = &n->bn;
	oracle_out o;
	gx_result r;
	int			rc,
				bad;

	if (gx_check(&env, bn, &bad) != GX_OK)
		FAIL("gx_check rejected a valid network (var %d)", bad);
	if (oracle_run(bn, &o) != 0)
		FAIL("oracle: too many missing");
	memset(&r, 0, sizeof(r));
	rc = gx_infer(&env, bn, &r);
	nchecks++;

	/* With sharing off the engine must give bit-identical answers. */
	{
		gx_env		ns = env;
		gx_result	r2;
		int			rc2;

		ns.no_sharing = 1;
		memset(&r2, 0, sizeof(r2));
		rc2 = gx_infer(&ns, bn, &r2);
		if (rc2 != rc)
			FAIL("sharing off returned %d, on returned %d", rc2, rc);
		if (rc == GX_OK)
		{
			for (int v = 0; v < bn->nvars; v++)
				if (bn->evidence[v] < 0)
					for (int x = 0; x < bn->card[v]; x++)
						if (M(&r2, v, x) != M(&r, v, x))
							FAIL("var %d value %d differs with sharing off", v, x);
			free_result(bn, &r2);
		}
	}

	if (o.z == 0)
	{
		if (rc != GX_EZERO)
			FAIL("evidence has probability 0 but gx_infer returned %d", rc);
		return;
	}
	if (rc != GX_OK)
		FAIL("gx_infer returned %d", rc);

	/* Marginals: match the oracle, sum to 1, non-negative. */
	for (int v = 0; v < bn->nvars; v++)
	{
		double		s = 0;

		if (bn->evidence[v] >= 0)
		{
			if (r.comp[v] != -1)
				FAIL("observed var %d has a component", v);
			continue;
		}
		for (int x = 0; x < bn->card[v]; x++)
		{
			if (M(&r, v, x) < 0)
				FAIL("var %d value %d: negative probability %g", v, x, M(&r, v, x));
			if (fabs(M(&r, v, x) - o.marg[v][x]) > TOL)
				FAIL("var %d value %d: engine %.17g oracle %.17g",
					 v, x, M(&r, v, x), o.marg[v][x]);
			s += M(&r, v, x);
		}
		if (fabs(s - 1.0) > TOL)
			FAIL("var %d: marginal sums to %.17g", v, s);
		if (r.comp[v] < 0 || r.comp[v] >= r.ncomp)
			FAIL("var %d: component %d out of range", v, r.comp[v]);
	}

	/* Components: no CPT joins missing variables of different components. */
	for (int i = 0; i < bn->ncpts; i++)
	{
		const gx_cpt *c = &bn->cpts[i];
		int			seen = -1;

		if (bn->evidence[c->child] < 0)
			seen = r.comp[c->child];
		for (int j = 0; j < c->nparents; j++)
		{
			int			p = c->parents[j];

			if (bn->evidence[p] >= 0)
				continue;
			if (seen == -1)
				seen = r.comp[p];
			else if (r.comp[p] != seen)
				FAIL("CPT of %d joins components %d and %d", c->child, seen, r.comp[p]);
		}
	}

	/*
	 * MAP: the assembled assignment's joint equals the oracle's maximum
	 * (ties may pick a different world), and the product of the per-component
	 * MAP probabilities equals that joint over P(evidence), since components
	 * are independent given the evidence.
	 */
	{
		int			assign[MAXV];
		double		prod = 1.0,
					joint;

		for (int v = 0; v < bn->nvars; v++)
		{
			assign[v] = bn->evidence[v] >= 0 ? bn->evidence[v] : r.map[v];
			if (assign[v] < 0 || assign[v] >= bn->card[v])
				FAIL("var %d: MAP value %d out of range", v, assign[v]);
		}
		joint = oracle_joint(bn, assign);
		if (fabs(joint - o.map_joint) > TOL * fmax(1.0, o.map_joint))
			FAIL("MAP joint %.17g, oracle max %.17g", joint, o.map_joint);
		for (int c = 0; c < r.ncomp; c++)
		{
			if (r.comp_map_prob[c] <= 0 || r.comp_map_prob[c] > 1 + TOL)
				FAIL("component %d: MAP probability %g", c, r.comp_map_prob[c]);
			prod *= r.comp_map_prob[c];
		}
		if (fabs(prod - joint / o.z) > 1e-10 * fmax(1.0, prod))
			FAIL("product of component MAP probabilities %.17g != %.17g",
				 prod, joint / o.z);
	}
	free_result(bn, &r);
}

/* Fixed cases for the error paths. */
static void
check_errors(void)
{
	uint64_t	cseed = 0;
	int			card[2] = {2, 2};
	int			ev[2] = {-1, -1};
	int			p0[1] = {1};
	int			p1[1] = {0};
	double		v4[4] = {0.5, 0.5, 0.5, 0.5};
	double		v2[2] = {0.5, 0.5};
	gx_cpt		cyc[2] = {{0, 1, p0, v4}, {1, 1, p1, v4}};
	gx_cpt		dup[2] = {{0, 0, NULL, v2}, {0, 0, NULL, v2}};
	gx_cpt		self[2] = {{0, 1, p1, v4}, {1, 0, NULL, v2}};
	gx_bn		bn = {2, card, ev, 2, cyc};
	int			bad;
	double		negv[2] = {1.5, -0.5};
	gx_cpt		neg[2] = {{0, 0, NULL, negv}, {1, 0, NULL, v2}};

	if (gx_check(&env, &bn, &bad) != GX_ECYCLE)
		FAIL("a 2-cycle was not rejected as GX_ECYCLE");
	bn.cpts = dup;
	if (gx_check(&env, &bn, &bad) != GX_EINVAL)
		FAIL("two CPTs for one variable were not rejected");
	bn.cpts = self;
	if (gx_check(&env, &bn, &bad) != GX_EINVAL || bad != 0)
		FAIL("a variable that is its own parent was not rejected");
	bn.cpts = neg;
	if (gx_check(&env, &bn, &bad) != GX_EINVAL || bad != 0)
		FAIL("a negative CPT entry was not rejected");
	ev[0] = 2;
	bn.cpts = dup + 0;
	{
		gx_cpt		ok[2] = {{0, 0, NULL, v2}, {1, 0, NULL, v2}};

		bn.cpts = ok;
		if (gx_check(&env, &bn, &bad) != GX_EINVAL || bad != 0)
			FAIL("out-of-range evidence was not rejected");
	}
	nchecks += 5;
}

/*
 * The size bound: a chain of 20 binary variables where every variable is a
 * parent of the last one needs a 2^20-cell factor.  With max_cells below
 * that, gx_infer must return GX_ETOOBIG, not allocate it.
 */
static void
check_bound(void)
{
	uint64_t	cseed = 0;
	enum
	{
		N = 21
	};
	int			card[N],
				ev[N],
				par[N - 1];
	static double big[(size_t) 1 << 21];
	double		v2[2] = {0.5, 0.5};
	gx_cpt		cpts[N];
	gx_bn		bn = {N, card, ev, N, cpts};
	gx_env		small = env;
	gx_result	r;

	for (int i = 0; i < N; i++)
	{
		card[i] = 2;
		ev[i] = -1;
	}
	for (int i = 0; i < N - 1; i++)
	{
		par[i] = i;
		cpts[i] = (gx_cpt)
		{
			i, 0, NULL, v2
		};
	}
	for (size_t i = 0; i < ((size_t) 1 << 21); i++)
		big[i] = 0.5;
	cpts[N - 1] = (gx_cpt)
	{
		N - 1, N - 1, par, big
	};
	small.max_cells = 1 << 16;
	memset(&r, 0, sizeof(r));
	if (gx_infer(&small, &bn, &r) != GX_ETOOBIG)
		FAIL("a 2^21-cell factor with max_cells 2^16 did not return GX_ETOOBIG");
	nchecks++;
}

/*
 * First-order sharing: many identical independent components must be
 * answered from the cache, with the same answer as computing each.
 */
static void
check_sharing(void)
{
	uint64_t	cseed = 0;
	enum
	{
		K = 50
	};
	int			card[2 * K],
				ev[2 * K],
				par[2 * K];
	double		prior[2] = {0.3, 0.7};
	double		cond[4] = {0.9, 0.1, 0.2, 0.8};
	gx_cpt		cpts[2 * K];
	gx_bn		bn = {2 * K, card, ev, 2 * K, cpts};
	gx_result	r;

	for (int k = 0; k < K; k++)
	{
		card[2 * k] = card[2 * k + 1] = 2;
		ev[2 * k] = -1;
		ev[2 * k + 1] = k % 2;	/* two distinct evidence signatures */
		par[2 * k + 1] = 2 * k;
		cpts[2 * k] = (gx_cpt)
		{
			2 * k, 0, NULL, prior
		};
		cpts[2 * k + 1] = (gx_cpt)
		{
			2 * k + 1, 1, &par[2 * k + 1], cond
		};
	}
	memset(&r, 0, sizeof(r));
	if (gx_infer(&env, &bn, &r) != GX_OK)
		FAIL("sharing case failed");
	if (r.ncomp != K)
		FAIL("expected %d components, got %d", K, r.ncomp);
	if (r.cache_hits != K - 2)
		FAIL("expected %d cache hits, got %ld", K - 2, r.cache_hits);
	/*
	 * Component 0 has y = 0: P(x=1 | y=0) = 0.7*0.2 / (0.3*0.9 + 0.7*0.2).
	 * Component 1 has y = 1: P(x=1 | y=1) = 0.7*0.8 / (0.3*0.1 + 0.7*0.8).
	 */
	if (fabs(M(&r, 0, 1) - 0.14 / 0.41) > TOL || fabs(M(&r, 2, 1) - 0.56 / 0.59) > TOL)
		FAIL("sharing case: wrong posterior");
	free_result(&bn, &r);
	nchecks++;
}

/*
 * Underflow: a chain of N binary variables, each a noisy copy of the one
 * before, all missing except the last.  The joint probability of any world
 * is below 2^-N, which underflows a double for N = 2000, yet every
 * posterior is well defined.  By symmetry (uniform prior, symmetric noise)
 * every marginal is exactly what a 2-state chain gives in closed form.
 */
static void
check_underflow(void)
{
	uint64_t	cseed = 0;
	enum
	{
		N = 2000
	};
	static int	card[N],
				ev[N],
				par[N];
	static gx_cpt cpts[N];
	double		prior[2] = {0.5, 0.5};
	double		noisy[4] = {0.6, 0.4, 0.4, 0.6};
	gx_bn		bn = {N, card, ev, N, cpts};
	gx_result	r;
	int			rc;

	for (int i = 0; i < N; i++)
	{
		card[i] = 2;
		ev[i] = -1;
		par[i] = i - 1;
		cpts[i] = (gx_cpt)
		{
			i, i == 0 ? 0 : 1, i == 0 ? NULL : &par[i], i == 0 ? prior : noisy
		};
	}
	ev[N - 1] = 1;
	memset(&r, 0, sizeof(r));
	rc = gx_infer(&env, &bn, &r);
	if (rc != GX_OK)
		FAIL("a %d-variable chain returned %d (underflow?)", N, rc);

	/*
	 * Variable i is N-1-i noisy steps from the observed end; with flip
	 * probability q = 0.4, P(agree after k steps) = (1 + (1-2q)^k) / 2.
	 */
	for (int i = 0; i < N - 1; i += 97)
	{
		double		want = (1.0 + pow(0.2, N - 1 - i)) / 2.0;

		if (fabs(M(&r, i, 1) - want) > 1e-9)
			FAIL("chain var %d: P(1) = %.17g, want %.17g", i, M(&r, i, 1), want);
	}
	/*
	 * The MAP is all ones (each step agrees with probability 0.6, so the
	 * world where every variable copies the observed 1 is the mode).  Its
	 * posterior is 0.6^(N-1) / P(evidence) with P(evidence) = 0.5, which
	 * underflows to 0 in a double; that is the honest answer for a world
	 * this unlikely, and it must not be NaN.
	 */
	for (int i = 0; i < N - 1; i++)
		if (r.map[i] != 1)
			FAIL("chain MAP var %d is %d, want 1", i, r.map[i]);
	if (!(r.comp_map_prob[0] >= 0.0) || !isfinite(r.comp_map_prob[0]))
		FAIL("chain MAP probability %g is not a finite non-negative number",
			 r.comp_map_prob[0]);
	free_result(&bn, &r);
	nchecks++;
}

/*
 * One missing parent with K observed children, each slightly more likely
 * under x = 1: the posterior is a logistic in K * log-likelihood-ratio,
 * computed in closed form.  The products underflow past K of a few hundred
 * and pass through subnormals before that, which loses precision silently
 * unless the engine works in log space.
 */
static void
check_many_children(int K, double lo, double hi)
{
	uint64_t	cseed = (uint64_t) K;
	int		   *card = malloc(sizeof(int) * (size_t) (K + 1));
	int		   *ev = malloc(sizeof(int) * (size_t) (K + 1));
	gx_cpt	   *cpts = malloc(sizeof(gx_cpt) * (size_t) (K + 1));
	int			zero = 0;
	double		prior[2] = {0.5, 0.5};
	double		cond[4] = {1 - lo, lo, 1 - hi, hi};
	gx_bn		bn = {K + 1, card, ev, K + 1, cpts};
	gx_result	r;
	double		want = 1.0 / (1.0 + exp(-K * (log(hi) - log(lo))));

	for (int i = 0; i <= K; i++)
	{
		card[i] = 2;
		ev[i] = i ? 1 : -1;
		cpts[i] = (gx_cpt)
		{
			i, i ? 1 : 0, i ? &zero : NULL, i ? cond : prior
		};
	}
	memset(&r, 0, sizeof(r));
	if (gx_infer(&env, &bn, &r) != GX_OK)
		FAIL("%d observed children: gx_infer failed", K);
	if (fabs(M(&r, 0, 1) - want) > 1e-9 * fmax(1.0, want))
		FAIL("%d observed children: P(x=1) = %.17g, want %.17g", K, M(&r, 0, 1), want);
	if (want != 0.5 && r.map[0] != (want > 0.5 ? 1 : 0))	/* 0.5 is a tie */
		FAIL("%d observed children: MAP %d", K, r.map[0]);
	if (fabs(r.comp_map_prob[0] - fmax(want, 1 - want)) > 1e-9)
		FAIL("%d observed children: MAP probability %.17g, want %.17g",
			 K, r.comp_map_prob[0], fmax(want, 1 - want));
	free_result(&bn, &r);
	free(card);
	free(ev);
	free(cpts);
	nchecks++;
}

/*
 * Opposing evidence: y is uniform and copied exactly into a and b; a has K
 * observed children favouring a = 1 by 9:1 each, b has K - 1 favouring
 * b = 0.  The evidence nearly cancels, leaving P(y = 1) = 0.9 for every K.
 * Each side's message has cells 9^K apart, which no rescaling of plain
 * doubles can hold for K past a few hundred; only log-domain factors give
 * the answer.  (Found by an independent review: K = 338-340 returned
 * plausible wrong numbers, K > 340 an error.)
 */
static void
check_opposing(int K)
{
	uint64_t	cseed = (uint64_t) K;
	int			n = 3 + K + K - 1;
	int		   *card = malloc(sizeof(int) * (size_t) n);
	int		   *ev = malloc(sizeof(int) * (size_t) n);
	gx_cpt	   *cpts = malloc(sizeof(gx_cpt) * (size_t) n);
	int			py = 0,
				pa = 1,
				pb = 2;
	double		prior[2] = {0.5, 0.5};
	double		copy[4] = {1, 0, 0, 1};
	double		fa[4] = {0.9, 0.1, 0.1, 0.9};
	double		fb[4] = {0.1, 0.9, 0.9, 0.1};
	gx_bn		bn = {n, card, ev, n, cpts};
	gx_result	r;

	for (int i = 0; i < n; i++)
	{
		card[i] = 2;
		ev[i] = i < 3 ? -1 : 1;
	}
	cpts[0] = (gx_cpt)
	{
		0, 0, NULL, prior
	};
	cpts[1] = (gx_cpt)
	{
		1, 1, &py, copy
	};
	cpts[2] = (gx_cpt)
	{
		2, 1, &py, copy
	};
	for (int i = 0; i < K; i++)
		cpts[3 + i] = (gx_cpt)
	{
		3 + i, 1, &pa, fa
	};
	for (int i = 0; i < K - 1; i++)
		cpts[3 + K + i] = (gx_cpt)
	{
		3 + K + i, 1, &pb, fb
	};
	memset(&r, 0, sizeof(r));
	if (gx_infer(&env, &bn, &r) != GX_OK)
		FAIL("opposing evidence K=%d: gx_infer failed", K);
	if (fabs(M(&r, 0, 1) - 0.9) > 1e-9)
		FAIL("opposing evidence K=%d: P(y=1) = %.17g, want 0.9", K, M(&r, 0, 1));
	if (fabs(r.comp_map_prob[0] - 0.9) > 1e-9)
		FAIL("opposing evidence K=%d: MAP probability %.17g, want 0.9", K, r.comp_map_prob[0]);
	free_result(&bn, &r);
	free(card);
	free(ev);
	free(cpts);
	nchecks++;
}

/*
 * A star: one missing hub with N missing children, half of them observed.
 * Checks the marginals against closed form and that the engine calls
 * interrupt often enough; a hub with many kids is where a quadratic
 * bucket or fill computation hides.
 */
static void
check_star(int N)
{
	uint64_t	cseed = (uint64_t) N;
	int		   *card = malloc(sizeof(int) * (size_t) N);
	int		   *ev = malloc(sizeof(int) * (size_t) N);
	gx_cpt	   *cpts = malloc(sizeof(gx_cpt) * (size_t) N);
	int			zero = 0,
				nobs = 0;
	double		prior[2] = {0.5, 0.5};
	double		cond[4] = {0.9, 0.1, 0.2, 0.8};
	gx_bn		bn = {N, card, ev, N, cpts};
	gx_result	r;
	double		l0,
				l1,
				ph;

	for (int i = 0; i < N; i++)
	{
		card[i] = 2;
		ev[i] = (i > 0 && i % 2 == 0) ? (i % 4 == 0) : -1;
		nobs += ev[i] >= 0 && ev[i] == 1;
		cpts[i] = (gx_cpt)
		{
			i, i ? 1 : 0, i ? &zero : NULL, i ? cond : prior
		};
	}

	/* Hub posterior from the observed children (log odds). */
	l0 = l1 = log(0.5);
	for (int i = 2; i < N; i += 2)
	{
		l0 += log(cond[0 * 2 + ev[i]]);
		l1 += log(cond[1 * 2 + ev[i]]);
	}
	ph = 1.0 / (1.0 + exp(l0 - l1));

	max_allocs_between = allocs_since_interrupt = 0;
	memset(&r, 0, sizeof(r));
	if (gx_infer(&cenv, &bn, &r) != GX_OK)
		FAIL("star %d: gx_infer failed", N);
	if (fabs(M(&r, 0, 1) - ph) > 1e-9)
		FAIL("star %d: hub P(1) = %.17g, want %.17g", N, M(&r, 0, 1), ph);
	for (int i = 1; i < N; i += 2)
	{
		double		want = (1 - ph) * cond[1] + ph * cond[3];

		if (fabs(M(&r, i, 1) - want) > 1e-9)
			FAIL("star %d: child %d P(1) = %.17g, want %.17g", N, i, M(&r, i, 1), want);
	}
	if (max_allocs_between > 2000)
		FAIL("star %d: %ld allocations between interrupts", N, max_allocs_between);
	free_result(&bn, &r);
	free(card);
	free(ev);
	free(cpts);
	nchecks++;
	(void) nobs;
}

/*
 * Allocation failure: gx.h lets alloc return NULL, so every allocation
 * point must fail cleanly.  Run a small loopy network with a duplicated
 * component once to count its allocations, then again failing the n-th
 * for every n: each run must return GX_ENOMEM (or GX_EZERO is impossible
 * here, so only that) and leave nothing allocated.  The sanitizer build
 * catches a bad free on these paths too.  With samples > 0 every
 * component is sampled, which covers the sampler's allocations.
 */
static long fa_calls,
			fa_failat,
			fa_live;

static void *
fa_alloc(void *arg, size_t size)
{
	void	   *p;

	(void) arg;
	if (++fa_calls == fa_failat)
		return NULL;
	p = malloc(size);
	if (p)
		fa_live++;
	return p;
}

static void
fa_free(void *arg, void *ptr)
{
	(void) arg;
	if (ptr)
		fa_live--;
	free(ptr);
}

static void
check_alloc_failure(int samples)
{
	uint64_t	cseed = 0;
	int			card[8] = {2, 3, 2, 2, 2, 2, 2, 2};
	int			ev[8] = {-1, -1, -1, -1, -1, 1, -1, -1};
	int			p0[1] = {0},
				p12[2] = {1, 2},
				p3[1] = {3},
				p6[1] = {6};
	double		pr[2] = {0.3, 0.7};
	double		c1[6] = {0.2, 0.3, 0.5, 0.1, 0.1, 0.8};
	double		c2[4] = {0.4, 0.6, 0.9, 0.1};
	double		c3[12];
	double		c5[4] = {0.7, 0.3, 0.2, 0.8};
	gx_cpt		cpts[8] = {
		{0, 0, NULL, pr}, {1, 1, p0, c1}, {2, 1, p0, c2}, {3, 2, p12, c3},
		{4, 0, NULL, pr}, {5, 1, p3, c5}, {6, 0, NULL, pr}, {7, 1, p6, c5}
	};
	gx_bn		bn = {8, card, ev, 8, cpts};
	gx_env		fenv = {fa_alloc, fa_free, NULL, NULL, (size_t) 1 << 20, (size_t) 1 << 26, 0,
	samples, 1, samples > 0};
	gx_result	r;
	long		total;
	int			bad;

	for (int i = 0; i < 6; i++)
	{
		c3[2 * i] = 0.1 + 0.1 * i;
		c3[2 * i + 1] = 0.9 - 0.1 * i;
	}
	fa_failat = 0;
	fa_calls = 0;
	if (gx_infer(&fenv, &bn, &r) != GX_OK)
		FAIL("allocation-failure network does not infer");
	free_result(&bn, &r);
	total = fa_calls;
	for (fa_failat = 1; fa_failat <= total; fa_failat++)
	{
		int			rc;

		fa_live = 0;
		fa_calls = 0;
		rc = gx_infer(&fenv, &bn, &r);
		if (rc == GX_OK)
			free_result(&bn, &r);
		else if (rc != GX_ENOMEM)
			FAIL("allocation %ld failed: gx_infer returned %d, not GX_ENOMEM", fa_failat, rc);
		if (fa_live != 0)
			FAIL("allocation %ld failed: %ld blocks left allocated", fa_failat, fa_live);
		fa_live = 0;
		fa_calls = 0;
		rc = gx_check(&fenv, &bn, &bad);
		if (rc != GX_OK && rc != GX_ENOMEM)
			FAIL("allocation %ld failed in gx_check: returned %d", fa_failat, rc);
		if (fa_live != 0)
			FAIL("allocation %ld failed in gx_check: %ld blocks left", fa_failat, fa_live);
	}
	nchecks++;
}

/*
 * Gibbs against exact.  GIBBS_N sweeps per chain and GIBBS_TOL were set
 * from a measurement on this generator, which makes at most MAXV
 * variables: over 20,000 strictly positive models (seed 1, all sampled),
 * the largest error of any pooled marginal was 0.025 and the largest
 * spread between the engine's chains 0.078, so none was refused for
 * disagreeing chains (GX_MIX_SPREAD 0.1).  With a child's term dropped
 * from the full conditional the error reaches 0.14 on the first few
 * hundred cases.  A refusal is a safe answer, so the random cases allow
 * up to 1% of runs to be refused (GIBBS_MAX_REFUSED), each with a spread
 * above the limit.  The tolerance is a property of these small models,
 * not of the method.
 */
#define GIBBS_N 4000
#define GIBBS_TOL 0.1
#define GIBBS_MAX_REFUSED(cases) ((cases) / 100 + 1)

static long gibbs_cases,
			gibbs_refused;

/* Is some completion of the evidence a world of probability zero? */
static int
has_zero_world(const gx_bn *bn)
{
	int			assign[MAXV];
	int			miss[MAXV],
				nm = 0;

	for (int v = 0; v < bn->nvars; v++)
	{
		assign[v] = bn->evidence[v] >= 0 ? bn->evidence[v] : 0;
		if (bn->evidence[v] < 0)
			miss[nm++] = v;
	}
	for (;;)
	{
		int			i;

		if (oracle_joint(bn, assign) == 0.0)
			return 1;
		for (i = nm - 1; i >= 0; i--)
		{
			if (++assign[miss[i]] < bn->card[miss[i]])
				break;
			assign[miss[i]] = 0;
		}
		if (i < 0)
			return 0;
	}
}

/*
 * One random case sampled with sample_always: either every marginal is
 * within GIBBS_TOL of the oracle, is a distribution, and is bit-identical
 * with sharing off and on a second run; or some world consistent with the
 * evidence has probability zero and the engine refused with GX_ENOTPOS.
 * A second seed must change the answer but stay within the tolerance.
 */
static void
check_gibbs_case(net *n, uint64_t cseed)
{
	const gx_bn *bn = &n->bn;
	oracle_out o;
	gx_env		g = env;
	gx_result	r,
				r2;
	int			rc,
				zero,
				differs = 0,
				chain_matters = 0;

	if (oracle_run(bn, &o) != 0)
		FAIL("oracle: too many missing");
	if (o.z == 0)
		return;					/* exact's GX_EZERO is checked by check_case */
	zero = has_zero_world(bn);
	g.samples = GIBBS_N;
	g.seed = cseed;
	g.sample_always = 1;
	rc = gx_infer(&g, bn, &r);
	if (zero)
	{
		if (rc != GX_ENOTPOS)
			FAIL("gibbs: a zero-probability world, but gx_infer returned %d", rc);
		nchecks++;
		return;
	}
	gibbs_cases++;
	if (rc == GX_ENOMIX)
	{
		if (!(r.spread > GX_MIX_SPREAD) || r.bad_size < 1)
			FAIL("gibbs: refused with spread %g, component size %d", r.spread, r.bad_size);
		gibbs_refused++;
		nchecks++;
		return;
	}
	if (rc != GX_OK)
		FAIL("gibbs: gx_infer returned %d on a strictly positive case", rc);
	for (int v = 0; v < bn->nvars; v++)
	{
		double		s = 0;

		if (bn->evidence[v] >= 0)
			continue;
		if (r.comp_samples[r.comp[v]] != GIBBS_N || !isnan(r.comp_map_prob[r.comp[v]]))
			FAIL("gibbs: var %d not reported as sampled", v);
		for (int x = 0; x < bn->card[v]; x++)
		{
			if (!(M(&r, v, x) >= 0))
				FAIL("gibbs: var %d value %d: probability %g", v, x, M(&r, v, x));
			if (fabs(M(&r, v, x) - o.marg[v][x]) > GIBBS_TOL)
				FAIL("gibbs: var %d value %d: %.6f, exact %.6f", v, x,
					 M(&r, v, x), o.marg[v][x]);
			s += M(&r, v, x);
		}
		if (fabs(s - 1.0) > TOL)
			FAIL("gibbs: var %d: marginal sums to %.17g", v, s);
		for (int x = 0; x < bn->card[v]; x++)
			if (M(&r, v, r.map[v]) < M(&r, v, x))
				FAIL("gibbs: var %d: value %d is not the most probable", v, r.map[v]);
	}

	/* Same seed, sharing off: bit-identical. */
	g.no_sharing = 1;
	if (gx_infer(&g, bn, &r2) != GX_OK)
		FAIL("gibbs: sharing off failed");
	for (int v = 0; v < bn->nvars; v++)
		if (bn->evidence[v] < 0)
			for (int x = 0; x < bn->card[v]; x++)
				if (M(&r2, v, x) != M(&r, v, x))
					FAIL("gibbs: var %d value %d differs with sharing off", v, x);
	free_result(bn, &r2);

	/* Another seed: a different chain, still within the tolerance. */
	g.no_sharing = 0;
	g.seed = cseed + 1;
	rc = gx_infer(&g, bn, &r2);
	gibbs_cases++;
	if (rc == GX_ENOMIX)
	{
		gibbs_refused++;
		free_result(bn, &r);
		nchecks++;
		return;
	}
	if (rc != GX_OK)
		FAIL("gibbs: second seed failed");
	for (int v = 0; v < bn->nvars; v++)
		if (bn->evidence[v] < 0)
			for (int x = 0; x < bn->card[v]; x++)
			{
				differs |= M(&r2, v, x) != M(&r, v, x);
				if (fabs(M(&r2, v, x) - o.marg[v][x]) > GIBBS_TOL)
					FAIL("gibbs: second seed: var %d value %d off by %g", v, x,
						 fabs(M(&r2, v, x) - o.marg[v][x]));
			}
	/*
	 * Where every full conditional ignores the other missing variables (a
	 * child of cardinality 1, say), the Rao-Blackwellized estimate is
	 * exact for every seed.  Only an estimate that differs from exact must
	 * depend on the seed.
	 */
	for (int v = 0; v < bn->nvars; v++)
		if (bn->evidence[v] < 0)
			for (int x = 0; x < bn->card[v]; x++)
				chain_matters |= fabs(M(&r, v, x) - o.marg[v][x]) > 1e-9;
	if (chain_matters && !differs)
		FAIL("gibbs: a different seed gave a bit-identical answer");
	free_result(bn, &r2);
	free_result(bn, &r);
	nchecks++;
}

/* Few enough of the random Gibbs runs were refused for disagreeing chains. */
static void
check_gibbs_refusals(uint64_t cseed)
{
	if (gibbs_refused > GIBBS_MAX_REFUSED(gibbs_cases))
		FAIL("gibbs: %ld of %ld runs refused for disagreeing chains", gibbs_refused,
			 gibbs_cases);
	nchecks++;
}

/*
 * A bimodal posterior that single-site Gibbs cannot cross at this sample
 * count: a hub with HUB_K children, each agreeing with it with
 * probability 0.8, and no evidence.  Flipping every variable keeps the
 * joint, so every marginal is 0.5.  Given the hub, the children are
 * redrawn independently each sweep, and the hub turns only when at least
 * half of them disagree with it, which has probability about 1e-6 per
 * sweep at HUB_K = 50; so each chain stays in the mode it starts in.  The
 * first two chains start in opposite modes, and the engine must refuse
 * for every seed tried.
 */
#define HUB_K 50

static void
check_gibbs_nomix(void)
{
	uint64_t	cseed = 0;
	int			card[HUB_K + 1],
				ev[HUB_K + 1],
				hub = 0;
	double		prior[2] = {0.5, 0.5},
				agree[4] = {0.8, 0.2, 0.2, 0.8};
	gx_cpt		cpts[HUB_K + 1];
	gx_bn		bn = {HUB_K + 1, card, ev, HUB_K + 1, cpts};
	gx_env		g = env;
	gx_result	r;
	int			rc;

	for (int v = 0; v <= HUB_K; v++)
	{
		card[v] = 2;
		ev[v] = -1;
		cpts[v] = v == 0 ? (gx_cpt) {0, 0, NULL, prior} : (gx_cpt) {v, 1, &hub, agree};
	}
	g.samples = GIBBS_N;
	g.sample_always = 1;
	for (cseed = 0; cseed < 5; cseed++)
	{
		g.seed = cseed;
		if ((rc = gx_infer(&g, &bn, &r)) != GX_ENOMIX)
			FAIL("gibbs nomix: returned %d, want GX_ENOMIX", rc);
		if (!(r.spread > GX_MIX_SPREAD) || r.bad_size != HUB_K + 1 || r.bad_var < 0)
			FAIL("gibbs nomix: spread %g, component size %d, variable %d", r.spread,
				 r.bad_size, r.bad_var);
	}
	nchecks++;
}

/*
 * The fallback as a user meets it: a grid of binary variables (loopy,
 * induced width about its side) with every factor bound set too low for
 * exact.  With samples 0 that is GX_ETOOBIG; with samples set, it is
 * sampled and agrees with exact under the default bounds.  A zero in one
 * CPT entry the evidence does not rule out turns the sampled answer into
 * GX_ENOTPOS.  The interrupt callback must run at least once per sweep.
 */
static long ninterrupts;

static void
n_interrupt(void *arg)
{
	(void) arg;
	ninterrupts++;
}

static void
check_gibbs_fallback(void)
{
	uint64_t	cseed = 0;
	enum
	{
		S = 6,
		N = S * S
	};
	int			card[N],
				ev[N],
				par[N][2];
	double		prior[2] = {0.4, 0.6};
	double		one[4] = {0.8, 0.2, 0.3, 0.7};
	double		two[8] = {0.9, 0.1, 0.6, 0.4, 0.5, 0.5, 0.2, 0.8};
	double		twoz[8] = {0.9, 0.1, 0.6, 0.4, 0.5, 0.5, 0.0, 1.0};
	gx_cpt		cpts[N];
	gx_bn		bn = {N, card, ev, N, cpts};
	gx_env		small = env;
	gx_result	exact,
				r;
	int			rc;

	/* Variable (i, j) depends on its upper and left neighbours. */
	for (int v = 0; v < N; v++)
	{
		int			i = v / S,
					j = v % S,
					np = 0;

		card[v] = 2;
		ev[v] = (v % 7 == 3) ? v % 2 : -1;
		if (i > 0)
			par[v][np++] = v - S;
		if (j > 0)
			par[v][np++] = v - 1;
		cpts[v] = (gx_cpt)
		{
			v, np, par[v], np == 0 ? prior : np == 1 ? one : two
		};
	}
	if (gx_infer(&env, &bn, &exact) != GX_OK || exact.comp_samples[0] != 0)
		FAIL("gibbs fallback: exact failed or claims samples");

	small.max_cells = 16;
	small.max_total_cells = 64 * N;
	small.interrupt = n_interrupt;
	if ((rc = gx_infer(&small, &bn, &r)) != GX_ETOOBIG)
		FAIL("gibbs fallback: samples 0 returned %d, want GX_ETOOBIG", rc);

	small.samples = GIBBS_N;
	small.seed = 7;
	ninterrupts = 0;
	if ((rc = gx_infer(&small, &bn, &r)) != GX_OK)
		FAIL("gibbs fallback: sampling returned %d", rc);
	if (ninterrupts < GX_CHAINS * (GIBBS_N + GIBBS_N / 10))
		FAIL("gibbs fallback: %ld interrupts for %d sweeps", ninterrupts,
			 GX_CHAINS * (GIBBS_N + GIBBS_N / 10));
	for (int v = 0; v < N; v++)
	{
		if (ev[v] >= 0)
			continue;
		if (r.comp_samples[r.comp[v]] != GIBBS_N)
			FAIL("gibbs fallback: var %d not sampled", v);
		if (fabs(M(&r, v, 1) - M(&exact, v, 1)) > GIBBS_TOL)
			FAIL("gibbs fallback: var %d: %.6f, exact %.6f", v, M(&r, v, 1),
				 M(&exact, v, 1));
	}
	free_result(&bn, &r);

	/* The same grid with one zero entry: too large, and not sampleable. */
	cpts[N - 1].vals = twoz;
	if ((rc = gx_infer(&small, &bn, &r)) != GX_ENOTPOS)
		FAIL("gibbs fallback: a zero CPT entry returned %d, want GX_ENOTPOS", rc);
	free_result(&bn, &exact);
	nchecks += 3;
}

int
main(int argc, char **argv)
{
	uint64_t	seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
	long		cases = argc > 2 ? atol(argv[2]) : 20000;

	check_errors();
	check_bound();
	check_sharing();
	check_underflow();
	check_many_children(321, 0.1, 0.1 * pow(1.5, 1.0 / 321));
	check_many_children(400, 0.1, 0.11);
	check_many_children(1100, 0.5, 0.5);
	check_alloc_failure(0);
	check_alloc_failure(2000);	/* the same, sampled */
	check_opposing(339);
	check_opposing(5000);
	check_star(1501);
	check_star(100001);
	check_gibbs_fallback();
	check_gibbs_nomix();
	for (long i = 0; i < cases; i++)
	{
		net			n;
		uint64_t	cseed = seed * 1000003ULL + (uint64_t) i;
		double		zero = (i % 4 == 0) ? 0.3 : 0.0;	/* i % 10 == 0 has both */
		double		evf = (double) (i % 3) * 0.35;

		rng = cseed;
		gen(&n, zero, evf);
		check_case(&n, cseed);
		if (i % 10 == 0)
			check_gibbs_case(&n, cseed);
	}
	check_gibbs_refusals(seed);
	printf("engine: %ld checks passed (seed %llu, %ld random cases)\n",
		   nchecks, (unsigned long long) seed, cases);
	return 0;
}
