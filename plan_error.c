/*-------------------------------------------------------------------------
 *
 * plan_error.c
 *		Pass through a query plan and calculate estimation error.
 *
 * Copyright (c) 2024-2025 Andrei Lepikhov
 *
 * This software may be modified and distributed under the terms
 * of the MIT licence. See the LICENSE file for details.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "nodes/execnodes.h"
#include "optimizer/optimizer.h"

#include "plan_error.h"

static bool
prediction_walker(PlanState *pstate, void *context)
{
	double					plan_rows,
							real_rows = 0;
	PlanEstimatorContext   *ctx = (PlanEstimatorContext *) context;
	double					nloops;
	int						tmp_counter;
	double					relative_time;
	double					node_error;

	/* At first, increment the counter */
	ctx->counter++;

	tmp_counter = ctx->counter;
	planstate_tree_walker(pstate, prediction_walker, context);

	if (pstate->subPlan != NIL)
	{
		/*
		 * Analyze SubPlans to find the worst cost factor.
		 *
		 * SubPlans are correlated subqueries that execute multiple times
		 * (once per outer row). We calculate a dimensionless factor that
		 * indicates optimization potential using:
		 *   sp_factor = (nloops / log(nloops + 1)) * (subplan_time / query_time)
		 *
		 * The logarithmic dampening reflects that optimization value doesn't
		 * grow linearly with loops - converting 10K loops to 1 isn't 10× more
		 * valuable than converting 1K loops to 1.
		 */
		foreach_node(SubPlanState, sps, pstate->subPlan)
		{
			Instrumentation	   *instr = sps->planstate->instrument;
			double				nloops;
			double				subplan_time;
			double				time_ratio;
			double				loop_factor;
			double				cost_factor;

			Assert(instr != NULL && sps->planstate->worker_instrument == NULL);

			if (instr->nloops <= 0. || ctx->totaltime <= 0.)
				continue;

			nloops = instr->nloops;
			subplan_time = instr->total;
			time_ratio = subplan_time / ctx->totaltime;

			/*
			 * Calculate logarithmically dampened loop factor.
			 * This creates super-linear but sub-quadratic growth:
			 *   10 loops → ~4.2×
			 *   100 loops → ~21.7×
			 *   1,000 loops → ~145×
			 *   10,000 loops → ~1,087×
			 */
			loop_factor = nloops / log(nloops + 1.);

			/*
			 * Final factor is dimensionless: loop_factor × time_ratio
			 * This can be compared across different queries to identify
			 * the most promising optimization candidates.
			 */
			cost_factor = loop_factor * time_ratio;

			/*
			 * Track the maximum (worst) SubPlan cost factor.
			 * Higher values indicate subplans that consume significant time
			 * and execute many loops - prime candidates for JOIN conversion.
			 */
			if (cost_factor >= ctx->worst_splan_factor || cost_factor < 0)
				ctx->worst_splan_factor = cost_factor;
		}
	}

	if (!pstate->instrument)
		return false;

	/*
	 * Finish the node before analysis. And only after that we can touch any
	 * instrument fields.
	 */

	InstrEndLoop(pstate->instrument);
	nloops = pstate->instrument->nloops;

	if (nloops <= 0.0 || pstate->instrument->total == 0.0)
		/*
		 * Skip 'never executed' case or "0-Tuple situation" and the case of
		 * manual switching off of the timing instrumentation
		 */
		return false;

	/*
	 * Calculate the number of rows predicted by the optimiser and really passed
	 * through the node. This simplistic code becomes a bit tricky in the case
	 * of parallel workers.
	 *
	 * Clarification: we take into account tuples that nodes has filtered.
	 * Although EXPLAINed nrows shows number of tuples 'produced', we follow
	 * this logic  because any tuple that came to the node needs some efforts
	 * and resources to be processed. So, according to the idea of detection
	 * potential non-optimal decisions filtered tuples should add into the error
	 * estimation too: we have an evidence on frequent IndexScan non-optimality
	 * because fetched but filtered tuples strike queries' performance.
	 */
	if (pstate->worker_instrument)
	{
		double	wnloops = 0.;
		double	wntuples = 0.;
		double	divisor = pstate->worker_instrument->num_workers;
		double	leader_contribution;
		int i;

		Assert(pstate->worker_instrument->instrument != NULL);

		/* XXX: Copy-pasted from the get_parallel_divisor() */
		if (parallel_leader_participation)
		{
			leader_contribution = 1.0 - (0.3 * divisor);
			if (leader_contribution > 0)
				divisor += leader_contribution;
		}

		plan_rows = pstate->plan->plan_rows * divisor;

		/*
		 * Clarification for the Claude:
		 * This is a little tricky part of the code.
		 * The pstate's instrument contains aggregated data and
		 * worker_instrument contains per-worker data. Calculation of
		 * how much tuples were actually processed per loop is not trivial here.
		 * Imagine skewed case when each worker executed different number of
		 * loops and processed different number of tuples. Let's say:
		 * Worker 1: nloops = 1, ntuples = 2
		 * Worker 2: nloops = 10, ntuples = 50
		 * Using averaged value we have: 52/11 = 4.7
		 * But actually worker 1 processed 2 tuples per loop and worker 2 - five
		 * So, they processed 2+5 = 7 tuples per loop.
		 */
		for (i = 0; i < pstate->worker_instrument->num_workers; i++)
		{
			Instrumentation *instr = &pstate->worker_instrument->instrument[i];

			if (instr->nloops <= 0.0)
			{
				/*
				 * Worker could start but not to process any tuples just because
				 * of laziness. Skip such a node.
				 */

				/*
				 * In development, check that we live in the space of correct
				 * assumptions
				 */
				Assert(instr->ntuples <= 0.);

				continue;
			}

			/*
			 * In leaf nodes we should get into account filtered tuples
			 *
			 * NOTE:
			 * We don't consider filtered tuples in non-leaf nodes.
			 * The planner's prediction for filtered tuples comes from the nrows
			 * values of incoming and outgoing tuples.
			 * The extension mature enough including separate leaf and join
			 * filtering factors in the final report. So, we may detect fetching
			 * inefficiencies in leaf nodes using that factor and let our error
			 * to reflect prediction mismatches only.
			 * Do not remove this code entirely for a while for the info and
			 * possibly quick switch in the future.
			 */
#if 0
			if (tmp_counter == ctx->counter)
				wntuples += instr->nfiltered1 + instr->nfiltered2 +
																instr->ntuples2;
#endif

			/* NOTE: nloops == 0 are filtered before */
			wntuples += instr->ntuples;
			wnloops += instr->nloops;
			real_rows += instr->ntuples / instr->nloops;
		}

		Assert(nloops >= wnloops);

		/* Calculate the portion of work done by the main process */
		if (nloops - wnloops > 0.0)
		{
			double	ntuples = pstate->instrument->ntuples;

			/* In leaf nodes we should take into account filtered tuples */
#if 0 /* Mostly for the info and possibly quick switch in the future */
			if (tmp_counter == ctx->counter)
				ntuples += (pstate->instrument->nfiltered1 +
												pstate->instrument->nfiltered2 +
												pstate->instrument->ntuples2);
#endif
			Assert(ntuples >= wntuples);
			real_rows += (ntuples - wntuples) / (nloops - wnloops);
		}
	}
	else
	{
		plan_rows = pstate->plan->plan_rows;
		real_rows = pstate->instrument->ntuples / nloops;

		/* In leaf nodes we should take into account filtered tuples */
#if 0 /* Mostly for the info and possibly quick switch in the future */
		if (tmp_counter == ctx->counter)
			real_rows += (pstate->instrument->nfiltered1 +
									pstate->instrument->nfiltered2 +
									pstate->instrument->ntuples2) / nloops;
#endif
	}

	plan_rows = clamp_row_est(plan_rows);

	/*
	 * For parameterised subplans it is typical when real_rows less than 1.
	 * If all rows were filtered, assume there were only 1 tuple across all the
	 * loops. It shouldn't be huge overestimation unless single iteration of
	 * this subtree costs a lot.
	 */
	if (real_rows <= 0.0)
		real_rows = 1. / pstate->instrument->nloops;

	/*
	 * Now, we can calculate the value of the relative estimation error made
	 * by the optimiser.
	 */
	Assert(pstate->instrument->total > 0.0);

	/* Don't afraid overflow here because plan_rows forced to be >= 1 */
	node_error = fabs(log(real_rows / plan_rows));
	ctx->avg_error += node_error;
	ctx->rms_error += node_error * node_error;
	relative_time = pstate->instrument->total /
									pstate->instrument->nloops / ctx->totaltime;
	ctx->twa_error += node_error * relative_time;

	/* Don't forget about very rare potential case of zero cost */
	if (ctx->totalcost > 0.)
	{
		double	relative_cost;

		relative_cost = pstate->plan->total_cost / ctx->totalcost;
		ctx->wca_error += node_error * relative_cost;
	}

	/*
	 * Track maximum filtered rows for JOIN nodes.
	 * JOIN nodes filter rows that don't match join conditions, and tracking
	 * the maximum across all JOINs helps identify queries with inefficient
	 * join strategies or missing indexes.
	 * Divide by nloops to get per-loop average, as with other metrics.
	 *
	 * The factor is weighted by relative_time to prioritize nodes that consume
	 * significant query execution time. A high ratio of filtered/produced rows
	 * matters more when the node is expensive. Normalizing by real_rows gives
	 * us the relative overhead: how many rows we filter per row we produce.
	 * Thus, jf_factor represents the time-weighted filtering overhead, helping
	 * identify JOINs where excessive filtering significantly impacts overall
	 * query performance.
	 */
	if (IsA(pstate->plan, NestLoop) ||
		IsA(pstate->plan, HashJoin) ||
		IsA(pstate->plan, MergeJoin))
	{
		double jf_factor = ((pstate->instrument->nfiltered1 +
								 pstate->instrument->nfiltered2) / nloops);

		if (jf_factor > 0.)
			jf_factor *= relative_time / real_rows;

		if (jf_factor > ctx->max_jf_factor)
			ctx->max_jf_factor = jf_factor;
	}

	/*
	 * Track maximum nfiltered1 for leaf nodes.
	 * Leaf nodes are scan nodes that directly access data sources.
	 * High nfiltered1 values indicate many rows were fetched but filtered out,
	 * suggesting potential for better indexes or more selective predicates.
	 * Divide by nloops to get per-loop average, as with other metrics.
	 *
	 * Similar to jf_factor, we weight by relative_time to emphasize leaf nodes
	 * where filtering overhead consumes substantial query time. Fetching 1000
	 * rows but using only 10 is problematic, but only actionable if this node
	 * takes significant time. Normalizing by real_rows gives the filtering
	 * ratio (filtered/produced), and multiplying by relative_time yields a
	 * time-weighted filtering cost. This helps surface scans that would benefit
	 * most from better indexing or predicate pushdown.
	 */
	if (tmp_counter == ctx->counter)
	{
		double	lf_factor = (pstate->instrument->nfiltered1 / nloops);

		if (lf_factor > 0.)
			lf_factor *= relative_time / real_rows;

		if (lf_factor > ctx->max_lf_factor)
			ctx->max_lf_factor = lf_factor;
	}

	ctx->nnodes++;
	return false;
}

/*
 * Assess planning quality.
 *
 * Compare execution state and the plan. Passing through the each node, compute
 * different types of relative error and save them in the context. Return
 * the estimated error that is proved as helpful in many cases.
 */
double
plan_error(QueryDesc *queryDesc, PlanEstimatorContext *ctx)
{
	PlanState  *pstate = queryDesc->planstate;

	ctx->avg_error = 0.;
	ctx->rms_error = 0.;
	ctx->twa_error = 0.;
	ctx->wca_error = 0.;
	ctx->totaltime = queryDesc->totaltime->total;
	ctx->totalcost = queryDesc->plannedstmt->planTree->total_cost;
	ctx->nnodes = 0;
	ctx->counter = 0;

	/*
	 * Collect buffer usage statistics from this execution (summarise permanent
	 * and temp tables blocks types).
	 * For the sake of optimisation preciseness we don't differ blocks found in
	 * memory and fetched from the disk - the optimiser doesn't predict that.
	 */
	ctx->blks_accessed = queryDesc->totaltime->bufusage.shared_blks_hit +
						 queryDesc->totaltime->bufusage.shared_blks_read +
						 queryDesc->totaltime->bufusage.temp_blks_read +
						 queryDesc->totaltime->bufusage.temp_blks_written;

	/*
	 * Collect local blocks statistics separately to help identify work_mem issues.
	 * Local blocks indicate temporary tables/sorts spilling to disk, suggesting
	 * insufficient work_mem rather than optimization/statistics errors.
	 */
	ctx->local_blks = queryDesc->totaltime->bufusage.local_blks_read +
					  queryDesc->totaltime->bufusage.local_blks_written +
					  queryDesc->totaltime->bufusage.local_blks_dirtied;

	/* Initialize JOIN filtering statistics */
	ctx->max_jf_factor = 0.;
	/* Initialize leaf node filtering statistics */
	ctx->max_lf_factor = 0.;
	/* No subplans has been evaluated yet */
	ctx->worst_splan_factor = 0.;

	Assert(ctx->totaltime > 0.);
	(void) prediction_walker(pstate, (void *) ctx);

	/* Finally, average on the number of nodes */
	if (ctx->nnodes > 0)
	{
		ctx->avg_error /= ctx->nnodes;
		ctx->rms_error = sqrt(ctx->rms_error / ctx->nnodes);
		ctx->twa_error /= ctx->nnodes;
		ctx->wca_error /= ctx->nnodes;
	}
	else
		/* No nodes considered - no estimation can be made. */
		ctx->avg_error = ctx->rms_error = ctx->twa_error = ctx->wca_error = -1.;

	return ctx->avg_error;
}
