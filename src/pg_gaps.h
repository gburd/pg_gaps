/*-------------------------------------------------------------------------
 *
 * pg_gaps.h
 *	  Declarations shared by the pg_gaps module's source files.
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_GAPS_H
#define PG_GAPS_H

/* GUCs (gaps_infer.c), registered in _PG_init (pg_gaps.c). */
extern int	gaps_max_factor_cells;
extern int	gaps_max_total_cells;

#endif							/* PG_GAPS_H */
