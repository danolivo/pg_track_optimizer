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
 * Internal representation of running statistics
 *
 * Uses Welford's algorithm to maintain numerically stable running statistics.
 * This algorithm computes mean and variance in a single pass with excellent
 * numerical stability, avoiding catastrophic cancellation errors.
 *
 * Fixed-size type (40 bytes) - no varlena header needed.
 *
 * Empty state: count=0, other fields=-1 (sentinel values for validation)
 */
typedef struct RStats
{
	int64	count;	/* number of values accumulated */
	double	mean;	/* running mean (arithmetic average) */
	double	m2;		/* sum of squared differences from mean (for variance calculation) */
	double	min;	/* minimum value observed */
	double	max;	/* maximum value observed */
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

/* Add a value to existing statistics using Welford's algorithm.
 * Automatically initializes if called on an empty RStats object. */
extern void rstats_add_value(RStats *stats, double value);

/* Check if RStats is in empty (uninitialized) state */
extern bool rstats_is_empty(RStats *result);

/* Set RStats to empty state with sentinel values */
extern void rstats_set_empty(RStats *result);

#endif /* RSTATS_H */
