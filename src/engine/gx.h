/*-------------------------------------------------------------------------
 *
 * gx.h
 *	  The pg_gaps inference engine: exact inference over a ground Bayesian
 *	  network with discrete variables.
 *
 * This header and the engine sources include no PostgreSQL headers, so the
 * engine compiles on its own for the standalone tests in test/engine/, which
 * compare it against brute-force enumeration of possible worlds.  The
 * extension supplies memory and interrupt callbacks through gx_env.
 *
 * Variables are numbered 0..nvars-1.  Each variable has a cardinality and
 * either an observed value (evidence) or -1 (missing).  Each variable has
 * exactly one conditional probability table (CPT).  A CPT is a dense array
 * in row-major order over (parents..., child), with the child varying
 * fastest, so for parent values p1..pn and child value c the entry is at
 * ((p1 * card(par2) + p2) * ... ) * card(child) + c.
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#ifndef GX_H
#define GX_H

#include <stddef.h>
#include <stdint.h>

/* Return codes. */
#define GX_OK		0
#define GX_ENOMEM	1			/* an allocation failed */
#define GX_ETOOBIG	2			/* a factor or the total would exceed a bound */
#define GX_EZERO	3			/* the evidence has probability zero */
#define GX_EINVAL	4			/* malformed network */
#define GX_ECYCLE	5			/* the network has a directed cycle */
#define GX_ENOTPOS	6			/* too large for exact inference, and a zero
								 * in its factors rules out Gibbs sampling */
#define GX_ENOMIX	7			/* sampled, but the chains disagree by more
								 * than GX_MIX_SPREAD */

/*
 * Gibbs sampling runs GX_CHAINS chains and refuses its answer when two of
 * them estimate some probability more than GX_MIX_SPREAD apart.
 */
#define GX_CHAINS	4
#define GX_MIX_SPREAD 0.1

/*
 * Services the caller provides.  alloc may return NULL (the engine then
 * returns GX_ENOMEM); under PostgreSQL it is palloc, which never does.
 * interrupt, if set, is called regularly in long loops and may longjmp
 * out; everything the engine allocated is then the caller's to discard,
 * which under PostgreSQL means resetting a memory context.
 */
typedef struct gx_env
{
	void	   *(*alloc) (void *arg, size_t size);
	void		(*free) (void *arg, void *ptr);
	void		(*interrupt) (void *arg);
	void	   *arg;
	size_t		max_cells;		/* largest factor the engine may build */
	size_t		max_total_cells;	/* all factors held at once, per component */
	int			no_sharing;		/* solve every component, even identical ones */
	int			samples;		/* Gibbs samples (sweeps) per chain for a
								 * component exact inference cannot solve;
								 * 0 never samples; at most INT_MAX / 2 */
	uint64_t	seed;			/* seeds every chain, with the component */
	int			sample_always;	/* sample even what exact could solve; for
								 * the tests, which compare the two */
} gx_env;

typedef struct gx_cpt
{
	int			child;
	int			nparents;
	const int  *parents;
	const double *vals;
} gx_cpt;

typedef struct gx_bn
{
	int			nvars;
	const int  *card;			/* cardinality per variable, >= 1 */
	const int  *evidence;		/* observed value, or -1 if missing */
	int			ncpts;			/* must equal nvars */
	const gx_cpt *cpts;			/* one per variable, any order */
} gx_bn;

/*
 * Results.  Missing variables are partitioned into components that are
 * independent of each other given the evidence; within a component they
 * are not.  Observed variables have comp -1 and no marginal.
 */
typedef struct gx_result
{
	int			ncomp;
	int		   *comp;			/* per variable */
	double	   *marg;			/* marginals, flattened: variable v's card[v]
								 * entries start at marg + marg_off[v] */
	size_t	   *marg_off;		/* per variable; observed ones have none */
	int		   *map;			/* per variable: jointly most probable value,
								 * or for a sampled component the most
								 * probable value of its sampled marginal */
	double	   *comp_map_prob;	/* per component: P(MAP assignment | evidence),
								 * NAN for a sampled component */
	int		   *comp_size;		/* per component: number of variables */
	int		   *comp_samples;	/* per component: Gibbs samples, 0 if exact */
	long		cache_hits;		/* components answered from the cache */
	size_t		peak_cells;		/* largest factor built */
	int			bad_var;		/* on error, a variable involved, or -1 */
	int			bad_size;		/* on error, the size of its component */
	double		spread;			/* on GX_ENOMIX, the chains' disagreement */
} gx_result;

/*
 * Check that bn is a well-formed network: cardinalities positive, one CPT
 * per variable, parents in range and distinct, CPT entries finite and
 * non-negative, evidence in range, and no directed cycle (a variable that
 * is its own parent is one).  Returns GX_OK, GX_EINVAL, GX_ECYCLE or
 * GX_ENOMEM, with *bad_var set to a variable involved in a failure.  Does
 * not check that CPT rows sum to 1; the caller validates that where the
 * values enter.
 */
extern int	gx_check(const gx_env *env, const gx_bn *bn, int *bad_var);

/*
 * Compute, for every missing variable, its marginal posterior given all
 * the evidence, and the jointly most probable assignment of each
 * component with its posterior probability.  Components with identical
 * structure, CPTs and evidence are computed once.  bn must pass gx_check.
 * On success out is filled with arrays from env->alloc.
 *
 * A component whose exact solution would exceed max_cells or
 * max_total_cells is sampled instead when env->samples > 0.  GX_CHAINS
 * Gibbs chains from dispersed starts each burn in for samples / 10
 * sweeps and then run samples sweeps, and their Rao-Blackwellized
 * marginals (the average over sweeps of each variable's full conditional)
 * are pooled.  If two chains estimate some probability more than
 * GX_MIX_SPREAD apart, the answer is refused with GX_ENOMIX and
 * out->spread set.  Agreement does not prove convergence: chains that are
 * all stuck in one mode agree.  Only a component whose reduced factors
 * (its CPT entries consistent with the evidence) are all positive is
 * sampled; one with a zero returns GX_ENOTPOS.  The chains are seeded
 * from env->seed and the component's canonical form, so equal components
 * get bit-identical answers whether or not they are shared, and a run is
 * reproducible given (seed, samples).  For a sampled component, map holds
 * each variable's most probable sampled value (not the joint MAP) and
 * comp_map_prob is NAN.
 */
extern int	gx_infer(const gx_env *env, const gx_bn *bn, gx_result *out);

/*
 * Expected counts for EM.  Every CPT belongs to a group (its first-order
 * factor), and every CPT of a group has group_size[g] cells.  The caller
 * fills the first five fields; gx_expected_counts fills the rest.
 */
typedef struct gx_counts
{
	const int  *group;			/* per CPT (in bn->cpts order), 0..ngroups-1 */
	int			ngroups;
	const size_t *group_size;	/* cells per group */
	double	   *counts;			/* caller's, sum of group_size doubles: group
								 * g's cells follow group g - 1's */
	double		loglik;			/* log P(all the evidence) */
	long		cache_hits;		/* components answered from the cache */
	int			bad_var;		/* on error, a variable involved, or -1 */
	int			bad_size;		/* on error, the size of its component */
} gx_counts;

/*
 * The E-step of EM: for every group g and cell x of its CPTs (row-major
 * over parents then child, as in a CPT), counts[g][x] is the sum over the
 * group's CPTs of P(family = x | evidence), the family being the CPT's
 * child and parents together.  A CPT whose family is fully observed adds
 * 1 to its observed cell, so with no missing values these are the plain
 * counts.  loglik is log P(evidence), constant CPTs included.
 *
 * Family posteriors come from the bucket tree gx_infer uses, and
 * components are shared through the same cache.  Nothing is sampled: a
 * component too large for exact inference is GX_ETOOBIG whatever
 * env->samples says.  bn must pass gx_check.  GX_EINVAL (with bad_var)
 * if a group is out of range or a CPT's size differs from its group's.
 */
extern int	gx_expected_counts(const gx_env *env, const gx_bn *bn, gx_counts *ec);

#endif							/* GX_H */
