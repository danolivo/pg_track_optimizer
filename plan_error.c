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

	if (!pstate->instrument)
		return false;

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

			wntuples += instr->ntuples;

			/*
			 * In leaf nodes we should get into account filtered tuples
			 *
			 * XXX:
			 * It seems that nfiltered1 counts the number of tuples filtered by
			 * a joinqual clause. The nfiltered2 counts the number of tuples,
			 * removed by an 'other' clause. So, should we consider them not
			 * only in leaf nodes?
			 */
			if (tmp_counter == ctx->counter)
				wntuples += instr->nfiltered1 + instr->nfiltered2 +
																instr->ntuples2;

			wnloops += instr->nloops;
			real_rows += instr->ntuples / instr->nloops;
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
