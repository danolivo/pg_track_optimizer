/*-------------------------------------------------------------------------
 *
 * plan_error.c
 *		Pass through a query plan and calculate estimation error.
 *
 * Copyright (c) 2024-2025 Andrei Lepikhov
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
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

	/* At first, increment the counter */
	ctx->counter++;

	tmp_counter = ctx->counter;
	planstate_tree_walker(pstate, prediction_walker, context);

	/*
	 * Finish the node before an analysis. And only after that we can touch any
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
	 * Calculate number of rows predicted by the optimizer and really passed
	 * through the node. This simplistic code becomes a bit tricky in the case
	 * of parallel workers.
	 */
	if (pstate->worker_instrument)
	{
		double	wnloops = 0.;
		double	wntuples = 0.;
		double	divisor = pstate->worker_instrument->num_workers;
		double	leader_contribution;
		int i;

		/* XXX: Copy-pasted from the get_parallel_divisor() */
		if (parallel_leader_participation)
		{
			leader_contribution = 1.0 - (0.3 * divisor);
			if (leader_contribution > 0)
				divisor += leader_contribution;
		}

		plan_rows = pstate->plan->plan_rows * divisor;

		for (i = 0; i < pstate->worker_instrument->num_workers; i++)
		{
			double t = pstate->worker_instrument->instrument[i].ntuples;
			double l = pstate->worker_instrument->instrument[i].nloops;

			if (l <= 0.0)
			{
				/*
				 * Worker could start but not to process any tuples just because
				 * of laziness. Skip such a node.
				 */
				continue;
			}

			wntuples += t;

			/* In leaf nodes we should get into account filtered tuples */
			if (tmp_counter == ctx->counter)
				wntuples += pstate->worker_instrument->instrument[i].nfiltered1 +
							pstate->worker_instrument->instrument[i].nfiltered2 +
							pstate->instrument->ntuples2;

			wnloops += l;
			real_rows += t/l;
		}

		Assert(nloops >= wnloops);

		/* Calculate the part of job have made by the main process */
		if (nloops - wnloops > 0.0)
		{
			double	ntuples = pstate->instrument->ntuples;

			/* In leaf nodes we should get into account filtered tuples */
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

		/* In leaf nodes we should get into account filtered tuples */
		if (tmp_counter == ctx->counter)
			real_rows += (pstate->instrument->nfiltered1 +
									pstate->instrument->nfiltered2 +
									pstate->instrument->ntuples2) / nloops;
	}

	plan_rows = clamp_row_est(plan_rows);
	real_rows = clamp_row_est(real_rows);

	/*
	 * Now, we can calculate a value of the estimation relative error has made
	 * by the optimizer.
	 */
	Assert(pstate->instrument->total > 0.0);

	ctx->error += fabs(log(real_rows / plan_rows));
	ctx->nnodes++;

	relative_time = pstate->instrument->total / pstate->instrument->nloops / ctx->totaltime;
	ctx->time_weighted_error += fabs(log(real_rows / plan_rows)) * relative_time;

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
plan_error(PlanState *pstate, double totaltime, PlanEstimatorContext *ctx)
{
	ctx->error = 0.;
	ctx->time_weighted_error = 0.;
	ctx->totaltime = totaltime;
	ctx->nnodes = 0;
	ctx->counter = 0;

	Assert(totaltime > 0.);
	(void) prediction_walker(pstate, (void *) ctx);
	return (ctx->nnodes > 0) ? (ctx->error / ctx->nnodes) : -1.0;
}
