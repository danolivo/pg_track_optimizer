/*
 * rstats.h - header for incremental statistics type
 *
 * Copyright (c) 2024-2025 Andrei Lepikhov
 *
 * This software may be modified and distributed under the terms
 * of the MIT licence. See the LICENSE file for details.
 */
#ifndef RSTATS_H
#define RSTATS_H

#include "postgres.h"

/*
 * Internal representation of statistics
 * Uses Welford's algorithm to maintain numerically stable running statistics
 * Fixed-size type (40 bytes) - no varlena header needed
 */
typedef struct RStats
{
	int64	count;	/* number of values */
	double	mean;	/* running mean */
	double	m2;		/* sum of squared differences from mean (for variance) */
	double	min;	/* minimum value */
	double	max;	/* maximum value */
} RStats;

/* Macros for easier access */
#define DatumGetRStatsP(X) ((RStats *) DatumGetPointer(X))
#define RStatsPGetDatum(X) PointerGetDatum(X)
#define PG_GETARG_RSTATS_P(n) DatumGetRStatsP(PG_GETARG_DATUM(n))
#define PG_RETURN_RSTATS_P(x) return RStatsPGetDatum(x)

/*
 * Internal functions for manipulating RStats objects
 * These can be used by other modules without going through Datum interface
 */

/* Initialize a RStats object from a single value */
extern void rstats_init_internal(RStats *result, double value);

/* Add a value to existing statistics using Welford's algorithm */
extern void rstats_add_value(RStats *stats, double value);

extern bool rstats_is_empty(RStats *result);
extern void rstats_set_empty(RStats *result);

#endif /* RSTATS_H */
