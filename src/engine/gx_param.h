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

/* One CPT of an instance, in gx.h's layout, allocated with env->alloc. */
typedef struct gx_pcpt
{
	int			child;
	int			card;			/* the child's domain size */
	int			nparents;
	int		   *parents;
	size_t		ncells;
	double	   *vals;
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
 * The CPTs of instance in of factor f, which must pass gx_param_check:
 * one per auxiliary variable in their numbering order, then the child's,
 * gx_param_naux(f, in) + 1 in all, into out, which has room for them.
 * The child's parents are its missing non-aggregating parents in slot
 * order, then the last auxiliary variable of the chain (noisy-OR) or of
 * each aggregating slot's chain (logistic), in slot order.  Returns GX_OK,
 * GX_EINVAL (a malformed instance, or a logistic score that overflows),
 * GX_ETOOBIG (a CPT larger than env->max_cells, or the instance's CPTs
 * together larger than env->max_total_cells, either found before the table
 * is allocated) or GX_ENOMEM.  On error, the CPTs built so far are freed.
 */
extern int	gx_param_ground(const gx_env *env, const gx_pfactor *f,
							const gx_pinst *in, gx_pcpt *out);

/* Free the CPTs gx_param_ground returned. */
extern void gx_param_free(const gx_env *env, gx_pcpt *out, int n);

#endif							/* GX_PARAM_H */
