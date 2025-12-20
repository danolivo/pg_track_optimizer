/*
 * stat_type.c - base type for incremental statistics type
 *
 * This implements a 'statistics' base type that maintains
 * running statistics using Welford's algorithm for numerical stability.
 */

#include "postgres.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include <math.h>

/*
 * Internal representation of statistics
 * Uses Welford's algorithm to maintain numerically stable running statistics
 * Fixed-size type (40 bytes) - no varlena header needed
 */
typedef struct Statistics
{
    int64       count;          /* number of values */
    double      mean;           /* running mean */
    double      m2;             /* sum of squared differences from mean (for variance) */
    double      min;            /* minimum value */
    double      max;            /* maximum value */
} Statistics;

/* Macros for easier access */
#define DatumGetStatisticsP(X)      ((Statistics *) DatumGetPointer(X))
#define StatisticsPGetDatum(X)      PointerGetDatum(X)
#define PG_GETARG_STATISTICS_P(n)   DatumGetStatisticsP(PG_GETARG_DATUM(n))
#define PG_RETURN_STATISTICS_P(x)   return StatisticsPGetDatum(x)

/* Function declarations */
PG_FUNCTION_INFO_V1(statistics_in);
PG_FUNCTION_INFO_V1(statistics_out);
PG_FUNCTION_INFO_V1(statistics_recv);
PG_FUNCTION_INFO_V1(statistics_send);

PG_FUNCTION_INFO_V1(statistics_init_double);
PG_FUNCTION_INFO_V1(statistics_init_numeric);

PG_FUNCTION_INFO_V1(statistics_add);
PG_FUNCTION_INFO_V1(statistics_get_count);
PG_FUNCTION_INFO_V1(statistics_get_mean);
PG_FUNCTION_INFO_V1(statistics_get_variance);
PG_FUNCTION_INFO_V1(statistics_get_stddev);
PG_FUNCTION_INFO_V1(statistics_get_min);
PG_FUNCTION_INFO_V1(statistics_get_max);
PG_FUNCTION_INFO_V1(statistics_eq);
PG_FUNCTION_INFO_V1(statistics_get_field);

/*
 * Input function: converts text representation to internal format
 * Format: (count:N,mean:M,min:MIN,max:MAX,variance:V)
 */
Datum
statistics_in(PG_FUNCTION_ARGS)
{
    char       *str = PG_GETARG_CSTRING(0);
    Statistics *result;
    int64       count;
    double      mean, min_val, max_val, variance;
    int         nfields;

    /* Parse the input string */
    nfields = sscanf(str, "(count:"INT64_FORMAT",mean:%lf,min:%lf,max:%lf,variance:%lf)",
                     &count, &mean, &min_val, &max_val, &variance);

    if (nfields != 5)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input syntax for type statistics: \"%s\"", str),
                 errhint("Expected format: (count:N,mean:M,min:MIN,max:MAX,variance:V)")));

    if (count < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("count must be non-negative")));

    /* Allocate and initialize the result */
    result = (Statistics *) palloc(sizeof(Statistics));

    result->count = count;
    result->mean = mean;
    result->min = min_val;
    result->max = max_val;

    /* Convert variance to m2 (sum of squared differences) */
    if (count > 1)
        result->m2 = variance * (count - 1);
    else
        result->m2 = 0.0;

    PG_RETURN_STATISTICS_P(result);
}

/*
 * Output function: converts internal format to text representation
 */
Datum
statistics_out(PG_FUNCTION_ARGS)
{
    Statistics *stats = PG_GETARG_STATISTICS_P(0);
    char       *result;
    double      variance;

    /* Calculate variance from m2 */
    if (stats->count > 1)
        variance = stats->m2 / (stats->count - 1);
    else
        variance = 0.0;

    result = psprintf("(count:%lld,mean:%.15g,min:%.15g,max:%.15g,variance:%.15g)",
                      stats->count, stats->mean, stats->min, stats->max, variance);

    PG_RETURN_CSTRING(result);
}

/*
 * Binary input function
 */
Datum
statistics_recv(PG_FUNCTION_ARGS)
{
    StringInfo  buf = (StringInfo) PG_GETARG_POINTER(0);
    Statistics *result;

    result = (Statistics *) palloc(sizeof(Statistics));

    result->count = pq_getmsgint64(buf);
    result->mean = pq_getmsgfloat8(buf);
    result->m2 = pq_getmsgfloat8(buf);
    result->min = pq_getmsgfloat8(buf);
    result->max = pq_getmsgfloat8(buf);

    PG_RETURN_STATISTICS_P(result);
}

/*
 * Binary output function
 */
Datum
statistics_send(PG_FUNCTION_ARGS)
{
    Statistics *stats = PG_GETARG_STATISTICS_P(0);
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
 * Initialize statistics from a single value
 * Usage: statistics_init_double(42.5) returns a statistics object with count=1
 */
Datum
statistics_init_double(PG_FUNCTION_ARGS)
{
    double      value = PG_GETARG_FLOAT8(0);
    Statistics *result;

    result = (Statistics *) palloc(sizeof(Statistics));

    result->count = 1;
    result->mean = value;
    result->m2 = 0.0;      /* No variance with single value */
    result->min = value;
    result->max = value;

    PG_RETURN_STATISTICS_P(result);
}
#include "utils/numeric.h"
Datum
statistics_init_numeric(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	char	   *tmp;
	Datum		float_value;
	double		value;
	Statistics *result;

	/* Convert numeric to double */
	tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
											  NumericGetDatum(num)));
	float_value = DirectFunctionCall1(float8in, CStringGetDatum(tmp));
	pfree(tmp);
	value = DatumGetFloat8(float_value);

    result = (Statistics *) palloc(sizeof(Statistics));

    result->count = 1;
    result->mean = value;
    result->m2 = 0.0;
    result->min = value;
    result->max = value;

    PG_RETURN_STATISTICS_P(result);
}

/*
 * Add a new value to existing statistics using Welford's algorithm
 * Usage: stats + 42.5 returns updated statistics
 */
Datum
statistics_add(PG_FUNCTION_ARGS)
{
    Statistics *stats = PG_GETARG_STATISTICS_P(0);
    double      value = PG_GETARG_FLOAT8(1);
    Statistics *result;
    double      delta, delta2;
    int64       new_count;

    /* Allocate result */
    result = (Statistics *) palloc(sizeof(Statistics));

    /* Welford's algorithm for incremental mean and variance */
    new_count = stats->count + 1;
    delta = value - stats->mean;

    result->count = new_count;
    result->mean = stats->mean + delta / new_count;

    delta2 = value - result->mean;
    result->m2 = stats->m2 + delta * delta2;

    /* Update min/max */
    result->min = (value < stats->min) ? value : stats->min;
    result->max = (value > stats->max) ? value : stats->max;

    PG_RETURN_STATISTICS_P(result);
}

/*
 * Accessor functions to get individual statistics
 */

Datum
statistics_get_count(PG_FUNCTION_ARGS)
{
    Statistics *stats = PG_GETARG_STATISTICS_P(0);
    PG_RETURN_INT64(stats->count);
}

Datum
statistics_get_mean(PG_FUNCTION_ARGS)
{
    Statistics *stats = PG_GETARG_STATISTICS_P(0);
    PG_RETURN_FLOAT8(stats->mean);
}

Datum
statistics_get_variance(PG_FUNCTION_ARGS)
{
    Statistics *stats = PG_GETARG_STATISTICS_P(0);
    double      variance;

    if (stats->count > 1)
        variance = stats->m2 / (stats->count - 1);
    else
        variance = 0.0;

    PG_RETURN_FLOAT8(variance);
}

Datum
statistics_get_stddev(PG_FUNCTION_ARGS)
{
    Statistics *stats = PG_GETARG_STATISTICS_P(0);
    double      variance, stddev;

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
statistics_get_min(PG_FUNCTION_ARGS)
{
    Statistics *stats = PG_GETARG_STATISTICS_P(0);
    PG_RETURN_FLOAT8(stats->min);
}

Datum
statistics_get_max(PG_FUNCTION_ARGS)
{
    Statistics *stats = PG_GETARG_STATISTICS_P(0);
    PG_RETURN_FLOAT8(stats->max);
}

/*
 * Equality comparison for statistics type
 * Two statistics objects are equal if all their fields match
 */
Datum
statistics_eq(PG_FUNCTION_ARGS)
{
    Statistics *stats1 = PG_GETARG_STATISTICS_P(0);
    Statistics *stats2 = PG_GETARG_STATISTICS_P(1);

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
statistics_get_field(PG_FUNCTION_ARGS)
{
    Statistics *stats = PG_GETARG_STATISTICS_P(0);
    text       *field_text = PG_GETARG_TEXT_PP(1);
    char       *field_name;
    double      result;

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
