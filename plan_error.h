/*-------------------------------------------------------------------------
 *
 * plan_error.h
 *		Pass through a query plan and calculate estimation error.
 *
 * Copyright (c) 2024-2025 Andrei Lepikhov
 *
 * This software may be modified and distributed under the terms
 * of the MIT licence. See the LICENSE file for details.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLAN_ERROR_H
#define PLAN_ERROR_H

#include "executor/executor.h"
#include "nodes/nodeFuncs.h"

/*
 * Data structure used for error estimation as well as for statistics gathering.
 *
 * NOTES:
 * 1. Need to consider that cost may be potentially zero. What about totaltime?
 * 2. The wca_error behaves a little differently: normally, it should have a
 * positive value, or -1 if no nodes to be taken into account. Also, it may be
 * in [-1;0) range if total cost is zero.
 */
typedef struct PlanEstimatorContext
{
	double	totaltime;
	double	totalcost;

	/* Number of nodes assessed */
	int		nnodes;

	/*
	 * Total number of nodes in the plan. Originally used to detect leaf nodes.
	 * Now, it is a part of statistics.
	 */
	int 	counter;

	/* Different types of planning error may be placed here */
	double	avg_error;
	double	rms_error;
	double	twa_error;
	double	wca_error;

	/* Buffer usage statistics for this query execution */
	int64	blks_accessed;	/* Sum of all block hits, reads, and writes */
	int64	local_blks;		/* Local blocks (read + written + dirtied) - indicates work_mem issues */

	/* JOIN filtering statistics */
	double	max_jf_factor;

	/* Leaf node filtering statistics */
	double	max_lf_factor;

	/*
	 * Identify and track SubPlans.
	 *
	 * SubPlans are correlated subqueries that execute within plan nodes.
	 * They're not regular child nodes in the plan tree - instead they're
	 * referenced from expression nodes (quals, targetlists, etc).
	 *
	 * Each SubPlan executes multiple times (once per outer row). We track
	 * a dimensionless factor that indicates optimization potential:
	 *   sp_factor = (nloops / log(nloops + 1)) * (subplan_time / query_time)
	 *
	 * This factor can be compared across different queries to identify the
	 * most promising optimization candidates (highest values = best targets
	 * for JOIN conversion).
	 */
	double	worst_splan_factor;
} PlanEstimatorContext;

extern double plan_error(QueryDesc *queryDesc, PlanEstimatorContext *ctx);

#endif /* PLAN_ERROR_H */
