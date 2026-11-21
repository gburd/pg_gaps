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
 * checks Gibbs sampling against exact inference (check_gibbs_case), and
 * the expected counts of gx_expected_counts against enumeration, on each
 * random network and on a first-order network built from it
 * (check_counts_case, check_counts_firstorder).
 * Exits non-zero on the first failure and prints the case's seed.
 */
#include "engine/gx.h"
#include "engine/gx_param.h"
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
	if (gx_check(&env, &bn, &bad) != GX_ECYCLE || bad != 0)
		FAIL("a variable that is its own parent was not rejected as GX_ECYCLE");
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
 * The chain's seed depends on the component and nothing else.  Three
 * copies of a three-variable chain are separate components, sampled with
 * sharing off; the third copy has one CPT entry moved by 1e-9.  The two
 * equal copies must be bit-identical (a seed that depends on where a
 * component sits in the network breaks that), and the third must differ
 * from them by Monte Carlo noise, not by about 1e-9 (a seed that ignores
 * the component runs the same chain on a near-identical model).
 */
static void
check_gibbs_seed(void)
{
	uint64_t	cseed = 0;
	int			card[9],
				ev[9],
				par[9];
	double		pr[2] = {0.3, 0.7},
				c1[4] = {0.8, 0.2, 0.35, 0.65},
				c1b[4] = {0.8 + 1e-9, 0.2 - 1e-9, 0.35, 0.65},
				c2[4] = {0.6, 0.4, 0.1, 0.9};
	gx_cpt		cpts[9];
	gx_bn		bn = {9, card, ev, 9, cpts};
	gx_env		g = env;
	gx_result	r;
	double		d12 = 0,
				d13 = 0;

	for (int k = 0; k < 3; k++)
	{
		int			b = 3 * k;

		for (int i = 0; i < 3; i++)
		{
			card[b + i] = 2;
			ev[b + i] = -1;
			par[b + i] = b + i - 1;
		}
		cpts[b] = (gx_cpt) {b, 0, NULL, pr};
		cpts[b + 1] = (gx_cpt) {b + 1, 1, &par[b + 1], k == 2 ? c1b : c1};
		cpts[b + 2] = (gx_cpt) {b + 2, 1, &par[b + 2], c2};
	}
	g.no_sharing = 1;
	g.samples = GIBBS_N;
	g.seed = 5;
	g.sample_always = 1;
	if (gx_infer(&g, &bn, &r) != GX_OK)
		FAIL("gibbs seed: gx_infer failed");
	for (int i = 0; i < 3; i++)
	{
		d12 = fmax(d12, fabs(M(&r, i, 0) - M(&r, 3 + i, 0)));
		d13 = fmax(d13, fabs(M(&r, i, 0) - M(&r, 6 + i, 0)));
	}
	if (d12 != 0)
		FAIL("gibbs seed: equal components differ by %g", d12);
	if (!(d13 > 1e-6))
		FAIL("gibbs seed: different components differ by only %g", d13);
	free_result(&bn, &r);
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

/* ----------------------------------------------------------------
 * Expected counts (gx_expected_counts) against brute force
 * ----------------------------------------------------------------
 */

#define MAXG MAXV
#define MAXCELLS (MAXG * 8 * 8 * 8 * 8)

/* What the random counts cases covered, so a vacuous pass shows. */
static long counts_multi_missing,
			counts_cache_hits;

/*
 * Run gx_expected_counts on bn with sharing on and off, require the two to
 * agree bit for bit, and compare them with the oracle: every count to
 * 1e-12 (relative above 1), loglik to 1e-10, every count non-negative and
 * each group's counts summing to its number of CPTs.  Evidence of
 * probability zero must be GX_EZERO.
 */
static void
check_counts(const gx_bn *bn, const int *group, int ngroups, const size_t *gsize,
			 uint64_t cseed)
{
	static double ocounts[MAXCELLS],
				counts[MAXCELLS],
				counts2[MAXCELLS];
	size_t		goff[MAXG],
				total = 0;
	int			ninst[MAXG] = {0};
	gx_env		ns = env;
	gx_counts	ec = {group, ngroups, gsize, counts, 0, 0, 0, 0};
	gx_counts	ec2 = {group, ngroups, gsize, counts2, 0, 0, 0, 0};
	double		z;
	int			orc,
				rc,
				rc2;

	for (int g = 0; g < ngroups; g++)
	{
		goff[g] = total;
		total += gsize[g];
	}
	for (int i = 0; i < bn->ncpts; i++)
		ninst[group[i]]++;
	memset(ocounts, 0, sizeof(double) * total);
	if ((orc = oracle_counts(bn, group, goff, ocounts, total, &z)) < 0)
		FAIL("counts oracle: too many missing");
	rc = gx_expected_counts(&env, bn, &ec);
	ns.no_sharing = 1;
	rc2 = gx_expected_counts(&ns, bn, &ec2);
	nchecks++;
	if (rc2 != rc)
		FAIL("counts: sharing off returned %d, on returned %d", rc2, rc);
	if (orc == 1)
	{
		if (rc != GX_EZERO)
			FAIL("counts: evidence has probability 0 but returned %d", rc);
		return;
	}
	if (rc != GX_OK)
		FAIL("gx_expected_counts returned %d", rc);
	counts_cache_hits += ec.cache_hits;
	for (int i = 0; i < bn->ncpts; i++)
	{
		int			nm = bn->evidence[bn->cpts[i].child] < 0;

		for (int j = 0; j < bn->cpts[i].nparents; j++)
			nm += bn->evidence[bn->cpts[i].parents[j]] < 0;
		counts_multi_missing += nm >= 2;
	}
	if (ec.loglik != ec2.loglik)
		FAIL("counts: loglik differs with sharing off");
	if (fabs(ec.loglik - log(z)) > 1e-10)
		FAIL("counts: loglik %.17g, oracle %.17g", ec.loglik, log(z));
	for (int g = 0; g < ngroups; g++)
	{
		double		sum = 0;

		for (size_t x = goff[g]; x < goff[g] + gsize[g]; x++)
		{
			if (counts[x] != counts2[x])
				FAIL("counts: group %d cell %zu differs with sharing off", g, x - goff[g]);
			if (counts[x] < 0)
				FAIL("counts: group %d cell %zu negative", g, x - goff[g]);
			if (fabs(counts[x] - ocounts[x]) > TOL * fmax(1.0, ocounts[x]))
				FAIL("counts: group %d cell %zu engine %.17g oracle %.17g",
					 g, x - goff[g], counts[x], ocounts[x]);
			sum += counts[x];
		}
		if (fabs(sum - ninst[g]) > TOL * fmax(1.0, ninst[g]))
			FAIL("counts: group %d sums to %.17g, has %d CPTs", g, sum, ninst[g]);
	}
}

/* A gen() network with every CPT its own group. */
static void
check_counts_case(net *n, uint64_t cseed)
{
	int			group[MAXV];
	size_t		gsize[MAXV];

	for (int i = 0; i < n->nvars; i++)
	{
		const gx_cpt *c = &n->cpts[i];

		group[i] = i;
		gsize[i] = (size_t) n->card[c->child];
		for (int j = 0; j < c->nparents; j++)
			gsize[i] *= (size_t) n->card[c->parents[j]];
	}
	check_counts(&n->bn, group, n->nvars, gsize, cseed);
}

/*
 * A first-order shape: a template of T variables (from gen()) copied R
 * times, every copy of a template variable in one group with the same
 * CPT.  With vertical edges, template variable t of copy r > 0 also has
 * t of copy r - 1 as its last parent (one group for those copies, another
 * for copy 0), which ties the copies into one component.  Evidence is
 * drawn per copy from two patterns, so equal copies are shared.  The CPT
 * order is shuffled.
 */
static void
check_counts_firstorder(uint64_t cseed)
{
	static double vvals[4][8 * 8 * 8 * 8 * 4];
	net			t;
	int			T,
				R = 1 + rnd(3),
				vert = rnd(2),
				nv;
	int			card[MAXV],
				ev[MAXV],
				par[MAXV][4],
				group[MAXV],
				order[MAXV],
				pat[2][4];
	size_t		gsize[2 * 4];
	gx_cpt		cpts[MAXV];
	gx_bn		bn;

	do
		gen(&t, rnd(4) == 0 ? 0.3 : 0.0, 0.0);
	while (t.nvars > 4);
	T = t.nvars;
	nv = T * R;
	for (int p = 0; p < 2; p++)
		for (int v = 0; v < T; v++)
			pat[p][v] = rnd(2) ? rnd(t.card[v]) : -1;

	/* Vertical CPTs: one random distribution per previous-copy value. */
	for (int v = 0; v < T; v++)
	{
		size_t		rows = (size_t) t.card[v];

		for (int j = 0; j < t.npar[v]; j++)
			rows *= (size_t) t.card[t.par[v][j]];
		for (size_t r = 0; r < rows; r++)
		{
			double		s = 0;
			double	   *row = &vvals[v][r * (size_t) t.card[v]];

			for (int x = 0; x < t.card[v]; x++)
				s += row[x] = 0.01 + rnd(1000) / 1000.0;
			for (int x = 0; x < t.card[v]; x++)
				row[x] /= s;
		}
		gsize[v] = (size_t) t.card[v];
		for (int j = 0; j < t.npar[v]; j++)
			gsize[v] *= (size_t) t.card[t.par[v][j]];
		gsize[T + v] = gsize[v] * (size_t) t.card[v];
	}

	for (int r = 0; r < R; r++)
	{
		int			p = rnd(2);

		for (int v = 0; v < T; v++)
		{
			int			g = r * T + v,
						np = t.npar[v];

			card[g] = t.card[v];
			ev[g] = pat[p][v];
			for (int j = 0; j < np; j++)
				par[g][j] = r * T + t.par[v][j];
			if (vert && r > 0)
				par[g][np++] = (r - 1) * T + v;
			cpts[g] = (gx_cpt)
			{
				g, np, par[g], (vert && r > 0) ? vvals[v] : t.vals[v]
			};
			group[g] = (vert && r > 0) ? T + v : v;
		}
	}

	/* Shuffle the CPTs, keeping each CPT's group with it. */
	for (int i = 0; i < nv; i++)
		order[i] = i;
	for (int i = nv - 1; i > 0; i--)
	{
		int			j = rnd(i + 1),
					tmp = order[i];

		order[i] = order[j];
		order[j] = tmp;
	}
	{
		gx_cpt		sc[MAXV];
		int			sg[MAXV];

		for (int i = 0; i < nv; i++)
		{
			sc[i] = cpts[order[i]];
			sg[i] = group[order[i]];
		}
		memcpy(cpts, sc, sizeof(sc));
		memcpy(group, sg, sizeof(sg));
	}
	bn = (gx_bn)
	{
		nv, card, ev, nv, cpts
	};
	if (gx_check(&env, &bn, &nv) != GX_OK)
		FAIL("first-order counts: gx_check rejected the network");
	check_counts(&bn, group, 2 * T, gsize, cseed);
}

/*
 * Fixed cases: complete data gives indicator counts; identical components
 * come from the cache and are counted once per instance; the size bound is
 * GX_ETOOBIG even with sampling on; a group of the wrong size is GX_EINVAL.
 */
static void
check_counts_fixed(void)
{
	uint64_t	cseed = 0;
	enum
	{
		K = 50
	};
	int			card[2 * K],
				ev[2 * K],
				par[2 * K],
				group[2 * K];
	double		prior[2] = {0.3, 0.7};
	double		cond[4] = {0.9, 0.1, 0.2, 0.8};
	size_t		gsize[2] = {2, 4};
	double		counts[6];
	gx_cpt		cpts[2 * K];
	gx_bn		bn = {2 * K, card, ev, 2 * K, cpts};
	gx_counts	ec = {group, 2, gsize, counts, 0, 0, 0, 0};
	gx_env		small = env;
	int			rc;

	/* K copies of x -> y; x missing, y observed 0 for even k, 1 for odd. */
	for (int k = 0; k < K; k++)
	{
		card[2 * k] = card[2 * k + 1] = 2;
		ev[2 * k] = -1;
		ev[2 * k + 1] = k % 2;
		par[2 * k + 1] = 2 * k;
		cpts[2 * k] = (gx_cpt) {2 * k, 0, NULL, prior};
		cpts[2 * k + 1] = (gx_cpt) {2 * k + 1, 1, &par[2 * k + 1], cond};
		group[2 * k] = 0;
		group[2 * k + 1] = 1;
	}
	if (gx_expected_counts(&env, &bn, &ec) != GX_OK || ec.cache_hits != K - 2)
		FAIL("counts sharing: cache hits %ld, want %d", ec.cache_hits, K - 2);

	/*
	 * P(x=1 | y=0) = 0.14 / 0.41 and P(x=1 | y=1) = 0.56 / 0.59, K / 2 times
	 * each; the y cells: (x, y) row-major.
	 */
	{
		double		a = 0.14 / 0.41,
					b = 0.56 / 0.59,
					want[6] = {K / 2 * (2 - a - b), K / 2 * (a + b),
				K / 2 * (1 - a), K / 2 * (1 - b), K / 2 * a, K / 2 * b};

		for (int x = 0; x < 6; x++)
			if (fabs(counts[x] - want[x]) > 1e-12 * (double) K)
				FAIL("counts sharing: cell %d is %.17g, want %.17g", x, counts[x], want[x]);
		if (fabs(ec.loglik - K / 2 * (log(0.41) + log(0.59))) > 1e-10)
			FAIL("counts sharing: loglik %.17g", ec.loglik);
	}

	/* Complete data: indicators, and log of the product of the entries. */
	for (int k = 0; k < K; k++)
		ev[2 * k] = 1;
	if (gx_expected_counts(&env, &bn, &ec) != GX_OK ||
		counts[0] != 0 || counts[1] != (double) K || counts[4] != K / 2 || counts[5] != K / 2 ||
		counts[2] != 0 || counts[3] != 0 ||
		fabs(ec.loglik - ((double) K * log(0.7) + K / 2 * (log(0.2) + log(0.8)))) > 1e-10)
		FAIL("counts on complete data are not indicators");

	/* A wrong group size. */
	gsize[1] = 3;
	if (gx_expected_counts(&env, &bn, &ec) != GX_EINVAL || ec.bad_var != 1)
		FAIL("counts: a group of the wrong size was not GX_EINVAL");
	gsize[1] = 4;

	/* Too big for exact: GX_ETOOBIG, not sampled. */
	for (int k = 0; k < K; k++)
		ev[2 * k] = -1;
	small.max_cells = 1;
	small.samples = 1000;
	if ((rc = gx_expected_counts(&small, &bn, &ec)) != GX_ETOOBIG || ec.bad_size != 1)
		FAIL("counts: over the bound returned %d, want GX_ETOOBIG", rc);
	nchecks += 4;
}

/* ----------------------------------------------------------------
 * Parameterised factors (gx_param.h)
 * ----------------------------------------------------------------
 */

#define PMAXM 8					/* explicit members, all slots together */

static long param_aux_cases;		/* compared cases with auxiliary variables */
#define PMAXS 3					/* slots */

/* A random parameterised factor and one instance of it. */
typedef struct pcase
{
	gx_pfactor	f;
	int			slot_card[PMAXS];
	unsigned char slot_agg[PMAXS];
	double		params[3 * (1 + 3 * 4)];
	int			off[PMAXS + 1];
	int			mvar[PMAXM];	/* member -> distinct variable 0..nd-1 */
	int			mval[PMAXM];	/* the member's evidence, or -1 */
	int			nd;				/* distinct member variables */
	int			dcard[PMAXM];
	int			dev[PMAXM];
} pcase;

/*
 * The formula, written out over every explicit member, independently of
 * gx_param.c: P(child = k | the members' values val[] by distinct
 * variable).
 */
static double
pformula(const pcase *pc, const int *val, int k)
{
	const gx_pfactor *f = &pc->f;

	if (f->kind == GX_NOISY_OR)
	{
		double		off = 1.0 - f->params[0];

		for (int s = 0; s < f->nslots; s++)
			for (int i = pc->off[s]; i < pc->off[s + 1]; i++)
				if (val[pc->mvar[i]] > 0)
					off *= 1.0 - f->params[1 + s];
		return k == 0 ? off : 1.0 - off;
	}
	else
	{
		int			width = 1;
		double		sc[4],
					z = 0.0;

		for (int s = 0; s < f->nslots; s++)
			width += pc->slot_card[s];
		for (int c = 0; c < f->card; c++)
		{
			int			base = 1;

			sc[c] = 0.0;
			if (c == 0)
				continue;
			sc[c] = f->params[(c - 1) * width];
			for (int s = 0; s < f->nslots; s++)
			{
				for (int i = pc->off[s]; i < pc->off[s + 1]; i++)
					sc[c] += f->params[(c - 1) * width + base + val[pc->mvar[i]]];
				base += pc->slot_card[s];
			}
		}
		for (int c = 0; c < f->card; c++)
			z += exp(sc[c]);
		return exp(sc[k]) / z;
	}
}

/* Draw a random factor and instance into pc. */
static void
pgen(pcase *pc)
{
	gx_pfactor *f = &pc->f;
	int			nm = 0;

	memset(pc, 0, sizeof(*pc));
	f->kind = rnd(2) ? GX_LOGISTIC : GX_NOISY_OR;
	f->card = f->kind == GX_NOISY_OR ? 2 : 2 + rnd(3);
	f->nslots = 1 + rnd(PMAXS);
	pc->nd = 0;
	for (int s = 0; s < f->nslots; s++)
	{
		int			n;

		pc->slot_card[s] = 1 + rnd(3);
		pc->slot_agg[s] = (unsigned char) rnd(2);
		n = pc->slot_agg[s] ? rnd(PMAXM / f->nslots + 1) : 1;
		pc->off[s] = nm;
		for (int i = 0; i < n; i++, nm++)
		{
			int			reuse = -1;

			/* Now and then a variable already in an earlier slot. */
			for (int d = 0; d < pc->nd && reuse < 0; d++)
				if (pc->dcard[d] == pc->slot_card[s] && rnd(4) == 0)
				{
					reuse = d;
					for (int j = pc->off[s]; j < nm; j++)
						if (pc->mvar[j] == d)
							reuse = -1;
				}
			if (reuse < 0)
			{
				reuse = pc->nd++;
				pc->dcard[reuse] = pc->slot_card[s];
				pc->dev[reuse] = rnd(2) ? -1 : rnd(pc->slot_card[s]);
			}
			pc->mvar[nm] = reuse;
		}
	}
	pc->off[f->nslots] = nm;
	for (int i = 0; i < nm; i++)
		pc->mval[i] = pc->dev[pc->mvar[i]];
	f->slot_card = pc->slot_card;
	f->slot_agg = pc->slot_agg;
	if (f->kind == GX_NOISY_OR)
	{
		f->nparams = 1 + f->nslots;
		for (int i = 0; i < f->nparams; i++)
			pc->params[i] = rnd(8) == 0 ? (double) rnd(2) : rnd(1000) / 1000.0;
	}
	else
	{
		int			width = 1;

		for (int s = 0; s < f->nslots; s++)
			width += pc->slot_card[s];
		f->nparams = (f->card - 1) * width;
		for (int i = 0; i < f->nparams; i++)
			pc->params[i] = (rnd(6001) - 3000) / 1000.0;
	}
	f->params = pc->params;
}

/*
 * One case: the dense network (member priors, then the child's CPT over
 * every distinct member by the formula) and the parameterised one (the
 * same priors, then gx_param_ground's CPTs, over the missing members
 * only, plus auxiliary variables) give the same marginals to 1e-12 for
 * every missing member and the child.  A component holding an auxiliary
 * CPT with a zero must be refused for sampling.
 */
static void
check_param_case(uint64_t cseed)
{
	pcase		pc;
	int			card[PMAXM + 1 + 64],
				ev[PMAXM + 1 + 64];
	double		prior[PMAXM][3];
	double	   *dense;
	int			dpar[PMAXM];
	gx_cpt		cpts[PMAXM + 1 + 64];
	gx_pcpt		pcpts[64 + 1];
	int			vals[PMAXM];
	gx_pinst	in;
	gx_bn		a,
				b;
	gx_result	ra,
				rb;
	int			child,
				naux,
				rc,
				rca,
				bad,
				zero_aux = 0;
	size_t		rows = 1;

	pgen(&pc);
	child = pc.nd;
	for (int d = 0; d < pc.nd; d++)
	{
		double		sum = 0.0;

		card[d] = pc.dcard[d];
		ev[d] = pc.dev[d];
		for (int x = 0; x < card[d]; x++)
			sum += (prior[d][x] = 0.05 + rnd(1000) / 1000.0);
		for (int x = 0; x < card[d]; x++)
			prior[d][x] /= sum;
		cpts[d] = (gx_cpt) {d, 0, NULL, prior[d]};
		dpar[d] = d;
		rows *= (size_t) card[d];
	}
	card[child] = pc.f.card;
	ev[child] = rnd(2) ? -1 : rnd(pc.f.card);

	/* The dense CPT, every member variable a parent, the last fastest. */
	dense = malloc(sizeof(double) * rows * (size_t) pc.f.card);
	memset(vals, 0, sizeof(vals));
	for (size_t r = 0; r < rows; r++)
	{
		for (int k = 0; k < pc.f.card; k++)
			dense[r * (size_t) pc.f.card + (size_t) k] = pformula(&pc, vals, k);
		for (int d = pc.nd - 1; d >= 0; d--)
		{
			if (++vals[d] < card[d])
				break;
			vals[d] = 0;
		}
	}
	cpts[child] = (gx_cpt) {child, pc.nd, dpar, dense};
	a = (gx_bn) {pc.nd + 1, card, ev, pc.nd + 1, cpts};

	/* The parameterised instance; its variables are the same numbers. */
	in.child = child;
	in.off = pc.off;
	in.var = pc.mvar;
	in.value = pc.mval;
	in.first_aux = child + 1;
	naux = gx_param_naux(&pc.f, &in);
	if (gx_param_check(&pc.f) != GX_OK)
		FAIL("param: gx_param_check rejected a valid factor");
	rc = gx_param_ground(&env, &pc.f, &in, pcpts);
	if (rc != GX_OK)
		FAIL("param: gx_param_ground returned %d", rc);
	b = a;
	{
		gx_cpt		bc[PMAXM + 1 + 64];

		memcpy(bc, cpts, sizeof(gx_cpt) * (size_t) pc.nd);
		for (int i = 0; i <= naux; i++)
		{
			gx_pcpt    *t = &pcpts[i];

			if (t->child != (i < naux ? child + 1 + i : child))
				FAIL("param: CPT %d is for variable %d", i, t->child);
			if (i < naux)
			{
				card[t->child] = t->card;
				ev[t->child] = -1;
				for (size_t x = 0; x < t->ncells; x++)
					zero_aux |= t->vals[x] == 0.0;
			}
			bc[pc.nd + i] = (gx_cpt) {t->child, t->nparents, t->parents, t->vals};
			for (int p = 0; p < t->nparents; p++)
				if (t->parents[p] < child && pc.dev[t->parents[p]] >= 0)
					FAIL("param: CPT %d has observed parent %d", i, t->parents[p]);
		}
		b.nvars = b.ncpts = pc.nd + 1 + naux;
		b.cpts = bc;
		if (gx_check(&env, &b, &bad) != GX_OK)
			FAIL("param: gx_check rejected the parameterised network (var %d)", bad);
		memset(&ra, 0, sizeof(ra));
		memset(&rb, 0, sizeof(rb));
		rca = gx_infer(&env, &a, &ra);
		rc = gx_infer(&env, &b, &rb);
		if (rca != rc)
			FAIL("param: dense returned %d, parameterised %d", rca, rc);
		if (rc == GX_OK)
		{
			for (int v = 0; v <= child; v++)
				if (ev[v] < 0)
					for (int x = 0; x < card[v]; x++)
						if (fabs(M(&ra, v, x) - M(&rb, v, x)) > TOL)
							FAIL("param: var %d value %d: dense %.17g, parameterised %.17g",
								 v, x, M(&ra, v, x), M(&rb, v, x));
			free_result(&a, &ra);
			free_result(&b, &rb);
			nchecks++;
			if (naux > 1)
				param_aux_cases++;
		}

		/* Deterministic auxiliary CPTs rule out sampling. */
		if (zero_aux)
		{
			gx_env		se = env;

			se.samples = 100;
			se.sample_always = 1;
			memset(&rb, 0, sizeof(rb));
			rc = gx_infer(&se, &b, &rb);
			if (rc == GX_OK)
				FAIL("param: a component with a zero auxiliary CPT entry was sampled");
		}
	}
	gx_param_free(&env, pcpts, naux + 1);
	free(dense);
}

/*
 * Fixed cases: bad descriptions are GX_EINVAL; the size bound is
 * GX_ETOOBIG; fifteen five-valued observed parents of a binary logistic
 * child give a one-row CPT.
 */
static void
check_param_fixed(void)
{
	uint64_t	cseed = 0;
	int			sc[15],
				off[16],
				var[15],
				val[15];
	unsigned char agg[15] = {0};
	double		w[1 + 15 * 5];
	gx_pfactor	f = {GX_LOGISTIC, 2, 15, sc, agg, 1 + 15 * 5, w};
	gx_pinst	in = {15, off, var, val, 16};
	gx_pcpt		out[1];
	gx_env		tiny = env;

	for (int i = 0; i < 15; i++)
	{
		sc[i] = 5;
		off[i] = i;
		var[i] = i;
		val[i] = i % 5;
	}
	off[15] = 15;
	for (int i = 0; i < 1 + 15 * 5; i++)
		w[i] = 0.01 * i;
	if (gx_param_ground(&env, &f, &in, out) != GX_OK || out[0].nparents != 0 ||
		out[0].ncells != 2)
		FAIL("param: 15 observed parents did not give a one-row CPT");
	gx_param_free(&env, out, 1);

	/* All fifteen missing: 5^15 x 2 cells, over the bound. */
	for (int i = 0; i < 15; i++)
		val[i] = -1;
	if (gx_param_ground(&env, &f, &in, out) != GX_ETOOBIG)
		FAIL("param: 5^15 cells were not GX_ETOOBIG");

	f.nparams--;
	if (gx_param_check(&f) != GX_EINVAL)
		FAIL("param: a short parameter array passed");
	f.nparams++;
	w[3] = NAN;
	if (gx_param_check(&f) != GX_EINVAL)
		FAIL("param: a NaN weight passed");
	w[3] = 0.0;
	f.kind = GX_NOISY_OR;
	f.nparams = 16;
	w[2] = 1.5;
	if (gx_param_check(&f) != GX_EINVAL)
		FAIL("param: a noisy-OR probability above 1 passed");

	/* An aggregating slot of 200 missing ternary members: the count of a
	 * value over 200 members has C(202, 2) = 20301 values, so its link is
	 * 20100 x 3 x 20301 cells, over a bound of 1 << 20. */
	{
		int			sc1[1] = {3},
					off1[2] = {0, 200},
					var1[200],
					val1[200];
		unsigned char agg1[1] = {1};
		double		w1[4] = {0.1, 0.2, 0.3, 0.4};
		gx_pfactor	f1 = {GX_LOGISTIC, 2, 1, sc1, agg1, 4, w1};
		gx_pinst	in1 = {0, off1, var1, val1, 201};
		gx_pcpt    *big = malloc(sizeof(gx_pcpt) * 201);

		for (int i = 0; i < 200; i++)
		{
			var1[i] = i + 1;
			val1[i] = -1;
		}
		tiny.max_cells = (size_t) 1 << 20;
		if (gx_param_ground(&tiny, &f1, &in1, big) != GX_ETOOBIG)
			FAIL("param: a count chain over the bound was not GX_ETOOBIG");
		free(big);
	}
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
	check_gibbs_seed();
	check_gibbs_nomix();
	check_counts_fixed();
	check_param_fixed();
	for (long i = 0; i < cases; i++)
	{
		net			n;
		uint64_t	cseed = seed * 1000003ULL + (uint64_t) i;
		double		zero = (i % 4 == 0) ? 0.3 : 0.0;	/* i % 10 == 0 has both */
		double		evf = (double) (i % 3) * 0.35;

		rng = cseed;
		gen(&n, zero, evf);
		check_case(&n, cseed);
		check_counts_case(&n, cseed);
		check_counts_firstorder(cseed);
		check_param_case(cseed);
		if (i % 10 == 0)
			check_gibbs_case(&n, cseed);
	}
	check_gibbs_refusals(seed);
	if (cases >= 100 && (counts_multi_missing == 0 || counts_cache_hits == 0))
	{
		uint64_t	cseed = seed;

		FAIL("counts cases had %ld families with two missing variables and %ld "
			 "cache hits; both must be positive", counts_multi_missing, counts_cache_hits);
	}
	if (cases >= 100 && param_aux_cases == 0)
	{
		uint64_t	cseed = seed;

		FAIL("no parameterised case had two or more auxiliary variables");
	}
	printf("engine: parameterised factors: %ld cases with auxiliary chains\n",
		   param_aux_cases);
	printf("engine: expected counts: %ld families with two or more missing, %ld cache hits\n",
		   counts_multi_missing, counts_cache_hits);
	printf("engine: %ld checks passed (seed %llu, %ld random cases)\n",
		   nchecks, (unsigned long long) seed, cases);
	return 0;
}
