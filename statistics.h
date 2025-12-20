/*
 * statistics.h - header for incremental statistics type
 *
 * Copyright (c) 2024-2025 Andrei Lepikhov
 *
 * This software may be modified and distributed under the terms
 * of the MIT licence. See the LICENSE file for details.
 */
#ifndef STATISTICS_H
#define STATISTICS_H

#include "postgres.h"

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

/*
 * Internal functions for manipulating Statistics objects
 * These can be used by other modules without going through Datum interface
 */

/* Initialize a Statistics object from a single value */
extern void statistics_init_internal(Statistics *result, double value);

/* Add a value to existing statistics using Welford's algorithm */
extern void statistics_add_value(Statistics *stats, double value);

#endif /* STATISTICS_H */
