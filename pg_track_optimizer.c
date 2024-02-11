/*-------------------------------------------------------------------------
 *
 * pg_track_optimizer.c
 *		Passing through a query plan, detect planning issues.
 *
 * Copyright (c) 2024 Andrei Lepikhov
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * IDENTIFICATION
 *	  contrib/pg_track_optimizer/pg_track_optimizer.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/explain.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "optimizer/optimizer.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

#define track_optimizer_enabled(eflags) \
	( \
	IsQueryIdEnabled() && log_min_error >= 0 && \
	((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0) \
	)

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static double log_min_error = 0;

void _PG_init(void);

static double track_prediction_estimation(PlanState *pstate, double totaltime);

/*
 * Here we need it to enable instrumentation.
 */
static void
explain_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (track_optimizer_enabled(eflags))
		queryDesc->instrument_options |= INSTRUMENT_TIMER | INSTRUMENT_ROWS;

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (!track_optimizer_enabled(eflags))
		return;

	/*
	 * Set up to track total elapsed time in ExecutorRun.  Make sure the
	 * space is allocated in the per-query context so it will go away at
	 * ExecutorEnd.
	 */
	if (queryDesc->totaltime == NULL)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
		queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * Copy-paste from auto_explain code
 */
static void
_explain_statement(QueryDesc *queryDesc, double normalized_error)
{
	ExplainState   *es = NewExplainState();
	double			msec;

	msec = queryDesc->totaltime->total * 1000.0;

	/*
	 * We triggered by an estimation error. So, show only the options which
	 * can be useful to determine possible solution.
	 */
	es->analyze = (queryDesc->instrument_options);
	es->verbose = false;
	es->buffers = false;
	es->wal = false;
	es->timing = true;
	es->summary = true;
	es->format = EXPLAIN_FORMAT_TEXT;
	es->settings = true;

	ExplainBeginOutput(es);
	ExplainQueryText(es, queryDesc);
	ExplainPrintPlan(es, queryDesc);
	ExplainEndOutput(es);

	/* Remove last line break */
	if (es->str->len > 0 && es->str->data[es->str->len - 1] == '\n')
		es->str->data[--es->str->len] = '\0';

	/*
	 * Note: we rely on the existing logging of context or
	 * debug_query_string to identify just which statement is being
	 * reported.  This isn't ideal but trying to do it here would
	 * often result in duplication.
	 */
	ereport(LOG,
			(errmsg("duration: %.3f ms, relative error: %.4lf, plan:\n%s",
					msec, normalized_error, es->str->data),
			 errhidestmt(true)));
}

static void
track_ExecutorEnd(QueryDesc *queryDesc)
{
	MemoryContext	oldcxt;
	double			normalized_error = -1.0;

	if (!queryDesc->totaltime ||
		!track_optimizer_enabled(queryDesc->estate->es_top_eflags) ||
		queryDesc->plannedstmt->queryId == 0)
		/*
		 * Just to remember: a stranger extension can decide somewhere in the
		 * middle to change queryId, eflags or another global variable. So,
		 * trust only local variables and check the state whenever possible.
		 */
		goto end;

	/* TODO: need shared state 'status' instead of assertions */
	Assert(queryDesc->planstate->instrument &&
			queryDesc->instrument_options & INSTRUMENT_TIMER &&
			queryDesc->instrument_options & INSTRUMENT_ROWS);

	/*
	 * Make sure we operate in the per-query context, so any cruft will be
	 * discarded later during ExecutorEnd.
	 */
	oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);

	/*
	 * Make sure stats accumulation is done.  (Note: it's okay if several
	 * levels of hook all do this.)
	 */
	InstrEndLoop(queryDesc->totaltime);

	normalized_error = track_prediction_estimation(queryDesc->planstate,
												   queryDesc->totaltime->total);

	if (normalized_error >= 0.0)
		_explain_statement(queryDesc, normalized_error);
	MemoryContextSwitchTo(oldcxt);

end:
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

void
_PG_init(void)
{
	/*
	 * Inform the postmaster that we want to enable query_id calculation if
	 * compute_query_id is set to auto.
	 */
	EnableQueryId();

	DefineCustomRealVariable("pg_track_optimizer.log_min_error",
							 "Sets the minimum planning error above which plans will be logged.",
							 "Zero prints all plans. -1 turns this feature off.",
							 &log_min_error,
							 0,
							 -1, INT_MAX, /* Looks like so huge error, as INT_MAX don't make a sense */
							 PGC_SUSET,
							 GUC_UNIT_MS,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_track_optimizer");

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = explain_ExecutorStart;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = track_ExecutorEnd;
}

typedef struct ScourContext
{
	double	error;
	double	totaltime;
	int		nnodes;
	int 	counter; /* Used to detect leaf nodes. */
} ScourContext;

static bool
prediction_walker(PlanState *pstate, void *context)
{
	double			plan_rows,
					real_rows = 0;
	ScourContext   *ctx = (ScourContext *) context;
	double			relative_time;
	double			nloops;
	int				tmp_counter;

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
		leader_contribution = 1.0 - (0.3 * divisor);
		if (leader_contribution > 0)
			divisor += leader_contribution;
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

	relative_time = pstate->instrument->total /
									pstate->instrument->nloops / ctx->totaltime;
	ctx->error += fabs(log(real_rows / plan_rows)) * relative_time;
	ctx->nnodes++;

	return false;
}

static double
track_prediction_estimation(PlanState *pstate, double totaltime)
{
	ScourContext	ctx = {.error = 0, .totaltime = totaltime, .nnodes = 0, .counter = 0};

	Assert(totaltime > 0);
	(void) prediction_walker(pstate, (void *) &ctx);
	return (ctx.nnodes > 0) ? ctx.error : -1.0;
}
