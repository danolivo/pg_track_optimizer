/*-------------------------------------------------------------------------
 *
 * rstats.c
 *	  PostgreSQL base type for numerically stable running statistics
 *
 * This module implements the RStats type, which maintains incremental
 * statistics (count, mean, variance, min, max) using Welford's algorithm
 * for numerical stability. The type is fixed-size (40 bytes) with no
 * varlena header, enabling efficient storage and indexing.
 *
 * Key Features:
 *   - Single-pass computation with excellent numerical stability
 *   - Canonical empty state (count=0, all fields=0.0) with validation
 *   - Expression index support via -> field accessor operator
 *   - Four-layer validation (text/binary I/O, runtime checks)
 *
 * Production Considerations:
 *   - Binary format lacks version field - future changes require dump/restore
 *   - No built-in casts to/from bytea (prevents simple binary round-trips)
 *   - Equality operator uses exact float comparison (no epsilon tolerance)
 *   - Type is specifically designed for pg_track_optimizer's use case
 *
 * Copyright (c) 2024-2026, Andrei Lepikhov
 *
 * IDENTIFICATION
 *	  contrib/pg_track_optimizer/rstats.c
 *
 * LICENSE
 *	  This software may be modified and distributed under the terms
 *	  of the MIT licence. See the LICENSE file for details.
 *
 *-------------------------------------------------------------------------
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

	if (variance < 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("variance must be non-negative")));

	if (count > 0 && min_val > max_val)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("min value (%g) cannot be greater than max value (%g)",
						min_val, max_val)));

	/*
	 * Validate canonical empty state: count=0 should have all zero fields.
	 * This ensures consistency with rstats_set_empty() and binary format.
	 */
	if (count == 0)
	{
		if (mean != 0.0 || min_val != 0.0 || max_val != 0.0 || variance != 0.0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("RStats with count=0 must have all zero fields"),
					 errdetail("Got: mean=%g, min=%g, max=%g, variance=%g",
							   mean, min_val, max_val, variance)));
	}

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

	/*
	 * Validate canonical empty state: count=0 requires all fields to be 0.0.
	 * This catches corruption in binary protocol transmission.
	 */
	if (result->count == 0)
	{
		if (result->mean != 0.0 || result->m2 != 0.0 ||
			result->min != 0.0 || result->max != 0.0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("binary input for RStats has corrupted empty state"),
					 errdetail("count=0 but other fields non-zero: mean=%g, m2=%g, min=%g, max=%g",
							   result->mean, result->m2, result->min, result->max)));
	}

	PG_RETURN_RSTATS_P(result);
}

/*
 * Binary output function
 */
Datum
rstats_send(PG_FUNCTION_ARGS)
{
	RStats		   *stats = PG_GETARG_RSTATS_P(0);
	StringInfoData	buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, stats->count);

	/*
	 * Validate canonical empty state before serialization.
	 * This catches internal bugs where RStats was corrupted in memory
	 * before reaching the output function.
	 */
	if (stats->count == 0)
	{
		if (stats->mean != 0.0 || stats->m2 != 0.0 ||
			stats->min != 0.0 || stats->max != 0.0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("RStats internal corruption before serialization"),
					 errdetail("count=0 but other fields non-zero: mean=%g, m2=%g, min=%g, max=%g",
							   stats->mean, stats->m2, stats->min, stats->max),
					 errhint("This indicates a bug in RStats manipulation code.")));
	}

	pq_sendfloat8(&buf, stats->mean);
	pq_sendfloat8(&buf, stats->m2);
	pq_sendfloat8(&buf, stats->min);
	pq_sendfloat8(&buf, stats->max);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Set RStats to an empty (uninitialized) state.
 *
 * We use count=0 as the primary indicator of empty state, and set all other
 * fields to 0.0 for consistency and to ensure clean serialization:
 * 1. Provides a canonical empty representation across text/binary formats
 * 2. Allows rstats_is_empty() to verify structural integrity
 * 3. Ensures empty state displays cleanly: (count:0,mean:0,min:0,max:0,variance:0)
 * 4. Prevents confusion with legitimate statistics that happen to have zero values
 *
 * Note: This means we cannot distinguish between truly empty stats and stats
 * initialized with a single value of 0. This is acceptable as both represent
 * valid states and the distinction is not operationally important.
 */
void
rstats_set_empty(RStats *result)
{
	result->count = 0;

	/* Set all fields to 0.0 for canonical empty representation */
	result->mean = 0.;
	result->m2 = 0.;
	result->min = 0.;
	result->max = 0.;
}

/*
 * Check if RStats is in empty (uninitialized) state.
 *
 * Primary criterion: count == 0
 *
 * Additionally validates that empty stats have canonical zero values for all
 * fields. This catches deserialization errors or memory corruption where count
 * was zeroed but other fields contain garbage.
 *
 * Note: Since we require strict zeros, this validation provides integrity
 * checking across serialization boundaries (text/binary I/O).
 */
bool
rstats_is_empty(RStats *value)
{
	if (value->count > 0)
		return false;

	/*
	 * count == 0 detected. Verify all fields are zero for canonical empty state.
	 * Non-zero values indicate corrupted data from deserialization or memory issues.
	 */
	if (value->mean != 0.0 || value->m2 != 0.0 || value->min != 0.0 ||
		value->max != 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("RStats data corruption detected"),
				 errdetail("count=0 but other fields non-zero: mean=%g, m2=%g, min=%g, max=%g",
						   value->mean, value->m2, value->min, value->max)));

	return true;
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
	Datum		num = PG_GETARG_DATUM(0);
	Datum		float_value;
	RStats *result;

	float_value = DirectFunctionCall1(numeric_float8, num);

	result = (RStats *) palloc(sizeof(RStats));
	rstats_init_internal(result, DatumGetFloat8(float_value));

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

 * Equality comparison for statistics type
 * Two statistics objects are equal if all their fields match.
 *
 * NOTE:
 * Use direct (not an epsilon match) because the sematics of this operator is
 * that is exactly the same data - same values were coming in the same order.
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
