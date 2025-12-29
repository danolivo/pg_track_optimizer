/*
 * rstats.c - base type for incremental statistics type
 *
 * This implements a 'statistics' base type that maintains
 * running statistics using Welford's algorithm for numerical stability.
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"

#include <math.h>

#include "rstats.h"

/* Function declarations */
PG_FUNCTION_INFO_V1(rstats_in);
PG_FUNCTION_INFO_V1(rstats_out);
PG_FUNCTION_INFO_V1(rstats_recv);
PG_FUNCTION_INFO_V1(rstats_send);

PG_FUNCTION_INFO_V1(rstats_empty_constructor);
PG_FUNCTION_INFO_V1(rstats_constructor);
PG_FUNCTION_INFO_V1(rstats_init_double);
PG_FUNCTION_INFO_V1(rstats_init_int4);
PG_FUNCTION_INFO_V1(rstats_init_numeric);

PG_FUNCTION_INFO_V1(rstats_add);
PG_FUNCTION_INFO_V1(rstats_get_count);
PG_FUNCTION_INFO_V1(rstats_get_mean);
PG_FUNCTION_INFO_V1(rstats_get_variance);
PG_FUNCTION_INFO_V1(rstats_get_stddev);
PG_FUNCTION_INFO_V1(rstats_get_min);
PG_FUNCTION_INFO_V1(rstats_get_max);
PG_FUNCTION_INFO_V1(rstats_eq);
PG_FUNCTION_INFO_V1(rstats_get_field);


/*
 * Input function: converts text representation to internal format
 * Format: (count:N,mean:M,min:MIN,max:MAX,variance:V)
 */
Datum
rstats_in(PG_FUNCTION_ARGS)
{
	char   *str = PG_GETARG_CSTRING(0);
	RStats *result;
	int64	count;
	double	mean, min_val, max_val, variance;
	int		nfields;


	/* Parse the input string */
	nfields = sscanf(str, "(count:"INT64_FORMAT",mean:%lf,min:%lf,max:%lf,variance:%lf)",
					 &count, &mean, &min_val, &max_val, &variance);

	if (nfields != 5)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type rstats: \"%s\"", str),
				 errhint("Expected format: (count:N,mean:M,min:MIN,max:MAX,variance:V)")));

	if (count < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("count must be non-negative")));

	/* Allocate and initialize the result */
	result = (RStats *) palloc(sizeof(RStats));

	result->count = count;
	result->mean = mean;
	result->min = min_val;
	result->max = max_val;

	/* Convert variance to m2 (sum of squared differences) */
	if (count > 1)
		result->m2 = variance * (count - 1);
	else
		result->m2 = 0.0;

	PG_RETURN_RSTATS_P(result);
}

/*
 * Output function: converts internal format to text representation
 */
Datum
rstats_out(PG_FUNCTION_ARGS)
{
	RStats *stats = PG_GETARG_RSTATS_P(0);
	char   *result;
	double	variance;

	/* Calculate variance from m2 */
	if (stats->count > 1)
		variance = stats->m2 / (stats->count - 1);
	else
		variance = 0.0;

	result = psprintf("(count:"INT64_FORMAT",mean:%.15g,min:%.15g,max:%.15g,variance:%.15g)",
					  stats->count, stats->mean, stats->min, stats->max, variance);

	PG_RETURN_CSTRING(result);
}

/*
 * Binary input function
 */
Datum
rstats_recv(PG_FUNCTION_ARGS)
{
	StringInfo  buf = (StringInfo) PG_GETARG_POINTER(0);
	RStats *result;

	result = (RStats *) palloc(sizeof(RStats));

	result->count = pq_getmsgint64(buf);
	result->mean = pq_getmsgfloat8(buf);
	result->m2 = pq_getmsgfloat8(buf);
	result->min = pq_getmsgfloat8(buf);
	result->max = pq_getmsgfloat8(buf);

	PG_RETURN_RSTATS_P(result);
}

/*
 * Binary output function
 */
Datum
rstats_send(PG_FUNCTION_ARGS)
{
	RStats *stats = PG_GETARG_RSTATS_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, stats->count);
	pq_sendfloat8(&buf, stats->mean);
	pq_sendfloat8(&buf, stats->m2);
	pq_sendfloat8(&buf, stats->min);
	pq_sendfloat8(&buf, stats->max);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Set RStats to an empty (uninitialized) state.
 * We use -1 as a sentinel value for all fields except count because:
 * 1. It's an invalid value for statistics (negative values shouldn't occur)
 * 2. It allows rstats_is_empty() to verify structural integrity
 * 3. It helps detect use of uninitialized/corrupted memory
 */
void
rstats_set_empty(RStats *result)
{
	result->count = 0;

	/* Use -1 as sentinel to detect uninitialized state and memory corruption */
	result->mean = -1.;
	result->m2 = -1.;
	result->min = -1.;
	result->max = -1.;
}

static double
get_double_value(Datum inputval, Oid argtype)
{
	double result;

	switch (argtype)
	{
	case FLOAT8OID:
		result = DatumGetFloat8(inputval);
		break;
	case INT4OID:
		result = DatumGetInt32(inputval);
		break;
	case UNKNOWNOID:
	{
		text   *txt = DatumGetTextPP(inputval);
		char   *str = text_to_cstring(txt);
		Datum	value;

		/* Convert to your target type, e.g., float8 */
		value = DirectFunctionCall1(float8in, CStringGetDatum(str));
		result = DatumGetFloat8(value);
		argtype = FLOAT8OID;
	}
		break;
	default:
			elog(ERROR, "unsupported input type");
			break;
	}

	return result;
}

Datum
rstats_empty_constructor(PG_FUNCTION_ARGS)
{
	RStats *result;

	result = (RStats *) palloc(sizeof(RStats));
	rstats_set_empty(result);

	PG_RETURN_POINTER(result);
}

Datum
rstats_constructor(PG_FUNCTION_ARGS)
{
	RStats			   *result;
	Datum				inputval;
	CoercionPathType	pathtype;
	Oid					elemtype;
	Oid					funcid;
	Datum				element;

	Assert(PG_NARGS() == 1);

	if (PG_ARGISNULL(0))
		elog(ERROR, "no NULL values allowed");

	elemtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (elemtype == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine data type of input")));

	inputval = PG_GETARG_DATUM(0);

	pathtype = find_coercion_pathway(FLOAT8OID, elemtype,
									 COERCION_EXPLICIT, &funcid);

	if (pathtype == COERCION_PATH_FUNC)
	{
		/* Use the cast function */
		element = OidFunctionCall1(funcid, inputval);
	}
	else if (pathtype == COERCION_PATH_RELABELTYPE)
	{
		/* Binary compatible, just relabel */
		element = inputval;
	}
	else if (pathtype == COERCION_PATH_COERCEVIAIO)
	{
		/* Convert via text representation */
		Oid		outfuncid;
		Oid		infuncid;
		bool	typisvarlena;
		char   *str;

		getTypeOutputInfo(elemtype, &outfuncid, &typisvarlena);
		str = OidOutputFunctionCall(outfuncid, inputval);

		getTypeInputInfo(elemtype, &infuncid, &outfuncid);
		element = OidInputFunctionCall(infuncid, str, outfuncid, -1);
	}
	else
		ereport(ERROR,
			(errcode(ERRCODE_CANNOT_COERCE),
			 errmsg("cannot cast type %s to double precision",
			 format_type_be(elemtype))));

	result = (RStats *) palloc(sizeof(RStats));
	rstats_init_internal(result, DatumGetFloat8(element));

	PG_RETURN_RSTATS_P(result);
}

/*
 * Internal function: Initialize a RStats object from a single value
 * This can be called directly from C code without going through the Datum interface.
 *
 * Note: We call rstats_set_empty() first to ensure clean state before initialization.
 * This is defensive programming - if the structure contained garbage, we clear it first.
 */
void
rstats_init_internal(RStats *result, double value)
{
	rstats_set_empty(result);

	result->count = 1;
	result->mean = value;
	result->m2 = 0.0;	  /* No variance with single value */
	result->min = value;
	result->max = value;
}

/*
 * Initialize statistics from a single value
 * Usage: rstats_init_double(42.5) returns a statistics object with count=1
 */
Datum
rstats_init_double(PG_FUNCTION_ARGS)
{
	double		value = PG_GETARG_FLOAT8(0);
	RStats	   *result;

	result = (RStats *) palloc(sizeof(RStats));
	rstats_init_internal(result, value);

	PG_RETURN_RSTATS_P(result);
}
Datum
rstats_init_int4(PG_FUNCTION_ARGS)
{
	double		value = PG_GETARG_INT32(0);
	RStats	   *result;

	result = (RStats *) palloc(sizeof(RStats));
	rstats_init_internal(result, value);

	PG_RETURN_RSTATS_P(result);
}
Datum
rstats_init_numeric(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	char	   *tmp;
	Datum		float_value;
	double		value;
	RStats *result;

	/* Convert numeric to double */
	tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
											  NumericGetDatum(num)));
	float_value = DirectFunctionCall1(float8in, CStringGetDatum(tmp));
	pfree(tmp);
	value = DatumGetFloat8(float_value);

	result = (RStats *) palloc(sizeof(RStats));
	rstats_init_internal(result, value);

	PG_RETURN_RSTATS_P(result);
}

/*
 * Internal function: Add a value to existing statistics using Welford's algorithm
 * This can be called directly from C code without going through the Datum interface.
 *
 * If the statistics object is in empty state (count == 0), it's automatically
 * initialized with the first value. This allows for convenient lazy initialization
 * of cumulative statistics.
 */
void
rstats_add_value(RStats *rstats, double value)
{
	double	delta;
	double	delta2;
	int64	new_count;

	/* Handle empty state: initialize with first value */
	if (rstats_is_empty(rstats))
	{
		rstats_init_internal(rstats, value);
		return;
	}

	/* Welford's algorithm for incremental mean and variance */
	new_count = rstats->count + 1;
	delta = value - rstats->mean;

	rstats->count = new_count;
	rstats->mean = rstats->mean + delta / new_count;

	delta2 = value - rstats->mean;
	rstats->m2 = rstats->m2 + delta * delta2;

	if (value < rstats->min)
		rstats->min = value;
	if (value > rstats->max)
		rstats->max = value;
}

/*
 * Add a new value to existing statistics using Welford's algorithm
 * Usage: stats + 42.5 returns updated statistics
 */
Datum
rstats_add(PG_FUNCTION_ARGS)
{
	RStats *stats = PG_GETARG_RSTATS_P(0);
	Oid		argtype;
	double	value;

	if (PG_ARGISNULL(1))
		elog(ERROR, "input value cannot be null");

	argtype = get_fn_expr_argtype(fcinfo->flinfo, 1);
	if (argtype == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine data type of input")));

	value = get_double_value(PG_GETARG_DATUM(1), argtype);

	rstats_add_value(stats, value);

	PG_RETURN_RSTATS_P(stats);
}

/*
 * Utility function to check if running statistics value is empty.
 * Also verifies the sentinel values to ensure the structure hasn't been
 * corrupted or used after being freed.
 */
bool
rstats_is_empty(RStats *value)
{
	if (value->count > 0)
		return false;

	/*
	 * Empty state detected. Verify sentinel values to ensure memory integrity.
	 * If these assertions fail, it indicates memory corruption or use-after-free.
	 */
	Assert(value->count == 0);
	Assert(value->mean == -1.);
	Assert(value->m2 == -1.);
	Assert(value->min == -1.);
	Assert(value->max == -1.);

	return true;
}

/*
 * Accessor functions to get individual statistics
 */

Datum
rstats_get_count(PG_FUNCTION_ARGS)
{
	RStats *stats = PG_GETARG_RSTATS_P(0);
	PG_RETURN_INT64(stats->count);
}

Datum
rstats_get_mean(PG_FUNCTION_ARGS)
{
	RStats *stats = PG_GETARG_RSTATS_P(0);
	PG_RETURN_FLOAT8(stats->mean);
}

Datum
rstats_get_variance(PG_FUNCTION_ARGS)
{
	RStats *stats = PG_GETARG_RSTATS_P(0);
	double	variance;

	if (stats->count > 1)
		variance = stats->m2 / (stats->count - 1);
	else
		variance = 0.0;

	PG_RETURN_FLOAT8(variance);
}

Datum
rstats_get_stddev(PG_FUNCTION_ARGS)
{
	RStats *stats = PG_GETARG_RSTATS_P(0);
	double	variance, stddev;

	if (stats->count > 1)
	{
		variance = stats->m2 / (stats->count - 1);
		stddev = sqrt(variance);
	}
	else
		stddev = 0.0;

	PG_RETURN_FLOAT8(stddev);
}

Datum
rstats_get_min(PG_FUNCTION_ARGS)
{
	RStats *stats = PG_GETARG_RSTATS_P(0);
	PG_RETURN_FLOAT8(stats->min);
}

Datum
rstats_get_max(PG_FUNCTION_ARGS)
{
	RStats *stats = PG_GETARG_RSTATS_P(0);
	PG_RETURN_FLOAT8(stats->max);
}

/*

 * Equality comparison for statistics type
 * Two statistics objects are equal if all their fields match
 */
Datum
rstats_eq(PG_FUNCTION_ARGS)
{
	RStats *stats1 = PG_GETARG_RSTATS_P(0);
	RStats *stats2 = PG_GETARG_RSTATS_P(1);

	/* Compare all fields for equality */
	if (stats1->count != stats2->count)
		PG_RETURN_BOOL(false);

	if (stats1->mean != stats2->mean)
		PG_RETURN_BOOL(false);

	if (stats1->m2 != stats2->m2)
		PG_RETURN_BOOL(false);

	if (stats1->min != stats2->min)
		PG_RETURN_BOOL(false);

	if (stats1->max != stats2->max)
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(true);
}

/*
 * Field accessor using -> operator
 * Allows accessing statistics fields like: stats -> 'mean'
 * Supported fields: count, mean, variance, stddev, min, max
 */
Datum
rstats_get_field(PG_FUNCTION_ARGS)
{
	RStats *stats = PG_GETARG_RSTATS_P(0);
	text   *field_text = PG_GETARG_TEXT_PP(1);
	char   *field_name;
	double	result;

	/* Convert text to C string */
	field_name = text_to_cstring(field_text);

	/* Determine which field was requested */
	if (strcmp(field_name, "count") == 0)
	{
		result = (double) stats->count;
	}
	else if (strcmp(field_name, "mean") == 0)
	{
		result = stats->mean;
	}
	else if (strcmp(field_name, "variance") == 0)
	{
		if (stats->count > 1)
			result = stats->m2 / (stats->count - 1);
		else
			result = 0.0;
	}
	else if (strcmp(field_name, "stddev") == 0)
	{
		if (stats->count > 1)
		{
			double variance = stats->m2 / (stats->count - 1);
			result = sqrt(variance);
		}
		else
			result = 0.0;
	}
	else if (strcmp(field_name, "min") == 0)
	{
		result = stats->min;
	}
	else if (strcmp(field_name, "max") == 0)
	{
		result = stats->max;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid field name for statistics type: \"%s\"", field_name),
				 errhint("Valid field names are: count, mean, variance, stddev, min, max")));
	}

	pfree(field_name);
	PG_RETURN_FLOAT8(result);
}
