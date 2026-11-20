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
#include "access/xact.h"
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
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
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
missing_values(Oid rel, const char *attname, double *out)
{
	HeapTuple	tup;
	Form_pg_class classform;
	double		reltuples;
	AttrNumber	attnum;
	Oid			user = GetUserId();

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
estimate_rows(GapsSrf srf, Datum model, double *rows)
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
		"ON a.model_id OPERATOR(pg_catalog.=) m.model_id "
		"WHERE m.name OPERATOR(pg_catalog.=) $1";
	Oid			argtype = TEXTOID;
	const char *sql = srf == SRF_LEARN_CPTS ? factors_sql : attributes_sql;
	TupleDesc	desc;
	bool		isnull;

	if (SPI_execute_with_args(sql, 1, &argtype, &model, NULL, true, 0) !=
		SPI_OK_SELECT || SPI_processed == 0)
		return false;
	desc = SPI_tuptable->tupdesc;

	if (srf == SRF_LEARN_CPTS)
	{
		*rows = DatumGetFloat8(SPI_getbinval(SPI_tuptable->vals[0], desc, 1,
											 &isnull));
		return true;
	}

	/*
	 * One row per attribute, or one row of NULLs for a model with no
	 * attributes.  Every attribute's random variables are all of its
	 * entities, whatever its stripes, so stripe predicates do not enter.
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
		if (!missing_values(rel, SPI_getvalue(tup, desc, 2), &missing))
			return false;
		*rows += srf == SRF_MARGINALS ? missing * card : missing;
	}
	return true;
}

/*
 * Run estimate_rows() in a subtransaction, turning any error into "no
 * estimate": planning must not fail because, say, the caller lacks SELECT
 * on gaps.model or a model's owner role was dropped.  A query cancel or
 * statement timeout is still raised, since swallowing it would make the
 * statement unkillable for the duration.
 */
static bool
estimate_rows_safely(GapsSrf srf, Datum model, double *rows)
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
		ok = estimate_rows(srf, model, rows);
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
 * The support function.  Answers SupportRequestRows for a call whose first
 * argument, the model name, is a non-NULL constant; returns NULL (no
 * answer) for every other request.
 */
Datum
gaps_rows_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	SupportRequestRows *req;
	char	   *fname;
	GapsSrf		srf;
	Node	   *arg;
	double		rows;

	/* A subtransaction cannot start in parallel mode. */
	if (!IsA(rawreq, SupportRequestRows) || IsInParallelMode())
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

	arg = linitial(((FuncExpr *) req->node)->args);
	if (req->root != NULL)
		arg = estimate_expression_value(req->root, arg);
	if (!IsA(arg, Const) || ((Const *) arg)->constisnull ||
		((Const *) arg)->consttype != TEXTOID)
		PG_RETURN_POINTER(NULL);

	/*
	 * Row-level security makes the answer depend on the role, so a cached
	 * plan must be replanned for another role rather than show it this
	 * role's estimate.
	 */
	if (req->root != NULL)
		req->root->glob->dependsOnRole = true;

	if (!estimate_rows_safely(srf, ((Const *) arg)->constvalue, &rows))
		PG_RETURN_POINTER(NULL);
	req->rows = rows;
	PG_RETURN_POINTER(req);
}
