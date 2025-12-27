/*-------------------------------------------------------------------------
 *
 * EXAMPLE_CUSTOM_METRIC.c
 *		Example custom metric for pg_track_optimizer framework
 *
 * This example demonstrates how to create a custom metric that tracks
 * "sequential scan overestimation" - queries where the planner predicted
 * far fewer rows from sequential scans than were actually fetched.
 *
 * Use case: Identify tables with severely outdated statistics or
 * data skew that causes massive underestimation in sequential scans.
 *
 * Copyright (c) 2025 Your Name
 * Licensed under the MIT License
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "metric_framework.h"
#include "nodes/plannodes.h"
#include <math.h>

PG_MODULE_MAGIC;

/*
 * Sequential Scan Overestimation Metric
 *
 * For each SeqScan node, compute the ratio:
 *   overestimation_factor = actual_rows / estimated_rows
 *
 * Track the maximum overestimation factor across all SeqScan nodes.
 * High values indicate severely underestimated sequential scans.
 *
 * Example output:
 *   seqscan_max_overestimate = 100.0 → Scan returned 100x more rows than estimated
 *   seqscan_max_overestimate = 1.5   → Fairly accurate estimation
 */

static MetricValue
seqscan_overestimate_compute(const MetricContext *ctx)
{
	MetricValue result = {0};
	double overestimate_factor;

	result.storage_type = METRIC_STORAGE_RSTATS;
	result.is_valid = false;  /* Assume invalid until proven otherwise */

	/*
	 * Only interested in SeqScan nodes.
	 * Skip other node types (IndexScan, BitmapScan, etc.)
	 */
	if (!IsA(ctx->plan, SeqScan))
		return result;

	/*
	 * Need valid row counts to compute ratio.
	 * Skip if estimation or actual is zero (division by zero).
	 */
	if (ctx->plan_rows <= 0 || ctx->actual_rows <= 0)
		return result;

	/*
	 * Compute overestimation factor.
	 * Values > 1.0 indicate underestimation (actual > estimated).
	 * Values < 1.0 indicate overestimation (actual < estimated).
	 *
	 * We only care about underestimation (actual > estimated),
	 * so ignore cases where factor < 1.0.
	 */
	overestimate_factor = ctx->actual_rows / ctx->plan_rows;

	if (overestimate_factor < 1.0)
		return result;  /* Not underestimated, skip */

	/*
	 * Valid underestimation detected.
	 * Store the factor for tracking maximum.
	 */
	result.is_valid = true;
	result.value.double_val = overestimate_factor;

	return result;
}

/*
 * Finalization: Track the MAXIMUM overestimation factor.
 *
 * We don't average because we want to highlight the WORST
 * sequential scan in the query, not dilute it with good ones.
 *
 * The framework will track min/max/avg/stddev automatically
 * via RStats, so we just need to return the accumulated max.
 */
static MetricValue
seqscan_overestimate_finalize(double accumulated, int nnodes)
{
	MetricValue result = {0};

	result.storage_type = METRIC_STORAGE_RSTATS;
	result.is_valid = (nnodes > 0);

	if (nnodes > 0)
	{
		/*
		 * For RSTATS storage with max tracking, we just return
		 * the accumulated value. The RStats infrastructure will
		 * track the maximum across executions.
		 */
		result.value.double_val = accumulated;
	}
	else
	{
		/* No SeqScan nodes in this query */
		result.value.double_val = -1.0;
	}

	return result;
}

/*
 * Initialize storage for this metric.
 * Called when a new query entry is created in the DSM hash table.
 */
static void
seqscan_overestimate_init(void *storage_ptr, MetricStorage storage_type)
{
	RStats *stats = (RStats *) storage_ptr;

	Assert(storage_type == METRIC_STORAGE_RSTATS);

	/* Initialize with empty state (sentinel values) */
	rstats_set_empty(stats);
}

/*
 * Metric Descriptor
 *
 * This structure describes our metric to the framework.
 * Register it in _PG_init() to make it available.
 */
static MetricDescriptor seqscan_overestimate_metric = {
	/* Identity */
	.name = "seqscan_max_overestimate",
	.description = "Maximum sequential scan underestimation factor (actual/estimated rows)",

	/* Behavior */
	.type = METRIC_TYPE_PER_NODE,          /* Compute for each plan node */
	.storage = METRIC_STORAGE_RSTATS,      /* Store as running statistics */

	/* Callbacks */
	.compute = seqscan_overestimate_compute,
	.finalize = seqscan_overestimate_finalize,
	.initialize = seqscan_overestimate_init,

	/* Configuration */
	.enabled_default = false,  /* Opt-in metric (not enabled by default) */
	.priority = 100,           /* Display order (higher = later in list) */
	.storage_size = sizeof(RStats)
};

/*
 * Extension initialization.
 *
 * Called when the extension is loaded via:
 *   CREATE EXTENSION pg_track_seqscan_metrics;
 */
void
_PG_init(void)
{
	/*
	 * Register our custom metric with the pg_track_optimizer framework.
	 * This makes it available for use via the metrics GUC.
	 */
	RegisterMetric(&seqscan_overestimate_metric);

	elog(LOG, "pg_track_seqscan_metrics loaded: added 'seqscan_max_overestimate' metric");
}

/*
 * Extension cleanup (optional).
 */
void
_PG_fini(void)
{
	/* Nothing to clean up */
}

/*
 * ============================================================================
 * USAGE EXAMPLE
 * ============================================================================
 *
 * 1. Compile and install:
 *
 *    gcc -fPIC -c EXAMPLE_CUSTOM_METRIC.c \
 *        -I$(pg_config --includedir-server)
 *    gcc -shared -o pg_track_seqscan_metrics.so EXAMPLE_CUSTOM_METRIC.o
 *    sudo cp pg_track_seqscan_metrics.so $(pg_config --pkglibdir)
 *
 * 2. Load the extension:
 *
 *    CREATE EXTENSION pg_track_seqscan_metrics;
 *
 * 3. Enable the metric:
 *
 *    SET pg_track_optimizer.metrics = 'avg_error,seqscan_max_overestimate';
 *
 * 4. Run queries and observe:
 *
 *    SELECT queryid, query,
 *           seqscan_max_overestimate -> 'max' AS worst_seqscan_underestimate,
 *           seqscan_max_overestimate -> 'avg' AS avg_seqscan_underestimate
 *    FROM pg_track_optimizer
 *    WHERE seqscan_max_overestimate -> 'max' > 10.0  -- 10x underestimation
 *    ORDER BY seqscan_max_overestimate -> 'max' DESC
 *    LIMIT 10;
 *
 *    Example output:
 *      queryid  |           query              | worst | avg
 *    -----------+------------------------------+-------+------
 *      12345678 | SELECT * FROM big_table ...  | 873.2 | 245.7
 *      87654321 | SELECT * FROM old_stats ...  | 156.4 | 156.4
 *      ...
 *
 *    Interpretation:
 *      - Query 12345678 had a SeqScan that returned 873x more rows than estimated
 *      - Across all executions, average underestimation was 245x
 *      - This table desperately needs ANALYZE or has severe data skew!
 *
 * 5. Investigate:
 *
 *    EXPLAIN (ANALYZE, BUFFERS) <the bad query>;
 *    ANALYZE <the table>;
 *
 * ============================================================================
 */

/*
 * ============================================================================
 * ADVANCED EXAMPLE: Per-Execution Metric
 * ============================================================================
 *
 * Here's how you'd write a per-execution metric (computed once per query
 * instead of once per node):
 */

#if 0  /* Commented out - just an example */

static MetricValue
total_planning_time_compute(const MetricContext *ctx)
{
	MetricValue result = {0};

	result.storage_type = METRIC_STORAGE_RSTATS;
	result.is_valid = true;

	/*
	 * For per-execution metrics, ctx->queryDesc contains
	 * the full QueryDesc with planning information.
	 */
	result.value.double_val = ctx->queryDesc->plannedstmt->planningTime;

	return result;
}

static MetricDescriptor planning_time_metric = {
	.name = "planning_time",
	.description = "Query planning time in milliseconds",
	.type = METRIC_TYPE_PER_EXECUTION,  /* Once per query, not per node */
	.storage = METRIC_STORAGE_RSTATS,
	.compute = total_planning_time_compute,
	.finalize = NULL,  /* No finalization needed */
	.initialize = seqscan_overestimate_init,  /* Reuse RStats init */
	.enabled_default = false,
	.priority = 110,
	.storage_size = sizeof(RStats)
};

#endif  /* End of advanced example */

/*
 * ============================================================================
 * COUNTER METRIC EXAMPLE
 * ============================================================================
 *
 * For simple counters (not statistical aggregates):
 */

#if 0  /* Commented out - just an example */

static MetricValue
hash_join_count_compute(const MetricContext *ctx)
{
	MetricValue result = {0};

	result.storage_type = METRIC_STORAGE_COUNTER;
	result.is_valid = true;

	/* Count HashJoin nodes */
	if (IsA(ctx->plan, HashJoin))
		result.value.int64_val = 1;
	else
		result.value.int64_val = 0;

	return result;
}

static void
hash_join_count_init(void *storage_ptr, MetricStorage storage_type)
{
	int64 *counter = (int64 *) storage_ptr;

	Assert(storage_type == METRIC_STORAGE_COUNTER);
	*counter = 0;
}

static MetricDescriptor hash_join_count_metric = {
	.name = "hash_join_count",
	.description = "Number of hash join nodes in query plan",
	.type = METRIC_TYPE_PER_NODE,
	.storage = METRIC_STORAGE_COUNTER,  /* Simple integer counter */
	.compute = hash_join_count_compute,
	.finalize = NULL,
	.initialize = hash_join_count_init,
	.enabled_default = false,
	.priority = 120,
	.storage_size = sizeof(int64)  /* 8 bytes instead of 32 for RStats */
};

#endif  /* End of counter example */
