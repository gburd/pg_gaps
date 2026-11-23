/*-------------------------------------------------------------------------
 *
 * gx_param.h
 *	  Parameterised factors (logistic and noisy-OR): the CPTs of one ground
 *	  instance, computed from the factor's parameters and the instance's
 *	  evidence, for the inference engine (gx.h) to use as ordinary CPTs.
 *
 * A parameterised factor stores weights, not a table, so a factor over 15
 * five-valued parents needs 15 x 5 weights where its CPT would need 5^15
 * rows.  Grounding knows each instance's evidence, so it asks this module
 * for the instance's CPT over its missing parents only, with the observed
 * parents folded into the parameters.  That table is P(child | missing
 * parents, observed parents = e), an ordinary conditional the engine
 * handles like any other, and the engine itself is unchanged.
 *
 * A parent slot may be aggregating: it holds any number of exchangeable
 * parents (zero included) that share the slot's parameters.  Its missing
 * parents are combined through a chain of auxiliary variables, so that no
 * CPT grows with their number:
 *
 *	 noisy-OR:	z_1 = fired(y_1), z_j = z_(j-1) OR fired(y_j), over every
 *				missing member of every aggregating slot in slot order; each
 *				z is binary, and stochastic (a member fires with the slot's
 *				probability when its value is active).
 *	 logistic:	per aggregating slot, c_j = c_(j-1) + e(y_j), the vector of
 *				counts of each value among its first j missing members; c_j
 *				is a deterministic function of the members.  The child's
 *				scores depend on a slot only through its counts.
 *
 * Both chains have one auxiliary variable per missing member of an
 * aggregating slot (gx_param_naux).  Their CPTs have zeros, so a component
 * holding them is never Gibbs sampled (gx_infer refuses one with a zero).
 *
 * Every table is returned as a log CPT (gx.h): natural logs, offset per
 * child value.  Many observed parents push a child's entries far below
 * the smallest double (noisy-OR's (1 - p)^n, a logistic score of 1e4), and
 * when the child is observed the posterior of its missing parents depends
 * only on the ratios between the rows of one column.  So the observed
 * parents are folded in log space, and each column is shifted so that its
 * largest entry is 0, its shift returned in lscale; no entry loses
 * precision, and the engine adds the shift of the observed value to
 * log P(evidence) alone.
 *
 * Like gx.h, this includes no PostgreSQL headers.
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#ifndef GX_PARAM_H
#define GX_PARAM_H

#include "engine/gx.h"

#define GX_LOGISTIC 1
#define GX_NOISY_OR 2

/*
 * A return code beyond gx.h's: an entry whose exact value is positive came
 * out as zero.  The tables are computed in logs, so this guards against a
 * mistake rather than an expected loss.
 */
#define GX_ELOST	8

/*
 * Solving the component of an instance holds about four cells per cell of
 * the instance's tables (each reduced factor, its copy in the cache key,
 * and the messages along a chain; measured 4.0 to 4.1 for count chains
 * and up to 6 for the small tables of an OR chain, whose variables'
 * working arrays weigh more).  An instance is held to GX_PARAM_HOLD x its
 * cells plus GX_PARAM_HOLD_VALUE per value of its auxiliary variables plus
 * GX_PARAM_HOLD_BASE against env->max_total_cells, so that one the engine cannot solve is
 * refused here, before its tables are built.  When env->samples > 0, an
 * instance without auxiliary variables is held to 2 x its cells instead,
 * what sampling it needs.  The estimate covers the instance's own tables;
 * its members' other factors in the same component add to what the engine
 * holds, and are bounded by the engine itself.
 */
#define GX_PARAM_HOLD 5
#define GX_PARAM_HOLD_VALUE 8
#define GX_PARAM_HOLD_BASE 64

/*
 * A parameterised factor.  Parameters, by kind:
 *
 *	 logistic:	for each child value k = 2..card (value 1 is the reference,
 *				score 0), a bias and then one weight per value of each slot,
 *				slots in order: (card - 1) x (1 + sum slot_card) entries,
 *				class-major.  The score of k is the bias plus the weight of
 *				each parent's value (for an aggregating slot, of each
 *				member's value); P(child = k) is the softmax of the scores.
 *	 noisy-OR:	the leak, then one probability per slot: 1 + nslots
 *				entries, each in [0, 1].  The child is binary, its second
 *				value "on"; a parent is active when its value is not its
 *				first.  P(child off) = (1 - leak) x the product, over active
 *				parents, of (1 - their slot's probability).
 */
typedef struct gx_pfactor
{
	int			kind;			/* GX_LOGISTIC or GX_NOISY_OR */
	int			card;			/* the child's domain size */
	int			nslots;
	const int  *slot_card;		/* domain size of each slot's parents */
	const unsigned char *slot_agg;	/* nonzero: an aggregating slot */
	int			nparams;
	const double *params;
} gx_pfactor;

/*
 * One ground instance.  Slot s has the members off[s] .. off[s + 1] - 1 of
 * var[] and value[]: exactly one for a slot that does not aggregate, any
 * number for one that does.  value is the member's observed value
 * (0-based) or -1 when missing.  Auxiliary variables are numbered
 * first_aux, first_aux + 1, ... in the order they are created.
 */
typedef struct gx_pinst
{
	int			child;
	const int  *off;			/* nslots + 1 entries, off[0] = 0 */
	const int  *var;
	const int  *value;
	int			first_aux;
} gx_pinst;

/*
 * One CPT of an instance, a log CPT in gx.h's layout (vals and lscale are
 * logs), allocated with env->alloc.
 */
typedef struct gx_pcpt
{
	int			child;
	int			card;			/* the child's domain size */
	int			nparents;
	int		   *parents;
	size_t		ncells;
	double	   *vals;
	double	   *lscale;			/* card entries */
} gx_pcpt;

/*
 * Check a factor's description: kind, domain sizes, the number of
 * parameters, every parameter finite, and for noisy-OR a binary child and
 * every parameter in [0, 1].  Returns GX_OK or GX_EINVAL.
 */
extern int	gx_param_check(const gx_pfactor *f);

/* The number of auxiliary variables gx_param_ground creates for in. */
extern int	gx_param_naux(const gx_pfactor *f, const gx_pinst *in);

/*
 * The number of parent links of the CPTs gx_param_ground builds for in,
 * the sum of their nparents, without building them.  in must pass the
 * checks of gx_param_size.
 */
extern int	gx_param_nlinks(const gx_pfactor *f, const gx_pinst *in);

/*
 * The cells of instance in's tables, vals and lscale together, into
 * *cells, and the cells solving them is estimated to hold (GX_PARAM_HOLD)
 * into *hold, without building them.  Applies gx_param_ground's bounds,
 * so returns GX_OK, GX_EINVAL (a malformed factor or instance: a slot
 * range out of order, a slot that does not aggregate without exactly one
 * member, a value out of range, or an aggregating slot whose variables
 * are not strictly increasing, which also refuses one variable twice) or
 * GX_ETOOBIG, exactly as gx_param_ground would.  On GX_ETOOBIG, *hold is
 * the estimate, or SIZE_MAX when a single table is over env->max_cells.
 */
extern int	gx_param_size(const gx_env *env, const gx_pfactor *f, const gx_pinst *in,
						  size_t *cells, size_t *hold);

/*
 * The CPTs of instance in of factor f, which must pass gx_param_check:
 * one per auxiliary variable in their numbering order, then the child's,
 * gx_param_naux(f, in) + 1 in all, into out, which has room for them.
 * The child's parents are its missing non-aggregating parents in slot
 * order, then the last auxiliary variable of the chain (noisy-OR) or of
 * each aggregating slot's chain (logistic), in slot order.  Returns GX_OK,
 * GX_EINVAL (as gx_param_size, or a logistic score that overflows),
 * GX_ETOOBIG (a CPT larger than env->max_cells, or the instance's CPTs
 * together over the GX_PARAM_HOLD bound, found before anything is
 * allocated), GX_ELOST or GX_ENOMEM.  On error, the CPTs built so far
 * are freed.
 */
extern int	gx_param_ground(const gx_env *env, const gx_pfactor *f,
							const gx_pinst *in, gx_pcpt *out);

/* Free the CPTs gx_param_ground returned. */
extern void gx_param_free(const gx_env *env, gx_pcpt *out, int n);

#endif							/* GX_PARAM_H */
