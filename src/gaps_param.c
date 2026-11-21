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
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "engine/gx_param.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "pg_gaps.h"

PG_FUNCTION_INFO_V1(gaps_param_cpts);

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

/* A growable int or float8 buffer for the output arrays. */
typedef struct obuf
{
	size_t		n;
	size_t		cap;
	size_t		elem;
	void	   *data;
} obuf;

/* Room for k more elements; the output must fit in one SQL array. */
static void *
obuf_room(obuf *b, size_t k)
{
	if (b->n + k > MaxArraySize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a parameterised factor's ground CPTs need more than %d array elements",
						(int) MaxArraySize)));
	if (b->n + k > b->cap)
	{
		size_t		cap = Max(b->cap * 2, b->n + k);

		cap = Max(cap, 16);
		b->data = b->data ? repalloc_huge(b->data, cap * b->elem)
			: MemoryContextAllocHuge(CurrentMemoryContext, cap * b->elem);
		b->cap = cap;
	}
	return (char *) b->data + b->n * b->elem;
}

static void
obuf_int(obuf *b, int x)
{
	*(int *) obuf_room(b, 1) = x;
	b->n++;
}

static Datum
obuf_array(obuf *b, Oid type)
{
	Datum	   *d = palloc(sizeof(Datum) * (b->n ? b->n : 1));

	for (size_t i = 0; i < b->n; i++)
		d[i] = type == INT4OID ? Int32GetDatum(((int *) b->data)[i])
			: Float8GetDatum(((double *) b->data)[i]);
	return PointerGetDatum(construct_array_builtin(d, (int) b->n, type));
}

/* The output: gaps._infer's arrays for the instances' CPTs. */
typedef struct pout
{
	obuf		aux_card;
	obuf		child;
	obuf		parent_off;
	obuf		parents;
	obuf		cpt_off;
	obuf		cpts;
} pout;

/*
 * Turn an error code of gx_param_ground into an ERROR, naming the
 * instance's child variable.
 */
static void
report(int rc, int child)
{
	if (rc == GX_ETOOBIG)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a parameterised factor's tables for variable %d need a table of more "
						"than gaps.max_factor_cells (%d) cells, or more than "
						"gaps.max_total_cells (%d) together", child, gaps_max_factor_cells,
						gaps_max_total_cells),
				 errhint("Raise the limits, or give the instance fewer missing parents.")));
	if (rc == GX_ENOMEM)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory grounding a parameterised factor")));
	if (rc != GX_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameterised factor instance of variable %d is malformed, or a "
						"logistic score is not finite", child)));
}

/*
 * Ground one instance and append its CPTs to o.  The instance's CPTs land
 * in one component, so gx_param_ground bounds their total by
 * gaps.max_total_cells as well as each by gaps.max_factor_cells.
 */
static void
emit_instance(const gx_env *env, const gx_pfactor *f, const gx_pinst *in, pout *o)
{
	int			naux = gx_param_naux(f, in);
	gx_pcpt    *t = palloc(sizeof(gx_pcpt) * (size_t) (naux + 1));
	int			rc;

	rc = gx_param_ground(env, f, in, t);
	report(rc, in->child);
	for (int i = 0; i <= naux; i++)
	{
		if (i < naux)
			obuf_int(&o->aux_card, t[i].card);
		obuf_int(&o->child, t[i].child);
		obuf_int(&o->cpt_off, (int) o->cpts.n + 1);
		memcpy(obuf_room(&o->cpts, t[i].ncells), t[i].vals, sizeof(double) * t[i].ncells);
		o->cpts.n += t[i].ncells;
		memcpy(obuf_room(&o->parents, (size_t) t[i].nparents), t[i].parents,
			   sizeof(int) * (size_t) t[i].nparents);
		o->parents.n += (size_t) t[i].nparents;
		obuf_int(&o->parent_off, (int) o->parents.n + 1);
	}
	gx_param_free(env, t, naux + 1);
	pfree(t);
}

/*
 * gaps._param_cpts(kind text, child_card int, slot_card int[],
 *					slot_agg boolean[], params float8[], first_aux int,
 *					inst_child int[], inst_off int[], member_var int[],
 *					member_ev int[])
 * RETURNS (aux_card int[], child int[], parent_off int[], parents int[],
 *			cpt_off int[], cpts float8[])
 *
 * The factor has one parent slot per element of slot_card (the slot's
 * domain size) and slot_agg (whether it aggregates).  Instance i has child
 * variable inst_child[i]; its slot s holds the members inst_off[i * ns +
 * s] .. inst_off[i * ns + s + 1] - 1 (1-based) of member_var and member_ev
 * (the member's evidence as in gaps._infer: 0 missing, else the 1-based
 * value), with ns slots.  Auxiliary variables are numbered from first_aux,
 * instance by instance, and aux_card gives their cardinalities in order.
 * Per instance, the auxiliary variables' CPTs come first, then the
 * child's.  parent_off has one more element than child, from 1; cpt_off
 * and parent_off are 1-based into the returned cpts and parents.
 */
Datum
gaps_param_cpts(PG_FUNCTION_ARGS)
{
	gx_pfactor	f;
	gx_env		env = {pg_alloc, pg_free, pg_interrupt, NULL, 0, 0, 0, 0, 0, 0};
	char	   *kind;
	int		   *slot_card,
			   *ichild,
			   *ioff,
			   *mvar,
			   *mev,
			   *mval;
	unsigned char *slot_agg;
	double	   *params;
	int			ns,
				nagg,
				ninst,
				noff,
				nm,
				nmev,
				first_aux;
	pout		o;
	TupleDesc	desc;
	Datum		values[6];
	bool		nulls[6] = {0};

	for (int i = 0; i < 10; i++)
		if (PG_ARGISNULL(i))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("gaps._param_cpts arguments must not be NULL")));
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "gaps._param_cpts must return a record");

	/* The factor. */
	kind = text_to_cstring(PG_GETARG_TEXT_PP(0));
	f.kind = strcmp(kind, "logistic") == 0 ? GX_LOGISTIC
		: strcmp(kind, "noisy_or") == 0 ? GX_NOISY_OR : 0;
	f.card = PG_GETARG_INT32(1);
	ns = get_ints(fcinfo, 2, "slot_card", &slot_card);
	nagg = get_bools(fcinfo, 3, "slot_agg", &slot_agg);
	f.nparams = get_floats(fcinfo, 4, "params", &params);
	if (nagg != ns)
		bad_arg("slot_card and slot_agg must have the same length");
	f.nslots = ns;
	f.slot_card = slot_card;
	f.slot_agg = slot_agg;
	f.params = params;
	if (gx_param_check(&f) != GX_OK)
		bad_arg("malformed parameterised factor (kind, domain sizes or parameters)");

	/* The instances: offsets in order and in range, members' evidence. */
	first_aux = PG_GETARG_INT32(5);
	ninst = get_ints(fcinfo, 6, "inst_child", &ichild);
	noff = get_ints(fcinfo, 7, "inst_off", &ioff);
	nm = get_ints(fcinfo, 8, "member_var", &mvar);
	nmev = get_ints(fcinfo, 9, "member_ev", &mev);
	if (nmev != nm || (int64) noff != (int64) ninst * ns + 1 || ioff[0] != 1 ||
		ioff[noff - 1] != nm + 1)
		bad_arg("inst_off must run from 1 to the number of members plus 1, one entry "
				"per slot of each instance and one more");
	for (int i = 1; i < noff; i++)
		if (ioff[i] < ioff[i - 1])
			bad_arg("inst_off must not decrease");
	mval = palloc(sizeof(int) * (nm ? nm : 1));
	for (int i = 0; i < nm; i++)
		mval[i] = mev[i] - 1;
	if (first_aux < 1)
		bad_arg("first_aux must be positive");

	env.max_cells = (size_t) gaps_max_factor_cells;
	env.max_total_cells = (size_t) gaps_max_total_cells;
	memset(&o, 0, sizeof(o));
	o.aux_card.elem = o.child.elem = o.parent_off.elem = o.parents.elem =
		o.cpt_off.elem = sizeof(int);
	o.cpts.elem = sizeof(double);
	obuf_int(&o.parent_off, 1);

	for (int i = 0; i < ninst; i++)
	{
		int		   *offp = palloc(sizeof(int) * (ns + 1));
		gx_pinst	in;
		int			base = ioff[i * ns] - 1;

		CHECK_FOR_INTERRUPTS();
		for (int s = 0; s <= ns; s++)
			offp[s] = ioff[i * ns + s] - 1 - base;
		in.child = ichild[i];
		in.off = offp;
		in.var = mvar + base;
		in.value = mval + base;
		if ((int64) first_aux + (int64) o.aux_card.n + gx_param_naux(&f, &in) > PG_INT32_MAX)
			bad_arg("too many auxiliary variables");
		in.first_aux = first_aux + (int) o.aux_card.n;
		emit_instance(&env, &f, &in, &o);
		pfree(offp);
	}

	values[0] = obuf_array(&o.aux_card, INT4OID);
	values[1] = obuf_array(&o.child, INT4OID);
	values[2] = obuf_array(&o.parent_off, INT4OID);
	values[3] = obuf_array(&o.parents, INT4OID);
	values[4] = obuf_array(&o.cpt_off, INT4OID);
	values[5] = obuf_array(&o.cpts, FLOAT8OID);
	desc = BlessTupleDesc(desc);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}
