/*-------------------------------------------------------------------------
 *
 * gaps_param.c
 *	  SQL entry point to the parameterised-factor grounding (engine/gx_param.h).
 *
 * gaps._param_cpts() takes one parameterised factor and all its ground
 * instances, with each parent's evidence, and returns the instances' CPTs
 * (and any auxiliary variables their aggregating slots need) as the flat
 * arrays gaps._infer takes, for gaps._ground to append to the ordinary
 * factors' arrays.  Like gaps._infer it is callable from SQL, so every
 * array is checked here before the engine module sees it.
 *
 * gaps._param_size() sizes the same tables without building them, so that
 * gaps._ground can bound the whole network before it builds any.
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "engine/gx_param.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/expandedrecord.h"
#include "utils/memutils.h"

#include "pg_gaps.h"

PG_FUNCTION_INFO_V1(gaps_param_cpts);
PG_FUNCTION_INFO_V1(gaps_param_size);

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

/* A one-dimensional array argument with no NULLs, as C values. */
static int
get_ints(FunctionCallInfo fcinfo, int arg, const char *what, int **out)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(arg);
	Datum	   *d;
	bool	   *nulls;
	int			n;

	if (ARR_NDIM(a) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("%s must be a one-dimensional array", what)));
	deconstruct_array_builtin(a, INT4OID, &d, &nulls, &n);
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
get_floats(FunctionCallInfo fcinfo, int arg, const char *what, double **out)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(arg);
	Datum	   *d;
	bool	   *nulls;
	int			n;

	if (ARR_NDIM(a) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("%s must be a one-dimensional array", what)));
	deconstruct_array_builtin(a, FLOAT8OID, &d, &nulls, &n);
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

static int
get_bools(FunctionCallInfo fcinfo, int arg, const char *what, unsigned char **out)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(arg);
	Datum	   *d;
	bool	   *nulls;
	int			n;

	if (ARR_NDIM(a) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("%s must be a one-dimensional array", what)));
	/* deconstruct_array_builtin does not know BOOLOID. */
	deconstruct_array(a, BOOLOID, 1, true, TYPALIGN_CHAR, &d, &nulls, &n);
	*out = palloc(n ? n : 1);
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("%s must not contain NULL", what)));
		(*out)[i] = DatumGetBool(d[i]) ? 1 : 0;
	}
	return n;
}

static void
bad_arg(const char *msg)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg_internal("%s", msg)));
}

/* A growable int buffer for the output arrays. */
typedef struct obuf
{
	size_t		n;
	size_t		cap;
	int		   *data;
} obuf;

/* Append x; the buffers are bounded by the cells checked in pass 1. */
static void
obuf_int(obuf *b, int x)
{
	if (b->n == b->cap)
	{
		size_t		cap = Max(b->cap * 2, 16);

		b->data = b->data ? repalloc_huge(b->data, cap * sizeof(int))
			: MemoryContextAllocHuge(CurrentMemoryContext, cap * sizeof(int));
		b->cap = cap;
	}
	b->data[b->n++] = x;
}

/*
 * A one-dimensional array of n elements of type (int4 or float8), with
 * its data uninitialized, for the caller to fill: built in place, so the
 * output is never held twice.  The caller has checked n.
 */
static ArrayType *
new_array(size_t n, Oid type)
{
	size_t		elem = type == INT4OID ? sizeof(int32) : sizeof(float8);
	Size		bytes = ARR_OVERHEAD_NONULLS(1) + n * elem;
	ArrayType  *a;

	if (n == 0)
		return construct_empty_array(type);
	a = MemoryContextAllocHuge(CurrentMemoryContext, bytes);
	SET_VARSIZE(a, bytes);
	a->ndim = 1;
	a->dataoffset = 0;
	a->elemtype = type;
	*ARR_DIMS(a) = (int) n;
	*ARR_LBOUND(a) = 1;
	return a;
}

static Datum
obuf_array(obuf *b)
{
	ArrayType  *a = new_array(b->n, INT4OID);

	if (b->n)
		memcpy(ARR_DATA_PTR(a), b->data, sizeof(int) * b->n);
	return PointerGetDatum(a);
}

/* The output: gaps._infer's arrays for the instances' CPTs. */
typedef struct pout
{
	obuf		aux_card;
	obuf		child;
	obuf		parent_off;
	obuf		parents;
	obuf		cpt_off;
	obuf		cpt_lscale;
	double	   *cpts;			/* the data of cpts_array */
	size_t		ncpts;			/* elements written */
	ArrayType  *cpts_array;
} pout;

/*
 * Turn an error code of gx_param_ground or gx_param_size for the instance
 * of variable child into an ERROR naming the factor.  hold is
 * gx_param_size's estimate of what solving the instance holds.
 */
static void
report(int rc, const char *fname, int child, size_t hold)
{
	if (rc == GX_ETOOBIG && hold == SIZE_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("factor \"%s\": the instance of variable %d needs a table of more than "
						"gaps.max_factor_cells (%d) cells", fname, child,
						gaps_max_factor_cells),
				 errhint("Raise gaps.max_factor_cells, or give the instance fewer missing "
						 "parents.")));
	if (rc == GX_ETOOBIG)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("factor \"%s\": solving the instance of variable %d would hold "
						"about %zu cells, more than gaps.max_total_cells (%d)", fname, child,
						hold, gaps_max_total_cells),
				 errhint("Raise gaps.max_total_cells, or give the instance fewer missing "
						 "parents.")));
	if (rc == GX_ELOST)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("factor \"%s\": a positive probability in the table of variable %d "
						"is not representable", fname, child),
				 errdetail("Its parameters are so extreme that a value comes out as zero "
						   "or below the smallest normal double.")));
	if (rc == GX_ENOMEM)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory grounding factor \"%s\"", fname)));
	if (rc != GX_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("factor \"%s\": the instance of variable %d is malformed, or a "
						"logistic score is not finite", fname, child),
				 errdetail("An aggregating slot's member variables must be distinct.")));
}

/*
 * Ground one instance and append its CPTs to o: each table's values, then
 * its log offsets, in o->cpts, whose room pass 1 reserved.
 */
static void
emit_instance(const gx_env *env, const gx_pfactor *f, const gx_pinst *in,
			  const char *fname, pout *o)
{
	int			naux = gx_param_naux(f, in);
	gx_pcpt    *t = palloc(sizeof(gx_pcpt) * (size_t) (naux + 1));
	int			rc;

	rc = gx_param_ground(env, f, in, t);
	report(rc, fname, in->child, 0);
	for (int i = 0; i <= naux; i++)
	{
		if (i < naux)
			obuf_int(&o->aux_card, t[i].card);
		obuf_int(&o->child, t[i].child);
		obuf_int(&o->cpt_off, (int) o->ncpts + 1);
		memcpy(o->cpts + o->ncpts, t[i].vals, sizeof(double) * t[i].ncells);
		o->ncpts += t[i].ncells;
		obuf_int(&o->cpt_lscale, (int) o->ncpts + 1);
		memcpy(o->cpts + o->ncpts, t[i].lscale, sizeof(double) * (size_t) t[i].card);
		o->ncpts += (size_t) t[i].card;
		for (int p = 0; p < t[i].nparents; p++)
			obuf_int(&o->parents, t[i].parents[p]);
		obuf_int(&o->parent_off, (int) o->parents.n + 1);
	}
	gx_param_free(env, t, naux + 1);
	pfree(t);
}

/*
 * Read and check the factor's description, arguments 1 to 5 of
 * gaps._param_cpts, into f; the arrays are palloc'd.
 */
static void
read_factor(FunctionCallInfo fcinfo, gx_pfactor *f)
{
	char	   *kind = text_to_cstring(PG_GETARG_TEXT_PP(1));
	int		   *slot_card;
	unsigned char *slot_agg;
	double	   *params;
	int			ns,
				nagg;

	f->kind = strcmp(kind, "logistic") == 0 ? GX_LOGISTIC
		: strcmp(kind, "noisy_or") == 0 ? GX_NOISY_OR : 0;
	f->card = PG_GETARG_INT32(2);
	ns = get_ints(fcinfo, 3, "slot_card", &slot_card);
	nagg = get_bools(fcinfo, 4, "slot_agg", &slot_agg);
	f->nparams = get_floats(fcinfo, 5, "params", &params);
	if (nagg != ns)
		bad_arg("slot_card and slot_agg must have the same length");
	f->nslots = ns;
	f->slot_card = slot_card;
	f->slot_agg = slot_agg;
	f->params = params;
	if (gx_param_check(f) != GX_OK)
		bad_arg("malformed parameterised factor (kind, domain sizes or parameters)");
}

/*
 * The instances, arguments arg to arg + 3 (inst_child, inst_off,
 * member_var and member_ev), checked.  The caller sets first_aux.
 */
typedef struct pinsts
{
	int			first_aux;
	int			ninst;
	int		   *ichild;
	int		   *ioff;
	int		   *mvar;
	int		   *mval;
} pinsts;

static void
read_instances(FunctionCallInfo fcinfo, int arg, int ns, pinsts *pi)
{
	int		   *mev;
	int			noff,
				nm,
				nmev;

	pi->ninst = get_ints(fcinfo, arg, "inst_child", &pi->ichild);
	noff = get_ints(fcinfo, arg + 1, "inst_off", &pi->ioff);
	nm = get_ints(fcinfo, arg + 2, "member_var", &pi->mvar);
	nmev = get_ints(fcinfo, arg + 3, "member_ev", &mev);
	if (nmev != nm || (int64) noff != (int64) pi->ninst * ns + 1 || pi->ioff[0] != 1 ||
		pi->ioff[noff - 1] != nm + 1)
		bad_arg("inst_off must run from 1 to the number of members plus 1, one entry "
				"per slot of each instance and one more");
	for (int i = 1; i < noff; i++)
		if (pi->ioff[i] < pi->ioff[i - 1])
			bad_arg("inst_off must not decrease");
	pi->mval = palloc(sizeof(int) * (nm ? nm : 1));
	for (int i = 0; i < nm; i++)
		pi->mval[i] = mev[i] - 1;
}

/* Instance i of pi as the engine takes it, offsets into offp (ns + 1). */
static void
instance(const pinsts *pi, int ns, int i, int *offp, gx_pinst *in)
{
	int			base = pi->ioff[i * ns] - 1;

	for (int s = 0; s <= ns; s++)
		offp[s] = pi->ioff[i * ns + s] - 1 - base;
	in->child = pi->ichild[i];
	in->off = offp;
	in->var = pi->mvar + base;
	in->value = pi->mval + base;
	in->first_aux = 0;
}

/*
 * Pass 1: size every instance (gx_param_size, which applies the
 * per-instance bounds) and the factor's tables together, and refuse
 * before anything is built.  Returns the total number of float8s, and the
 * auxiliary variables and the tables' parent links into *naux and *nlinks.
 */
static size_t
size_factor(const gx_env *env, const gx_pfactor *f, const pinsts *pi, const char *fname,
			int64 *naux, int64 *nlinks)
{
	int		   *offp = palloc(sizeof(int) * (f->nslots + 1));
	size_t		total = 0;

	*naux = 0;
	*nlinks = 0;

	for (int i = 0; i < pi->ninst; i++)
	{
		gx_pinst	in;
		size_t		cells,
					hold;
		int			rc;

		CHECK_FOR_INTERRUPTS();
		instance(pi, f->nslots, i, offp, &in);
		rc = gx_param_size(env, f, &in, &cells, &hold);
		report(rc, fname, in.child, hold);
		total += cells;
		*naux += gx_param_naux(f, &in);
		*nlinks += gx_param_nlinks(f, &in);
		if (total > env->max_total_cells)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("factor \"%s\": its instances' tables need more than "
							"gaps.max_total_cells (%d) cells", fname, gaps_max_total_cells),
					 errdetail("The first %d of its %d instances need %zu.", i + 1, pi->ninst,
							   total),
					 errhint("Raise gaps.max_total_cells, or give its instances fewer "
							 "missing parents.")));
	}
	if (total > MaxArraySize || !AllocSizeIsValid(ARR_OVERHEAD_NONULLS(1) + total * 8))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("factor \"%s\": its instances' tables need %zu cells, more than one "
						"array can hold", fname, total)));
	if ((int64) pi->first_aux + *naux > PG_INT32_MAX)
		bad_arg("too many auxiliary variables");
	pfree(offp);
	return total;
}

/*
 * gaps._param_cpts(factor text, kind text, child_card int, slot_card int[],
 *					slot_agg boolean[], params float8[], first_aux int,
 *					inst_child int[], inst_off int[], member_var int[],
 *					member_ev int[])
 * RETURNS gaps._param_net (aux_card int[], child int[], parent_off int[],
 *			parents int[], cpt_off int[], cpts float8[], cpt_lscale int[])
 *
 * factor names the factor in errors.  It has one parent slot per element
 * of slot_card (the slot's domain size) and slot_agg (whether it
 * aggregates).  Instance i has child variable inst_child[i]; its slot s
 * holds the members inst_off[i * ns + s] .. inst_off[i * ns + s + 1] - 1
 * (1-based) of member_var and member_ev (the member's evidence as in
 * gaps._infer: 0 missing, else the 1-based value), with ns slots; an
 * aggregating slot's member variables must be strictly increasing.
 * Auxiliary variables are numbered from first_aux, instance by instance,
 * and aux_card gives their cardinalities in order.  Per instance, the
 * auxiliary variables' CPTs come first, then the child's.  Every CPT is a
 * log CPT (engine/gx_param.h): cpt_off is the offset of its log values in
 * cpts and cpt_lscale that of its child's card log offsets, which follow
 * them.  parent_off has one more element than child, from 1; all offsets
 * are 1-based into the returned cpts and parents.
 *
 * Every instance is sized before anything is built, and the factor's
 * tables together are held to gaps.max_total_cells, so a factor that
 * cannot be grounded fails before it allocates.
 */
Datum
gaps_param_cpts(PG_FUNCTION_ARGS)
{
	gx_pfactor	f;
	gx_env		env = {pg_alloc, pg_free, pg_interrupt, NULL, 0, 0, 0, 0, 0, 0};
	pinsts		pi;
	char	   *fname;
	int		   *offp;
	size_t		total;
	int64		naux,
				nlinks;
	pout		o;
	TupleDesc	desc;
	ExpandedRecordHeader *erh;
	Datum		values[7];

	for (int i = 0; i < 11; i++)
		if (PG_ARGISNULL(i))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("gaps._param_cpts arguments must not be NULL")));
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "gaps._param_cpts must return a record");

	fname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	read_factor(fcinfo, &f);
	pi.first_aux = PG_GETARG_INT32(6);
	if (pi.first_aux < 1)
		bad_arg("first_aux must be positive");
	read_instances(fcinfo, 7, f.nslots, &pi);
	env.max_cells = (size_t) gaps_max_factor_cells;
	env.max_total_cells = (size_t) gaps_max_total_cells;
	env.samples = gaps_samples;
	total = size_factor(&env, &f, &pi, fname, &naux, &nlinks);

	/* Pass 2: build each instance's tables into the output. */
	memset(&o, 0, sizeof(o));
	o.cpts_array = new_array(total, FLOAT8OID);
	o.cpts = total ? (double *) ARR_DATA_PTR(o.cpts_array) : NULL;
	obuf_int(&o.parent_off, 1);
	offp = palloc(sizeof(int) * (f.nslots + 1));
	for (int i = 0; i < pi.ninst; i++)
	{
		gx_pinst	in;

		CHECK_FOR_INTERRUPTS();
		instance(&pi, f.nslots, i, offp, &in);
		in.first_aux = pi.first_aux + (int) o.aux_card.n;
		emit_instance(&env, &f, &in, fname, &o);
	}
	Assert(o.ncpts == total);

	values[0] = obuf_array(&o.aux_card);
	values[1] = obuf_array(&o.child);
	values[2] = obuf_array(&o.parent_off);
	values[3] = obuf_array(&o.parents);
	values[4] = obuf_array(&o.cpt_off);
	values[5] = PointerGetDatum(o.cpts_array);
	values[6] = obuf_array(&o.cpt_lscale);

	/*
	 * An expanded record, not a tuple: a tuple of the seven arrays would be
	 * held to 1 GB in all, where each array alone may reach it.  A caller
	 * that assigns the result to a record variable takes the record over;
	 * one that selects it into a row flattens it, and is held to 1 GB
	 * again (doc/DESIGN.md, "Grounding capacity").  Each field is copied
	 * into the record, so the original is freed once it is.
	 */
	erh = make_expanded_record_from_tupdesc(desc, CurrentMemoryContext);
	for (int i = 0; i < 7; i++)
	{
		expanded_record_set_field(erh, i + 1, values[i], false, false);
		pfree(DatumGetPointer(values[i]));
	}
	PG_RETURN_DATUM(ExpandedRecordGetDatum(erh));
}

/*
 * gaps._param_size(factor text, kind text, child_card int, slot_card int[],
 *					slot_agg boolean[], params float8[], inst_child int[],
 *					inst_off int[], member_var int[], member_ev int[])
 * RETURNS (aux bigint, links bigint, cells bigint)
 *
 * What gaps._param_cpts would build for the same arguments (without
 * first_aux), from its first pass alone, so nothing is built: the number
 * of auxiliary variables, the tables' parent links, and their cells, vals
 * and lscale together, which are the lengths of the aux_card, parents and
 * cpts arrays it would return.  Its bounds apply and fail the same way.
 */
Datum
gaps_param_size(PG_FUNCTION_ARGS)
{
	gx_pfactor	f;
	gx_env		env = {pg_alloc, pg_free, pg_interrupt, NULL, 0, 0, 0, 0, 0, 0};
	pinsts		pi;
	char	   *fname;
	int64		naux,
				nlinks;
	size_t		total;
	TupleDesc	desc;
	Datum		values[3];
	bool		nulls[3] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "gaps._param_size must return a record");
	fname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	read_factor(fcinfo, &f);
	read_instances(fcinfo, 6, f.nslots, &pi);
	pi.first_aux = 1;
	env.max_cells = (size_t) gaps_max_factor_cells;
	env.max_total_cells = (size_t) gaps_max_total_cells;
	env.samples = gaps_samples;
	total = size_factor(&env, &f, &pi, fname, &naux, &nlinks);
	values[0] = Int64GetDatum(naux);
	values[1] = Int64GetDatum(nlinks);
	values[2] = Int64GetDatum((int64) total);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}
