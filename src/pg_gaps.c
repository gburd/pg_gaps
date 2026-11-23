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
 * This file holds the module magic block, the GUCs, and the check that
 * the installed extension's SQL matches this library.  gaps_infer.c is
 * the SQL entry point to the inference engine in engine/; the rest of the
 * SQL interface is in pg_gaps--*.sql.
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_extension.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/syscache.h"

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
 * Whether the installed extension's version has been found to equal
 * PG_GAPS_VERSION in this backend.  Cleared whenever pg_extension changes,
 * so an ALTER EXTENSION in this or another session is seen.
 */
static bool version_checked = false;

#if PG_VERSION_NUM >= 190000
typedef SysCacheIdentifier gaps_cacheid;
#else
typedef int gaps_cacheid;
#endif

static void
version_invalidate(Datum arg, gaps_cacheid cacheid, uint32 hashvalue)
{
	(void) arg;
	(void) cacheid;
	(void) hashvalue;
	version_checked = false;
}

/*
 * The extversion of extension pg_gaps in this database, palloc'd, or NULL
 * when it is not installed.  Reads pg_extension by name, as
 * get_extension_oid() does; there is no syscache on the name.
 */
static char *
installed_version(void)
{
	Relation	rel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	char	   *version = NULL;

	rel = table_open(ExtensionRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_extension_extname, BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum("pg_gaps"));
	scan = systable_beginscan(rel, ExtensionNameIndexId, true, NULL, 1, &key);
	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
	{
		bool		isnull;
		Datum		d = heap_getattr(tup, Anum_pg_extension_extversion,
									 RelationGetDescr(rel), &isnull);

		if (!isnull)
			version = text_to_cstring(DatumGetTextPP(d));
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return version;
}

/*
 * Check that the SQL objects of the installed extension are the version
 * this library was built for.  A library replaced under an extension that
 * was not updated would otherwise be called by the old SQL with arguments
 * it does not expect.  With raise, a mismatch is an ERROR naming both
 * versions; without, it returns false, for callers that must never fail
 * (the planner support function).  The answer is cached for the backend
 * until pg_extension changes.  Note that ALTER EXTENSION UPDATE writes the
 * new extversion before it runs the update script, so the script's own
 * calls pass.
 */
bool
gaps_check_version(bool raise)
{
	char	   *installed;

	if (version_checked)
		return true;
	installed = installed_version();
	if (installed != NULL && strcmp(installed, PG_GAPS_VERSION) == 0)
	{
		version_checked = true;
		pfree(installed);
		return true;
	}
	if (!raise)
		return false;
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("pg_gaps library %s is loaded but the extension's SQL is %s",
					PG_GAPS_VERSION, installed ? installed : "not installed"),
			 errhint("Run ALTER EXTENSION pg_gaps UPDATE.")));
	return false;				/* keep the compiler quiet */
}

/*
 * Register the GUCs.  The two cell limits bound inference (AGENTS.md rule
 * 16): a query whose model would need a larger factor, or more cells held
 * at once, fails with an ERROR naming the setting instead of exhausting
 * memory.  gaps.samples lets such a component be sampled instead, and
 * gaps.seed makes the sample reproducible (rule 15).  Sampling holds
 * memory linear in the component, charged to gaps.max_total_cells; its
 * time is linear in samples times the component's factor scopes, and it
 * is cancellable.  gaps.samples counts sweeps per chain, and the engine
 * runs GX_CHAINS chains; the upper limit keeps a chain's burn-in and
 * sweep count within an int.
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
	DefineCustomIntVariable("gaps.samples",
							"Gibbs samples for a component too large for exact inference.",
							"Per chain; four chains run.  0, the default, never samples: "
							"such a component is an error.",
							&gaps_samples,
							0, 0, INT_MAX / 2,
							PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gaps.seed",
							"Seed for Gibbs sampling.",
							"With the same seed and gaps.samples, sampled answers repeat.",
							&gaps_seed,
							0, 0, INT_MAX,
							PGC_USERSET, 0,
							NULL, NULL, NULL);
	MarkGUCPrefixReserved("gaps");
	CacheRegisterSyscacheCallback(EXTENSIONOID, version_invalidate, (Datum) 0);
}
