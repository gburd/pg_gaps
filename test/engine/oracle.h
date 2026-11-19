/*
 * test/engine/oracle.h
 *	  Brute-force inference by enumerating every possible world.
 *
 * This is the reference the engine is checked against.  It shares no code
 * with the engine: it enumerates every assignment of the missing variables,
 * multiplies the CPT entries, and sums.  It is exponential on purpose and
 * only usable on small networks.
 */
#ifndef ORACLE_H
#define ORACLE_H

#include "engine/gx.h"

#define ORACLE_MAX_MISSING 16

typedef struct oracle_out
{
	double		z;				/* P(evidence) */
	double		marg[64][8];	/* [var][value] posterior; var < 64, card <= 8 */
	double		map_joint;		/* max over worlds of the joint */
	int			map[64];		/* one maximizing world */
} oracle_out;

/* Returns 0 on success, -1 if too many missing variables. */
int			oracle_run(const gx_bn *bn, oracle_out *out);

/* Joint probability of a complete assignment (evidence filled in). */
double		oracle_joint(const gx_bn *bn, const int *assign);

#endif
