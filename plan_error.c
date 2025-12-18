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
	 * Clarification: we take into account tuples that the nodes has filtered.
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
			 * In contrast, in leaf nodes we don't see prediction on how many
			 * tuples the planner anticipated to fetch from disk - only the nrows
			 * value of the final result. So, it makes sense to take them into
			 * account to highlight the potential issues.
			 */
			if (tmp_counter == ctx->counter)
				wntuples += instr->nfiltered1 + instr->nfiltered2 +
																instr->ntuples2;

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
			if (tmp_counter == ctx->counter)
				ntuples += (pstate->instrument->nfiltered1 +
												pstate->instrument->nfiltered2 +
												pstate->instrument->ntuples2);

			Assert(ntuples >= wntuples);
			real_rows += (ntuples - wntuples) / (nloops - wnloops);
		}
	}
	else
	{
		plan_rows = pstate->plan->plan_rows;
		real_rows = pstate->instrument->ntuples / nloops;

		/* In leaf nodes we should take into account filtered tuples */
		if (tmp_counter == ctx->counter)
			real_rows += (pstate->instrument->nfiltered1 +
									pstate->instrument->nfiltered2 +
									pstate->instrument->ntuples2) / nloops;
	}

	plan_rows = clamp_row_est(plan_rows);
	real_rows = clamp_row_est(real_rows);

	/*
	 * Now, we can calculate the value of the relative estimation error made
	 * by the optimiser.
	 */
	Assert(pstate->instrument->total > 0.0);

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

	/* Collect buffer usage statistics from this execution (sum of all types) */
	ctx->blks_accessed = queryDesc->totaltime->bufusage.shared_blks_hit +
						 queryDesc->totaltime->bufusage.shared_blks_read +
						 queryDesc->totaltime->bufusage.local_blks_hit +
						 queryDesc->totaltime->bufusage.local_blks_read +
						 queryDesc->totaltime->bufusage.temp_blks_read +
						 queryDesc->totaltime->bufusage.temp_blks_written;

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
