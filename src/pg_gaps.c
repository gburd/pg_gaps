/*-------------------------------------------------------------------------
 *
 * pg_gaps.c
 *	  Module entry point for pg_gaps.
 *
 * pg_gaps brings the BayesStore probabilistic data model to PostgreSQL: a
 * table whose probabilistic columns may be missing (an "incomplete
 * relation"), plus a separately stored probabilistic graphical model that
 * describes how those values are distributed and correlated.  See
 * doc/DESIGN.md for the data model and ROADMAP.md for what is built.
 *
 * This file holds the module magic block and the GUCs.  gaps_infer.c is
 * the SQL entry point to the inference engine in engine/; the rest of the
 * SQL interface is in pg_gaps--*.sql.
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "fmgr.h"
#include "utils/guc.h"

#include "pg_gaps.h"

/*
 * PostgreSQL 18 added PG_MODULE_MAGIC_EXT, which lets pg_get_loaded_modules()
 * report the library's name and version.  PG_GAPS_VERSION comes from the
 * control file through the Makefile, so the two cannot disagree.
 */
#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(.name = "pg_gaps",.version = PG_GAPS_VERSION);
#else
PG_MODULE_MAGIC;
#endif

void		_PG_init(void);

/*
 * Register the GUCs.  Both bound inference (AGENTS.md rule 16): a query
 * whose model would need a larger factor, or more cells held at once,
 * fails with an ERROR naming the setting instead of exhausting memory.
 */
void
_PG_init(void)
{
	DefineCustomIntVariable("gaps.max_factor_cells",
							"Largest factor, in cells, that inference may build.",
							NULL,
							&gaps_max_factor_cells,
							1 << 20, 1, INT_MAX / 2,
							PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gaps.max_total_cells",
							"Cells inference may hold at once for one component.",
							NULL,
							&gaps_max_total_cells,
							1 << 26, 1, INT_MAX / 2,
							PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("gaps.share_components",
							 "Solve identical components once (first-order sharing).",
							 "Turning this off solves every component separately; "
							 "the answers are the same.",
							 &gaps_share_components,
							 true,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);
	MarkGUCPrefixReserved("gaps");
}
