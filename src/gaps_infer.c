/*-------------------------------------------------------------------------
 *
 * gaps_infer.c
 *	  SQL entry point to the inference engine.
 *
 * gaps._infer() takes a ground Bayesian network as flat arrays, which the
 * SQL grounding in pg_gaps--*.sql builds from the model catalog and the
 * user's tables, runs the engine on it, and returns one row per value of
 * every missing variable.  It knows nothing about models or tables; its
 * whole job is to check its arguments, hand them to the engine with
 * PostgreSQL's memory and interrupt handling, and turn engine errors into
 * ERRORs.
 *
 * The function is callable from SQL, so every array is validated here
 * before the engine sees it: lengths agree, indexes are in range, and each
 * factor's CPT has exactly the length its variables' cardinalities imply.
 * The engine itself checks parents, entries and cycles (gx_check).
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "engine/gx.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

#include "pg_gaps.h"

PG_FUNCTION_INFO_V1(gaps_infer);
PG_FUNCTION_INFO_V1(gaps_expected_counts);

/* GUCs, registered in _PG_init. */
int			gaps_max_factor_cells = 1 << 20;
int			gaps_max_total_cells = 1 << 26;
bool		gaps_share_components = true;
int			gaps_samples = 0;
int			gaps_seed = 0;

static void *
pg_alloc(void *arg, size_t size)
{
	(void) arg;
	return MemoryContextAllocHuge(CurrentMemoryContext, size);
}

static void
pg_free(void *arg, void *ptr)
{
	(void) arg;
	pfree(ptr);
}

static void
pg_interrupt(void *arg)
{
	(void) arg;
	CHECK_FOR_INTERRUPTS();
}

/*
 * Deconstruct a one-dimensional int4[] or float8[] argument with no NULLs.
 * Returns the element count and sets *out to palloc'd C values.
 */
static int
int_array(ArrayType *a, const char *what, int **out)
{
	Datum	   *d;
	bool	   *nulls;
	int			n;

	if (ARR_NDIM(a) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("%s must be a one-dimensional array", what)));
	/* Not deconstruct_array_builtin: before 17.8 it does not know INT4OID. */
	deconstruct_array(a, INT4OID, sizeof(int32), true, TYPALIGN_INT, &d, &nulls, &n);
	*out = palloc(sizeof(int) * (n ? n : 1));
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("%s must not contain NULL", what)));
		(*out)[i] = DatumGetInt32(d[i]);
	}
	return n;
}

static int
float_array(ArrayType *a, const char *what, double **out)
{
	Datum	   *d;
	bool	   *nulls;
	int			n;

	if (ARR_NDIM(a) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("%s must be a one-dimensional array", what)));
	deconstruct_array(a, FLOAT8OID, sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE,
					  &d, &nulls, &n);
	*out = palloc(sizeof(double) * (n ? n : 1));
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("%s must not contain NULL", what)));
		(*out)[i] = DatumGetFloat8(d[i]);
	}
	return n;
}

static void
bad_arg(const char *fmt, int a, int b)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg_internal(fmt, a, b)));
}

/*
 * gaps._infer(card int[], evidence int[], child int[], parent_off int[],
 *			   parents int[], cpt_off int[], cpts float8[],
 *			   cpt_lscale int[] DEFAULT '{}')
 * RETURNS TABLE (var int, value int, probability float8, component int,
 *				  map_value int, map_probability float8, samples int)
 *
 * Variables are numbered 1..n (SQL style); card[i] and evidence[i] (NULL is
 * not allowed: missing is 0, observed is the 1-based value) describe
 * variable i.  There is one CPT per variable: entry j of child[] names its
 * child, and its parents are parents[parent_off[j] .. parent_off[j+1]-1]
 * (parent_off has n+1 elements).  Its values start at cpts[cpt_off[j]]
 * and run for as many entries as its variables' cardinalities imply, so
 * every instance of a first-order factor points at the same values.  All
 * offsets are 1-based.  cpt_lscale is empty, or has one entry per CPT: 0
 * for a table of probabilities, or the offset in cpts of the card(child)
 * log offsets of a log CPT, whose values are then logs (gx.h); the
 * parameterised factors' tables are log CPTs (engine/gx_param.h).
 *
 * samples is NULL for a component solved exactly, and otherwise the number
 * of Gibbs samples per chain behind its probabilities; map_value is then each
 * variable's most probable sampled value, and map_probability is NULL,
 * since sampling estimates no joint probability.
 */
/*
 * Read and check gaps._infer's arrays and build the engine's network from
 * them, in the engine's 0-based form, and set *cpt_off to the checked
 * cpt_off array (1-based, one per CPT).  Everything is palloc'd in the
 * current context.
 */
static void
build_network(FunctionCallInfo fcinfo, gx_bn *bn, const int **cpt_off)
{
	int		   *card,
			   *evid,
			   *child,
			   *poff,
			   *par,
			   *coff,
			   *lsc;
	double	   *vals;
	int			n,
				nev,
				ncpt,
				npoff,
				npar,
				ncoff,
				nvals,
				nlsc;
	gx_cpt	   *cpts;

	n = int_array(PG_GETARG_ARRAYTYPE_P(0), "card", &card);
	nev = int_array(PG_GETARG_ARRAYTYPE_P(1), "evidence", &evid);
	ncpt = int_array(PG_GETARG_ARRAYTYPE_P(2), "child", &child);
	npoff = int_array(PG_GETARG_ARRAYTYPE_P(3), "parent_off", &poff);
	npar = int_array(PG_GETARG_ARRAYTYPE_P(4), "parents", &par);
	ncoff = int_array(PG_GETARG_ARRAYTYPE_P(5), "cpt_off", &coff);
	nvals = float_array(PG_GETARG_ARRAYTYPE_P(6), "cpts", &vals);
	nlsc = int_array(PG_GETARG_ARRAYTYPE_P(7), "cpt_lscale", &lsc);

	if (nev != n || ncpt != n)
		bad_arg("card, evidence and child must have the same length (%d, %d)", n,
				nev != n ? nev : ncpt);
	if (npoff != n + 1 || ncoff != n)
		bad_arg("parent_off needs %d elements and cpt_off %d", n + 1, n);
	if (nlsc != 0 && nlsc != n)
		bad_arg("cpt_lscale needs 0 or %d elements, got %d", n, nlsc);
	if (n > 0 && (poff[0] != 1 || poff[n] != npar + 1))
		bad_arg("parent_off must run from 1 to %d, got last %d", npar + 1, poff[n]);

	for (int v = 0; v < n; v++)
	{
		if (card[v] < 1)
			bad_arg("variable %d has cardinality %d", v + 1, card[v]);
		if (evid[v] < 0 || evid[v] > card[v])
			bad_arg("variable %d has evidence %d", v + 1, evid[v]);
		evid[v] -= 1;
	}
	/* Huge: at 32 bytes a CPT, past 33.5M variables this passes 1 GB. */
	cpts = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(gx_cpt) * (n ? n : 1));
	for (int j = 0; j < n; j++)
	{
		int			c = child[j];
		int			np = poff[j + 1] - poff[j];
		int64		need;

		if (c < 1 || c > n)
			bad_arg("CPT %d names child %d", j + 1, c);
		if (np < 0 || poff[j] < 1 || poff[j + 1] > npar + 1)
			bad_arg("CPT %d has a bad parent range ending at %d", j + 1, poff[j + 1]);
		need = card[c - 1];
		for (int k = poff[j] - 1; k < poff[j + 1] - 1; k++)
		{
			if (par[k] < 1 || par[k] > n)
				bad_arg("CPT %d names parent %d", j + 1, par[k]);
			par[k] -= 1;
			need *= card[par[k]];
			if (need > gaps_max_factor_cells)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("a CPT would have more than %d cells", gaps_max_factor_cells),
						 errhint("Raise gaps.max_factor_cells, or give the factor fewer parents.")));
		}
		if (coff[j] < 1 || (int64) coff[j] - 1 + need > nvals)
			bad_arg("CPT %d needs %d values from its offset", j + 1, (int) need);
		if (nlsc && (lsc[j] < 0 ||
					 (lsc[j] > 0 && (int64) lsc[j] - 1 + card[c - 1] > nvals)))
			bad_arg("CPT %d needs %d log offsets from its cpt_lscale", j + 1, card[c - 1]);
		cpts[j].child = c - 1;
		cpts[j].nparents = np;
		cpts[j].parents = &par[poff[j] - 1];
		cpts[j].vals = &vals[coff[j] - 1];
		cpts[j].lscale = nlsc && lsc[j] > 0 ? &vals[lsc[j] - 1] : NULL;
	}

	bn->nvars = n;
	bn->card = card;
	bn->evidence = evid;
	bn->ncpts = n;
	bn->cpts = cpts;
	*cpt_off = coff;
}

/*
 * Turn an engine error into an ERROR.  bad is the variable the engine
 * names; res, when not NULL, is gx_infer's result, which also carries the
 * failing component's size and, for GX_ENOMIX, the chains' disagreement.
 */
static void
report(int rc, int bad, const gx_result *res)
{
	if (rc == GX_ECYCLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the ground network has a directed cycle through variable %d",
						bad + 1)));
	if (rc == GX_EZERO)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("the observed values have probability zero under the model"),
				 errdetail("Variable %d is involved.", bad + 1),
				 errhint("A CPT gives zero probability to data that is present.")));
	if (rc == GX_ETOOBIG && gaps_samples == 0)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("inference needs a factor larger than gaps.max_factor_cells (%d) "
						"or more than gaps.max_total_cells (%d)",
						gaps_max_factor_cells, gaps_max_total_cells),
				 errdetail("The component containing variable %d is too densely connected "
						   "for exact inference.", bad + 1),
				 errhint("Raise the limits, or set gaps.samples to sample the component.")));
	if (rc == GX_ETOOBIG)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("inference needs more than gaps.max_total_cells (%d) to hold the "
						"component's factors, or to sample it", gaps_max_total_cells),
				 errdetail("The component containing variable %d is too large.", bad + 1)));
	if (rc == GX_ENOTPOS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("the component containing variable %d is too large for exact "
						"inference and cannot be sampled", bad + 1),
				 errdetail("Gibbs sampling needs every CPT entry the component uses to be "
						   "positive, and one is zero."),
				 errhint("Raise gaps.max_factor_cells or gaps.max_total_cells, or make the "
						 "CPT entries positive (gaps.learn_cpts with alpha > 0 does).")));
	if (rc == GX_ENOMIX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("Gibbs sampling did not converge on the component of %d variables "
						"containing variable %d", res->bad_size, bad + 1),
				 errdetail("With gaps.samples = %d in each of %d chains, two chains "
						   "estimate a probability %.3f apart; the limit is %.2f.",
						   gaps_samples, GX_CHAINS, res->spread, GX_MIX_SPREAD),
				 errhint("Raise gaps.samples, or raise gaps.max_factor_cells and "
						 "gaps.max_total_cells so the component is solved exactly.")));
	if (rc == GX_EINVAL)
		bad_arg("malformed ground network at variable %d (code %d)", bad + 1, rc);
	if (rc != GX_OK)
		elog(ERROR, "inference engine failed with code %d", rc);
}

/* The engine's services and bounds, from the GUCs. */
static void
make_env(gx_env *env)
{
	env->alloc = pg_alloc;
	env->free = pg_free;
	env->interrupt = pg_interrupt;
	env->arg = NULL;
	env->max_cells = (size_t) gaps_max_factor_cells;
	env->max_total_cells = (size_t) gaps_max_total_cells;
	env->no_sharing = !gaps_share_components;
	env->samples = gaps_samples;
	env->seed = (uint64_t) gaps_seed;
	env->sample_always = 0;
}

Datum
gaps_infer(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	gx_bn		bn;
	gx_env		env;
	gx_result	res;
	const int  *coff;
	int			bad;
	int			rc;
	MemoryContext work,
				old;

	gaps_check_version(true);
	for (int i = 0; i < 8; i++)
		if (PG_ARGISNULL(i))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("gaps._infer arguments must not be NULL")));

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * Everything the engine allocates goes into one context, released at the
	 * end; if the engine longjmps out (an interrupt, an out-of-memory), the
	 * context is a child of the query's and goes with it.
	 */
	work = AllocSetContextCreate(CurrentMemoryContext, "pg_gaps inference",
								 ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(work);

	build_network(fcinfo, &bn, &coff);
	make_env(&env);

	/*
	 * The engine call must finish before bad or res is read: C does not
	 * order the evaluation of a call's arguments.
	 */
	rc = gx_check(&env, &bn, &bad);
	report(rc, bad, NULL);
	rc = gx_infer(&env, &bn, &res);
	report(rc, res.bad_var, &res);

	MemoryContextSwitchTo(old);
	for (int v = 0; v < bn.nvars; v++)
	{
		if (bn.evidence[v] >= 0)
			continue;
		for (int x = 0; x < bn.card[v]; x++)
		{
			int			k = res.comp[v];
			Datum		values[7];
			bool		nulls[7] = {0};

			values[0] = Int32GetDatum(v + 1);
			values[1] = Int32GetDatum(x + 1);
			values[2] = Float8GetDatum(res.marg[res.marg_off[v] + (size_t) x]);
			values[3] = Int32GetDatum(k + 1);
			values[4] = Int32GetDatum(res.map[v] + 1);
			values[5] = Float8GetDatum(res.comp_map_prob[k]);
			values[6] = Int32GetDatum(res.comp_samples[k]);
			nulls[5] = res.comp_samples[k] > 0;
			nulls[6] = res.comp_samples[k] == 0;
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	MemoryContextDelete(work);
	PG_RETURN_NULL();
}

static int
int_cmp(const void *a, const void *b)
{
	int			x = *(const int *) a,
				y = *(const int *) b;

	return (x > y) - (x < y);
}

/*
 * The groups of gaps._expected_counts: one per distinct cpt_off, numbered
 * in increasing offset, into group[] (per CPT) and *goffs (the offsets,
 * ascending).  Each group's size is its first CPT's; the engine refuses a
 * CPT of another size.  Errors if all the groups' cells together exceed
 * gaps.max_total_cells.  Returns the number of groups.
 */
static int
derive_groups(const gx_bn *bn, const int *coff, int *group, int **goffs,
			  size_t **gsize)
{
	int		   *u = palloc(sizeof(int) * (bn->ncpts ? bn->ncpts : 1));
	int			ng = 0;
	size_t		total = 0;

	memcpy(u, coff, sizeof(int) * bn->ncpts);
	qsort(u, bn->ncpts, sizeof(int), int_cmp);
	for (int i = 0; i < bn->ncpts; i++)
		if (ng == 0 || u[ng - 1] != u[i])
			u[ng++] = u[i];
	*gsize = palloc0(sizeof(size_t) * (ng ? ng : 1));
	for (int i = 0; i < bn->ncpts; i++)
	{
		const gx_cpt *c = &bn->cpts[i];
		int		   *hit = bsearch(&coff[i], u, ng, sizeof(int), int_cmp);
		size_t		n = (size_t) bn->card[c->child];

		group[i] = (int) (hit - u);
		if ((*gsize)[group[i]] != 0)
			continue;
		/* build_network bounded each CPT by gaps.max_factor_cells. */
		for (int j = 0; j < c->nparents; j++)
			n *= (size_t) bn->card[c->parents[j]];
		(*gsize)[group[i]] = n;
		total += n;
		if (total > (size_t) gaps_max_total_cells)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("the CPTs together have more than gaps.max_total_cells (%d) cells",
							gaps_max_total_cells)));
	}
	*goffs = u;
	return ng;
}

/*
 * gaps._expected_counts(card int[], evidence int[], child int[],
 *						 parent_off int[], parents int[], cpt_off int[],
 *						 cpts float8[], cpt_lscale int[] DEFAULT '{}')
 * RETURNS TABLE (cpt_off int, cell int, n float8, loglik float8)
 *
 * The E-step of EM over the ground network gaps._infer takes (same
 * arguments, same checks).  CPTs that share a cpt_off are instances of
 * one first-order factor.  For each such offset and each cell (1-based,
 * in the CPT's own order), n is the sum over its instances of the
 * posterior probability that the instance's child and parents take that
 * cell's values, given the evidence; an instance with all of them
 * observed adds 1 to its cell.  loglik, the same on every row, is log
 * P(evidence) under the given CPTs, log offsets included.  Exact inference
 * only: a component too large for it is an ERROR whatever gaps.samples
 * says.
 */
Datum
gaps_expected_counts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	gx_bn		bn;
	gx_env		env;
	gx_counts	ec;
	const int  *coff;
	int		   *group,
			   *goffs;
	size_t	   *gsize;
	size_t		off = 0;
	int			bad;
	int			rc;
	MemoryContext work,
				old;

	gaps_check_version(true);
	for (int i = 0; i < 8; i++)
		if (PG_ARGISNULL(i))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("gaps._expected_counts arguments must not be NULL")));

	InitMaterializedSRF(fcinfo, 0);
	work = AllocSetContextCreate(CurrentMemoryContext, "pg_gaps expected counts",
								 ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(work);

	build_network(fcinfo, &bn, &coff);
	make_env(&env);
	group = palloc(sizeof(int) * (bn.ncpts ? bn.ncpts : 1));
	memset(&ec, 0, sizeof(ec));
	ec.group = group;
	ec.ngroups = derive_groups(&bn, coff, group, &goffs, &gsize);
	ec.group_size = gsize;
	for (int g = 0; g < ec.ngroups; g++)
		off += gsize[g];
	ec.counts = palloc(sizeof(double) * (off ? off : 1));

	rc = gx_check(&env, &bn, &bad);
	report(rc, bad, NULL);
	rc = gx_expected_counts(&env, &bn, &ec);
	if (rc == GX_EINVAL)
		bad_arg("CPTs sharing a cpt_off must have the same size (CPT of variable %d)",
				ec.bad_var + 1, 0);
	if (rc == GX_ETOOBIG)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("expected counts need a factor larger than gaps.max_factor_cells (%d) "
						"or more than gaps.max_total_cells (%d)",
						gaps_max_factor_cells, gaps_max_total_cells),
				 errdetail("The component of %d variables containing variable %d is too "
						   "large for exact inference, and expected counts are not sampled.",
						   ec.bad_size, ec.bad_var + 1),
				 errhint("Raise the limits.")));
	report(rc, ec.bad_var, NULL);

	MemoryContextSwitchTo(old);
	off = 0;
	for (int g = 0; g < ec.ngroups; g++)
		for (size_t x = 0; x < gsize[g]; x++)
		{
			Datum		values[4];
			bool		nulls[4] = {0};

			values[0] = Int32GetDatum(goffs[g]);
			values[1] = Int32GetDatum((int) x + 1);
			values[2] = Float8GetDatum(ec.counts[off++]);
			values[3] = Float8GetDatum(ec.loglik);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	MemoryContextDelete(work);
	PG_RETURN_NULL();
}
