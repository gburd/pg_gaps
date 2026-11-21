/*-------------------------------------------------------------------------
 *
 * gaps_support.c
 *	  Planner support for the set-returning functions: row estimates.
 *
 * gaps._rows_support() is the SUPPORT function of gaps.marginals(),
 * gaps.impute() and gaps.learn_cpts().  For SupportRequestRows it
 * estimates how many rows the call returns, from the model catalog and the
 * planner statistics, never from user data (doc/DESIGN.md, "Row
 * estimates"):
 *
 *	 marginals:  sum over attributes of reltuples * null_frac * |domain|
 *	 impute:	 sum over attributes of reltuples * null_frac
 *	 learn_cpts: the number of factors
 *
 * For a composite model the attributes are its own and its members'
 * (gaps._models), so the estimate is the members' sum plus its own.
 * learn_cpts refuses a composite, so its estimate stays the model's own
 * factors.
 *
 * A restricted call (rel and entities given) to marginals or impute gets
 * the number of requested entities times the same sum over rel's
 * attributes alone, the rows its requested entities return on average,
 * when both are constants; otherwise no estimate.
 *
 * It runs during planning, so it must neither fail nor be slow.  The model
 * argument must be a constant (after estimate_expression_value, so a bound
 * parameter of a custom plan counts); anything else, a model the caller
 * cannot see, a relation without statistics, or any error at all gives no
 * estimate, and the planner uses the function's declared ROWS.
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/rls.h"
#include "utils/syscache.h"

PG_FUNCTION_INFO_V1(gaps_rows_support);

/* Which set-returning function a request is for. */
typedef enum
{
	SRF_MARGINALS,
	SRF_IMPUTE,
	SRF_LEARN_CPTS
} GapsSrf;

/*
 * The expected number of missing values of rel.attname: reltuples times the
 * column's null fraction.  Returns false, with no estimate, when the
 * relation or column is gone, has inheritance children, has never been
 * analyzed, or the caller may not read the column's statistics.
 *
 * The statistics are read through the syscache, which checks no privileges,
 * so this applies the rule of the pg_stats view: the caller has SELECT on
 * the column (or the table), and row-level security is not active on the
 * relation for the caller.  That is the rule PostgreSQL itself uses to
 * decide which statistics a user may see; the planner's own rule
 * (all_rows_selectable() in selfuncs.c) is about running leaky operators
 * against them and needs a range table entry we do not have.  reltuples is
 * readable by everyone in pg_class, so it needs no check.
 */
static bool
missing_values(PlannerGlobal *glob, Oid rel, const char *attname,
			   double *out)
{
	HeapTuple	tup;
	Form_pg_class classform;
	double		reltuples;
	AttrNumber	attnum;
	Oid			user = GetUserId();

	/*
	 * The estimate depends on the relation's statistics and privileges, so a
	 * cached plan depends on the relation: ANALYZE, GRANT and REVOKE send a
	 * relcache invalidation that replans it.  It is recorded before any
	 * check, so a plan that got no estimate also replans after a GRANT.  We
	 * run in SPI's memory context, so the list is extended in the planner's,
	 * the context that holds glob.
	 */
	if (glob != NULL)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(glob));
		glob->relationOids = lappend_oid(glob->relationOids, rel);
		MemoryContextSwitchTo(oldcontext);
	}

	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(rel));
	if (!HeapTupleIsValid(tup))
		return false;
	classform = (Form_pg_class) GETSTRUCT(tup);
	reltuples = classform->reltuples;
	ReleaseSysCache(tup);

	/*
	 * shortcut: a relation with inheritance children or partitions is
	 * refused, because grounding reads its children too and its own
	 * reltuples does not count them; sum the children if users need it.
	 */
	if (reltuples < 0 || has_subclass(rel))
		return false;

	attnum = get_attnum(rel, attname);
	if (attnum == InvalidAttrNumber)
		return false;
	if (pg_class_aclcheck(rel, user, ACL_SELECT) != ACLCHECK_OK &&
		pg_attribute_aclcheck(rel, attnum, user, ACL_SELECT) != ACLCHECK_OK)
		return false;
	if (check_enable_rls(rel, InvalidOid, true) == RLS_ENABLED)
		return false;

	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(rel),
						  Int16GetDatum(attnum), BoolGetDatum(false));
	if (!HeapTupleIsValid(tup))
		return false;
	*out = reltuples * ((Form_pg_statistic) GETSTRUCT(tup))->stanullfrac;
	ReleaseSysCache(tup);
	return true;
}

/*
 * Estimate the rows of one call for the named model.  Runs read-only SPI
 * queries on gaps.*, as the caller, so the catalog's row-level security
 * hides a model the caller may not read exactly as it does from the
 * function itself.  Returns false for no estimate.  The caller has
 * connected to SPI and catches errors.
 */
static bool
estimate_rows(GapsSrf srf, Datum *args, PlannerGlobal *glob, double *rows)
{
	static const char *const factors_sql =
		"SELECT (SELECT pg_catalog.count(*) FROM gaps.factor f "
		"WHERE f.model_id OPERATOR(pg_catalog.=) m.model_id)"
		"::pg_catalog.float8 "
		"FROM gaps.model m WHERE m.name OPERATOR(pg_catalog.=) $1";
	static const char *const attributes_sql =
		"SELECT a.rel::pg_catalog.oid, a.attname::pg_catalog.text, "
		"pg_catalog.cardinality(a.domain)::pg_catalog.float8 "
		"FROM gaps.model m LEFT JOIN gaps.attribute a "
		"ON a.model_id OPERATOR(pg_catalog.=) ANY (gaps._models(m.model_id)) "
		"WHERE m.name OPERATOR(pg_catalog.=) $1";
	static const char *const restricted_sql =
		"SELECT a.rel::pg_catalog.oid, a.attname::pg_catalog.text, "
		"pg_catalog.cardinality(a.domain)::pg_catalog.float8, "
		"pg_catalog.jsonb_array_length($3)::pg_catalog.float8 "
		"FROM gaps.model m LEFT JOIN gaps.attribute a "
		"ON a.model_id OPERATOR(pg_catalog.=) ANY (gaps._models(m.model_id)) "
		"AND a.rel OPERATOR(pg_catalog.=) $2 "
		"WHERE m.name OPERATOR(pg_catalog.=) $1";
	Oid			argtypes[3] = {TEXTOID, REGCLASSOID, JSONBOID};
	bool		restricted = args[2] != (Datum) 0;
	const char *sql = srf == SRF_LEARN_CPTS ? factors_sql :
		restricted ? restricted_sql : attributes_sql;
	TupleDesc	desc;
	bool		isnull;
	double		nentities = 1;

	if (SPI_execute_with_args(sql, restricted ? 3 : 1, argtypes, args, NULL,
							  true, 0) != SPI_OK_SELECT || SPI_processed == 0)
		return false;
	desc = SPI_tuptable->tupdesc;

	if (srf == SRF_LEARN_CPTS)
	{
		*rows = DatumGetFloat8(SPI_getbinval(SPI_tuptable->vals[0], desc, 1,
											 &isnull));
		return true;
	}

	/*
	 * jsonb_array_length fails on a non-array, which the caller turns into
	 * no estimate, as the call itself will fail.
	 */
	if (restricted)
		nentities = DatumGetFloat8(SPI_getbinval(SPI_tuptable->vals[0], desc,
												 4, &isnull));

	/*
	 * One row per attribute, or one row of NULLs for a model with no
	 * attributes (of rel, for a restricted call).  Every attribute's random
	 * variables are all of its entities, whatever its stripes, so stripe
	 * predicates do not enter.
	 */
	*rows = 0;
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		HeapTuple	tup = SPI_tuptable->vals[i];
		Oid			rel;
		double		card;
		double		missing;

		CHECK_FOR_INTERRUPTS();
		rel = DatumGetObjectId(SPI_getbinval(tup, desc, 1, &isnull));
		if (isnull)
			continue;
		card = DatumGetFloat8(SPI_getbinval(tup, desc, 3, &isnull));
		if (!missing_values(glob, rel, SPI_getvalue(tup, desc, 2), &missing))
			return false;
		*rows += srf == SRF_MARGINALS ? missing * card : missing;
	}

	/*
	 * Each requested entity is one of rel's, so it has, on average, the
	 * missing values of rel's attributes divided by its row count.
	 */
	if (restricted)
	{
		double		reltuples = 0;
		HeapTuple	tup = SearchSysCache1(RELOID, args[1]);

		if (HeapTupleIsValid(tup))
		{
			reltuples = ((Form_pg_class) GETSTRUCT(tup))->reltuples;
			ReleaseSysCache(tup);
		}
		if (reltuples <= 0)
			return false;
		*rows = *rows / reltuples * nentities;
	}
	return true;
}

/*
 * Take AccessShareLock on relid and on each of its indexes without waiting.
 * Returns false, leaving any locks already granted in place, if the relation
 * is gone or a lock is not free right now.  Sets *toastrelid to the
 * relation's TOAST table, or InvalidOid.
 */
static bool
lock_with_indexes(Oid relid, Oid *toastrelid)
{
	Relation	rel;
	List	   *indexes;
	ListCell   *lc;
	bool		ok = true;

	if (!ConditionalLockRelationOid(relid, AccessShareLock))
		return false;
	rel = try_relation_open(relid, NoLock);
	if (rel == NULL)
		return false;
	*toastrelid = rel->rd_rel->reltoastrelid;
	indexes = RelationGetIndexList(rel);
	relation_close(rel, NoLock);
	foreach(lc, indexes)
	{
		if (!ConditionalLockRelationOid(lfirst_oid(lc), AccessShareLock))
		{
			ok = false;
			break;
		}
	}
	list_free(indexes);
	return ok;
}

/*
 * Lock gaps.<relname>, its TOAST table, and their indexes without waiting,
 * and make cached plans depend on the table.  Returns false if the table is
 * missing or any lock is not free right now.
 *
 * These are the locks the lookup's SPI queries take anyway (the planner
 * opens the indexes, detoasting opens the TOAST table), taken first so that
 * planning never waits for them: a plain EXPLAIN must not block behind
 * someone's LOCK or REINDEX of the catalog.  Locks granted are kept to the
 * end of the transaction, as the queries' own locks would be.
 *
 * The dependency replans on DDL, VACUUM or ANALYZE of the table, not on
 * ordinary inserts and updates, which send no relcache invalidation for a
 * user table.  So a cached plan's estimate can lag an edit of the model
 * (doc/DESIGN.md, "Row estimates").
 */
static bool
lock_catalog_table(Oid nsp, const char *relname, PlannerGlobal *glob)
{
	Oid			relid = get_relname_relid(relname, nsp);
	Oid			toastrelid;
	Oid			unused;

	if (!OidIsValid(relid))
		return false;
	if (glob != NULL)
		glob->relationOids = lappend_oid(glob->relationOids, relid);
	if (!lock_with_indexes(relid, &toastrelid))
		return false;
	return !OidIsValid(toastrelid) || lock_with_indexes(toastrelid, &unused);
}

/*
 * Run estimate_rows() in a subtransaction, turning any error into "no
 * estimate": planning must not fail because, say, the caller lacks SELECT
 * on gaps.model or a model's owner role was dropped.  A query cancel or
 * statement timeout is still raised, since swallowing it would make the
 * statement unkillable for the duration.
 */
static bool
estimate_rows_safely(GapsSrf srf, Datum *args, PlannerGlobal *glob,
					 double *rows)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	volatile bool ok = false;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		ok = estimate_rows(srf, args, glob, rows);
		SPI_finish();
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED)
			ReThrowError(edata);
		FreeErrorData(edata);
		ok = false;
	}
	PG_END_TRY();
	return ok;
}

/*
 * Argument n of the call as a constant of type typid, after
 * estimate_expression_value.  Returns false if it is not one; sets *isnull
 * and, when not null, *value.  An argument the call does not have counts as
 * NULL.
 */
static bool
const_arg(SupportRequestRows *req, int n, Oid typid, Datum *value, bool *isnull)
{
	List	   *args = ((FuncExpr *) req->node)->args;
	Node	   *arg;

	if (list_length(args) <= n)
	{
		/* The planner expands defaults, but a missing one is NULL. */
		*isnull = true;
		return true;
	}
	arg = list_nth(args, n);
	if (req->root != NULL)
		arg = estimate_expression_value(req->root, arg);
	if (!IsA(arg, Const) || ((Const *) arg)->consttype != typid)
		return false;
	*isnull = ((Const *) arg)->constisnull;
	*value = ((Const *) arg)->constvalue;
	return true;
}

/*
 * The support function.  Answers SupportRequestRows for a call whose first
 * argument, the model name, is a non-NULL constant, and whose rel and
 * entities arguments, if it has them, are constants, both NULL or both
 * not; returns NULL (no answer) for every other request.
 */
Datum
gaps_rows_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	SupportRequestRows *req;
	char	   *fname;
	GapsSrf		srf;
	PlannerGlobal *glob;
	Oid			nsp;
	double		rows;
	Datum		args[3] = {0, 0, 0};
	bool		isnull;
	bool		relnull = true;
	bool		entnull = true;

	/*
	 * No estimate in parallel mode: the worker has no use for one, the
	 * leader having planned the query, and planning there should stay cheap
	 * and inert.  No estimate during recovery either: a recovery conflict
	 * that arrives while the lookup runs in its subtransaction terminates
	 * the session rather than cancelling the query (postgres.c promotes it
	 * to FATAL inside a subtransaction), and a row estimate is not worth a
	 * session on a hot standby.  And none under SERIALIZABLE, where the
	 * lookup's reads would take predicate locks on the catalog, so that
	 * merely planning (an EXPLAIN, a branch never executed) could make a
	 * transaction fail to serialize against a concurrent model edit.
	 */
	if (!IsA(rawreq, SupportRequestRows) || IsInParallelMode() ||
		RecoveryInProgress() || IsolationIsSerializable())
		PG_RETURN_POINTER(NULL);
	req = (SupportRequestRows *) rawreq;
	if (!is_funcclause(req->node) || ((FuncExpr *) req->node)->args == NIL)
		PG_RETURN_POINTER(NULL);

	fname = get_func_name(req->funcid);
	if (fname == NULL)
		PG_RETURN_POINTER(NULL);
	if (strcmp(fname, "marginals") == 0)
		srf = SRF_MARGINALS;
	else if (strcmp(fname, "impute") == 0)
		srf = SRF_IMPUTE;
	else if (strcmp(fname, "learn_cpts") == 0)
		srf = SRF_LEARN_CPTS;
	else
		PG_RETURN_POINTER(NULL);

	if (!const_arg(req, 0, TEXTOID, &args[0], &isnull) || isnull)
		PG_RETURN_POINTER(NULL);
	if (srf != SRF_LEARN_CPTS &&
		(!const_arg(req, 1, REGCLASSOID, &args[1], &relnull) ||
		 !const_arg(req, 2, JSONBOID, &args[2], &entnull) ||
		 relnull != entnull))
		PG_RETURN_POINTER(NULL);
	if (entnull)
		args[1] = args[2] = (Datum) 0;

	/*
	 * Row-level security makes the answer depend on the role, so a cached
	 * plan must be replanned for another role rather than show it this
	 * role's estimate.
	 */
	glob = req->root != NULL ? req->root->glob : NULL;
	if (glob != NULL)
		glob->dependsOnRole = true;

	/*
	 * The lookup's tables, locked without waiting.  The lookup reads
	 * gaps.model, and gaps.attribute and gaps.model_member (through
	 * gaps._models) or gaps.factor; the catalog's policies read only
	 * gaps.model.
	 */
	nsp = get_namespace_oid("gaps", true);
	if (!OidIsValid(nsp) || !lock_catalog_table(nsp, "model", glob) ||
		!lock_catalog_table(nsp, srf == SRF_LEARN_CPTS ? "factor" :
							"attribute", glob) ||
		(srf != SRF_LEARN_CPTS && !lock_catalog_table(nsp, "model_member", glob)))
		PG_RETURN_POINTER(NULL);

	if (!estimate_rows_safely(srf, args, glob, &rows))
		PG_RETURN_POINTER(NULL);
	req->rows = rows;
	PG_RETURN_POINTER(req);
}
