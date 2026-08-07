/*-------------------------------------------------------------------------
 *
 * pg_track_optimizer.c
 *		Passing through a query plan, detect planning issues.
 *
 * Copyright (c) 2024-2025 Andrei Lepikhov
 *
 * This software may be modified and distributed under the terms
 * of the MIT licence. See the LICENSE file for details.
 *
 * IDENTIFICATION
 *	  contrib/pg_track_optimizer/pg_track_optimizer.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif
#include "executor/executor.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "pgstat.h"
#include "port/pg_crc32c.h"
#include "storage/dsm_registry.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "plan_error.h"
#include "rstats.h"

#if (PG_VERSION_NUM < 180000)
PG_MODULE_MAGIC;
#else
PG_MODULE_MAGIC_EXT(
					.name = "pg_track_optimizer",
					.version = "0.9.3"
);
#endif

#define DATATBL_NCOLS	(17)

/* INJECTION_POINT() grew a second argument in PostgreSQL 18 */
#if PG_VERSION_NUM >= 180000
#define PGTO_INJECTION_POINT(name) INJECTION_POINT(name, NULL)
#else
#define PGTO_INJECTION_POINT(name) INJECTION_POINT(name)
#endif

typedef struct TODSMRegistry
{
	LWLock				lock;
	int					tranche_id;
	dshash_table	   *htab;
	dsa_handle			dsah;
	dshash_table_handle	dshh;

	/*
	 * An atomic counter keeps track of the number of entries in the hash table.
	 * Usage:
	 * - Cheap access to the number of elements of the shared HTAB.
	 * - Use the HTAB dump code to write the number of entries to the file.
	 * Design choice explanation: should be accessible on read without acquiring
	 * the HTAB lock (scalability in read). Must be written only under the lock.
	 * Exception behaviour: in case of an inconsistency detected (it should be
	 * checked each time we scan the whole HTAB), it must be a FATAL error that
	 * should cause the HTAB to be reset, along with a descriptive error.
	 */
	pg_atomic_uint32	htab_counter;

	pg_atomic_uint32	need_syncing;
} TODSMRegistry;

/*
 * Key for the tracker hash table.
 * Since it is impossible to imagine when a query could be used in another
 * database, use the database oid to reduce chance of collision and possible
 * other filtering options.
 */
typedef struct DSMOptimizerTrackerKey
{
	Oid			dbOid;
	uint64		queryId;
} DSMOptimizerTrackerKey;

/*
 * Entry in the optimiser tracking hash table.
 *
 * Contains both per-execution snapshots (overwritten each time) and
 * cumulative statistics (accumulated across all executions).
 */
typedef struct DSMOptimizerTrackerEntry
{
	DSMOptimizerTrackerKey	key;

	/* Per-execution statistics (most recent execution only - snapshots) */
	int32					evaluated_nodes;	/* Number of plan nodes evaluated (last execution) */
	int32					plan_nodes;			/* Total number of plan nodes (last execution) */

	/* Cumulative statistics (accumulated across all executions) */
	RStats					avg_error;			/* Average estimation error - running stats */
	RStats					rms_error;			/* Root mean square error - running stats */
	RStats					twa_error;			/* Time-weighted average error - running stats */
	RStats					wca_error;			/* Weighted Cost Average error - running stats */
	RStats					blks_accessed;		/* Block I/O (hits + reads + writes) - running stats */
	RStats					temp_blks;			/* Temp blocks (read + written) - work_mem indicator */
	RStats					exec_time;			/* Execution time per query - running stats (milliseconds) */
	RStats					f_join_filter;	/* Maximum filtered rows (nfiltered1+nfiltered2) across JOIN nodes */
	RStats					f_scan_filter;	/* Maximum nfiltered1 for leaf nodes in the query plan */
	RStats					f_worst_splan;	/* Worst SubPlan factor: (nloops/log(nloops+1)) * (time/total_time) */
	RStats					njoins;			/* Number of JOIN nodes per execution - running stats */
	int64					nexecs;				/* Number of executions tracked */

	/*
	 * Metadata.
	 *
	 * query_ptr doubles as the entry's validity marker.  A writer sets it to
	 * InvalidDsaPointer as the very first store after inserting the entry and
	 * assigns the real allocation as the very last store of initialization,
	 * so an invalid pointer means "initialization did not complete".  Readers
	 * (sequential scans, flush, reset) skip such entries and the next writer
	 * of the same key rebuilds them, which keeps a failure anywhere in the
	 * initialization window - the query text allocation included - from ever
	 * exposing a half-built entry.
	 *
	 * NB: the allocation is therefore performed into a local variable and
	 * published in a single store.  Keep it the last failure-prone operation
	 * of the window: an error between a successful allocation and that store
	 * orphans the allocation (nothing references it, so not even reset can
	 * reclaim it - only a hash table reset that destroys the DSA does).
	 *
	 * Accessed only under the entry's dshash partition lock; LWLock
	 * acquire/release provides the required memory barriers.
	 */
	dsa_pointer				query_ptr;			/* Pointer to query text in shared memory */
} DSMOptimizerTrackerEntry;

static const dshash_parameters dsh_params = {
	sizeof(DSMOptimizerTrackerKey),
	sizeof(DSMOptimizerTrackerEntry),
	dshash_memcmp,
	dshash_memhash,
#if PG_VERSION_NUM >= 170000
	dshash_memcpy,
#endif
	LWTRANCHE_PGSTATS_HASH
};

static TODSMRegistry *shared = NULL;
static dsa_area *htab_dsa = NULL;
static dshash_table *htab = NULL;

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/*
 * The module's work modes:
 * - NORMAL - track the query if the log_min_error threshold is exceeded
 * - FORCED - track each query
 * - DISABLED - do not track any queries
 *
 * XXX: What about DML commands? It seems they use nfilteredX/ntuples2
 * instrumentation fields in a way we don't count here. Is it a subject for
 * correction our method?
 */
typedef enum
{
	TRACK_MODE_NORMAL,
	TRACK_MODE_FORCED,
	/* XXX: Do we need 'frozen' mode ? */
	TRACK_MODE_DISABLED,
} TrackMode;

static const struct config_enum_entry format_options[] = {
	{"normal", TRACK_MODE_NORMAL, false},
	{"forced", TRACK_MODE_FORCED, false},
	{"disabled", TRACK_MODE_DISABLED, false},
	{NULL, 0, false}
};

/*
 * Instrumentation effort levels.  The per-tuple cost of tracking is driven
 * by which executor instrumentation we request, so let the user choose how
 * much to pay:
 *
 * - ROWS: per-node row counters only.  No clock reads.  Still computes the
 *   estimation-error metrics that drive detection (avg/rms/wca error) plus
 *   query-level execution time and buffer statistics, which come from the
 *   cheap query-level instrumentation.  Time-weighted metrics (twa_error,
 *   filter factors, SubPlan factor) are not collected.
 * - TIMING: adds per-node INSTRUMENT_TIMER - two clock reads per tuple per
 *   node - enabling all time-weighted metrics.  This is the default and
 *   matches the historical behaviour metric-wise.
 * - FULL: adds per-node INSTRUMENT_BUFFERS and includes buffer usage in the
 *   logged EXPLAIN output.  The per-node buffer counters are not used for
 *   any stored metric (block statistics come from the query-level
 *   instrumentation), so this level is only useful when the logged plans
 *   need per-node buffer detail.
 */
typedef enum
{
	TRACK_EFFORT_ROWS,
	TRACK_EFFORT_TIMING,
	TRACK_EFFORT_FULL,
} TrackEffort;

static const struct config_enum_entry effort_options[] = {
	{"rows", TRACK_EFFORT_ROWS, false},
	{"timing", TRACK_EFFORT_TIMING, false},
	{"full", TRACK_EFFORT_FULL, false},
	{NULL, 0, false}
};

static int track_mode = TRACK_MODE_DISABLED;
static int track_effort = TRACK_EFFORT_TIMING;
static double log_min_error = -1.0;
static int hash_mem = 4096;
static bool auto_flush = true;

/* Per-plan-node instrumentation flags implied by the current effort level */
static inline int
effort_instrument_options(void)
{
	int		options = INSTRUMENT_ROWS;

	if (track_effort >= TRACK_EFFORT_TIMING)
		options |= INSTRUMENT_TIMER;
	if (track_effort >= TRACK_EFFORT_FULL)
		options |= INSTRUMENT_BUFFERS;

	return options;
}

void _PG_init(void);

static uint32 _load_hash_table_safe(TODSMRegistry *state);
static uint32 _flush_hash_table(void);
static void pto_before_shmem_exit(int code, Datum arg);

static inline bool
track_optimizer_enabled(QueryDesc *queryDesc, int eflags)
{
	if (IsQueryIdEnabled() && !IsParallelWorker() &&
		queryDesc->plannedstmt->utilityStmt == NULL &&
		(log_min_error >= 0. || track_mode == TRACK_MODE_FORCED) &&
		track_mode != TRACK_MODE_DISABLED &&
		((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0))
		return true;

	return false;
}

/*
 * First-time initialization code. Secured by the lock on DSM registry.
 */
static void
#if PG_VERSION_NUM < 190000
to_init_shmem(void *ptr)
#else
to_init_shmem(void *ptr, void *arg)
#endif
{
	TODSMRegistry	   *state = (TODSMRegistry *) ptr;

	Assert(htab_dsa == NULL && htab == NULL);

#if PG_VERSION_NUM < 190000
	LWLockInitialize(&state->lock, LWLockNewTrancheId());
	state->tranche_id = LWLockNewTrancheId();
	LWLockRegisterTranche(state->tranche_id, "pgto_dshash_tranche");
#else
	LWLockInitialize(&state->lock,
					 LWLockNewTrancheId("pgto_lock_tranche"));
	state->tranche_id = LWLockNewTrancheId("pgto_dshash_tranche");
#endif
	htab_dsa = dsa_create(state->tranche_id);
	state->dsah = dsa_get_handle(htab_dsa);
	dsa_pin(htab_dsa);

	htab = dshash_create(htab_dsa, &dsh_params, 0);
	state->dshh = dshash_get_hash_table_handle(htab);
	pg_atomic_init_u32(&state->htab_counter, 0);
	pg_atomic_init_u32(&state->need_syncing, 0);

	_load_hash_table_safe(state);
}

/*
 * Using DSM for shared memory segments we need to check attachment at each
 * point where we are going to use it.
 */
static void
track_attach_shmem(void)
{
	bool			found;
	MemoryContext	mctx;

	if (htab != NULL)
		return;

	mctx = MemoryContextSwitchTo(TopMemoryContext);

#if PG_VERSION_NUM < 190000
	shared = GetNamedDSMSegment("pg_track_optimizer",
								   sizeof(TODSMRegistry),
								   to_init_shmem,
								   &found);
#else
	shared = GetNamedDSMSegment("pg_track_optimizer",
								   sizeof(TODSMRegistry),
								   to_init_shmem,
								   &found, NULL);
#endif

	if (found)
	{
		Assert(shared->dshh != DSHASH_HANDLE_INVALID);

		htab_dsa = dsa_attach(shared->dsah);
		/* Attach to existing hash table */
		htab = dshash_attach(htab_dsa, &dsh_params, shared->dshh, NULL);
	}

	dsa_pin_mapping(htab_dsa);
	MemoryContextSwitchTo(mctx);

	/*
	 * Register the on-exit flush callback in this backend.  Registering it
	 * in _PG_init() is not enough: under shared_preload_libraries _PG_init()
	 * runs in the postmaster only, whose on_exit callbacks are cleared in
	 * forked children (and the postmaster itself fails the IsUnderPostmaster
	 * check in the callback), so auto_flush would never fire anywhere.  This
	 * function runs once per backend - the htab test above short-circuits
	 * all later calls - which is exactly the registration point we need.
	 */
	before_shmem_exit(pto_before_shmem_exit, (Datum) 0);
}

/*
 * Here we need it to enable instrumentation.
 */
static void
explain_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	track_attach_shmem();

	if (track_optimizer_enabled(queryDesc, eflags))
	{
		queryDesc->instrument_options |= effort_instrument_options();
#if PG_VERSION_NUM >= 190000
		/*
		 * Ask core to set up query-level instrumentation for us.  This MUST
		 * be done before standard_ExecutorStart: core inspects
		 * query_instr_options there and allocates query_instr only if it is
		 * non-zero (see standard_ExecutorStart in execMain.c).  Setting the
		 * options later has no effect, and plan_error() needs both timing
		 * and buffer usage from query_instr.
		 */
		queryDesc->query_instr_options |= INSTRUMENT_TIMER | INSTRUMENT_BUFFERS;
#endif
	}

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (!track_optimizer_enabled(queryDesc, eflags))
		return;

#if PG_VERSION_NUM < 190000
	/*
	 * Pre-19 PostgreSQL has no query_instr; allocate totaltime ourselves in
	 * the per-query context so it goes away at ExecutorEnd.
	 */
	if (queryDesc->totaltime == NULL)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
		queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
		MemoryContextSwitchTo(oldcxt);
	}
#endif
}

/*
 * Copy-paste from auto_explain code
 */
static void
_explain_statement(QueryDesc *queryDesc, double normalized_error)
{
	ExplainState   *es = NewExplainState();
	double			msec;

	if (log_min_error < 0. || normalized_error < log_min_error)
		return;
#if PG_VERSION_NUM >= 190000
	msec = INSTR_TIME_GET_MILLISEC(queryDesc->query_instr->total);
#else
	msec = queryDesc->totaltime->total * 1000.0;
#endif
	/*
	 * We are triggered by an estimation error. So, show only the options which
	 * can be useful to determine a possible solution.  Timing and buffers in
	 * the printed plan reflect what the current effort level collected.
	 */
	es->analyze = (queryDesc->instrument_options & INSTRUMENT_ROWS) != 0;
	es->verbose = false;
	es->buffers = (queryDesc->instrument_options & INSTRUMENT_BUFFERS) != 0;
	es->wal = false;
	es->timing = (queryDesc->instrument_options & INSTRUMENT_TIMER) != 0;
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
	 * NOTE: Don't afraid special symbols inside a query plan: errmsg works out
	 * this issue (see, autoexplain do the same).
	 */
	ereport(LOG,
			(errmsg("queryId: "INT64_FORMAT" duration: %.3f ms, relative error: %.4lf, plan:\n%s",
					queryDesc->plannedstmt->queryId, msec, normalized_error,
					es->str->data),
			 errhidestmt(true)));
}

static uint32
hashtable_elements_max(void)
{
	return (uint32) (hash_mem * (Size) 1024 / sizeof(DSMOptimizerTrackerEntry));
}

/*
 * Write (UBSERT/UPDATE) an entry into the HTAB.
 *
 * Returns false if memory limit was exceeded.
 */
static bool
store_data(QueryDesc *queryDesc, PlanEstimatorContext *ctx)
{
	DSMOptimizerTrackerEntry   *entry;
	DSMOptimizerTrackerKey		key;
	bool						found;
	uint32						counter;

	Assert(htab != NULL && queryDesc->plannedstmt->queryId != UINT64CONST(0));

	if (!(ctx->avg_error >= log_min_error || track_mode == TRACK_MODE_FORCED))
		return false;

	/* Guard on the number of elements */
	counter = pg_atomic_read_u32(&shared->htab_counter);
	if (counter == UINT32_MAX || counter > hashtable_elements_max())
	{
		/*
		 * Silently ignore new entries when hash table is full. Logging here
		 * would quickly overfill log files under high transaction rates
		 * (thousands per second). Users should monitor hash table capacity
		 * via pg_track_optimizer_status() instead.
		 */
		return false;
	}

	memset(&key, 0, sizeof(DSMOptimizerTrackerKey));
	key.dbOid = MyDatabaseId;
	key.queryId = queryDesc->plannedstmt->queryId;
	entry = dshash_find_or_insert(htab, &key, &found);

	if (!found)
		/*
		 * A freshly inserted entry holds uninitialized memory, so its
		 * query_ptr may look like anything.  Mark the entry incomplete with
		 * the very first store, before any operation that could fail.
		 */
		entry->query_ptr = InvalidDsaPointer;

	/*
	 * Store per-execution statistics (most recent execution only).
	 * These values are overwritten on each execution, showing only the latest
	 * query execution metrics.
	 */
	entry->evaluated_nodes = ctx->nnodes;
	entry->plan_nodes = ctx->counter;

	if (!DsaPointerIsValid(entry->query_ptr))
	{
		size_t		len = strlen(queryDesc->sourceText);
		dsa_pointer	query_ptr;
		char	   *strptr;

		/*
		 * A fresh entry, or one left behind by an initialization that failed
		 * midway: build it from scratch.  Such an entry owns nothing - the
		 * query text is published in the same store that marks the entry
		 * complete - so there is nothing to release first.
		 */
		rstats_set_empty(&entry->avg_error);
		rstats_set_empty(&entry->rms_error);
		rstats_set_empty(&entry->twa_error);
		rstats_set_empty(&entry->wca_error);
		rstats_set_empty(&entry->blks_accessed);
		rstats_set_empty(&entry->temp_blks);
		rstats_set_empty(&entry->exec_time);
		rstats_set_empty(&entry->f_join_filter);
		rstats_set_empty(&entry->f_scan_filter);
		rstats_set_empty(&entry->f_worst_splan);
		rstats_set_empty(&entry->njoins);

		entry->nexecs = 0;

		/*
		 * Allocate the query text last, so that the only failure-prone
		 * operation of this window sits right next to the store that
		 * publishes it.  dsa_allocate0() throws on out-of-shared-memory; the
		 * abort path then releases the partition lock and leaves an entry
		 * that readers skip and the next execution of this query rebuilds.
		 */
		PGTO_INJECTION_POINT("pg_track_optimizer-query-text-alloc");
		query_ptr = dsa_allocate0(htab_dsa, len + 1);
		Assert(DsaPointerIsValid(query_ptr));
		strptr = (char *) dsa_get_address(htab_dsa, query_ptr);
		strlcpy(strptr, queryDesc->sourceText, len + 1);

		/* This store completes the entry - keep it last */
		PGTO_INJECTION_POINT("pg_track_optimizer-entry-publish");
		entry->query_ptr = query_ptr;

		/*
		 * Global accounting after the entry-local state is complete: an
		 * incomplete leftover was never counted, so completing it (fresh or
		 * rebuilt) counts it exactly once.
		 */
		pg_atomic_fetch_add_u32(&shared->htab_counter, 1);
	}

	/*
	 * Accumulate cumulative statistics across executions.
	 *
	 * All error metrics (avg_error, rms_error, twa_error, wca_error) are only
	 * accumulated when non-negative. Negative values can occur legitimately when
	 * calculations produce undefined results (e.g., division by zero cost in wca_error).
	 *
	 * blks_accessed & temp_blks: Always accumulated. Block access counts are
	 * always >= 0 and represent valid physical I/O measurements for every
	 * execution.
	 */
	if (ctx->avg_error >= 0.)
		rstats_add_value(&entry->avg_error, ctx->avg_error);
	if (ctx->rms_error >= 0.)
		rstats_add_value(&entry->rms_error, ctx->rms_error);
	if (ctx->twa_error >= 0.)
		rstats_add_value(&entry->twa_error, ctx->twa_error);
	if (ctx->wca_error >= 0.)
		rstats_add_value(&entry->wca_error, ctx->wca_error);
	Assert(ctx->blks_accessed >= 0);
	rstats_add_value(&entry->blks_accessed, (double) ctx->blks_accessed);
	Assert(ctx->temp_blks >= 0);
	rstats_add_value(&entry->temp_blks, (double) ctx->temp_blks);

	/*
	 * Time-weighted factors are only computed when per-node timing was
	 * collected (effort >= timing); plan_error() reports -1 otherwise and
	 * the running statistics simply accumulate fewer samples.
	 */
	if (ctx->f_join_filter >= 0.)
		rstats_add_value(&entry->f_join_filter, ctx->f_join_filter);
	if (ctx->f_scan_filter >= 0.)
		rstats_add_value(&entry->f_scan_filter, ctx->f_scan_filter);
	if (ctx->f_worst_splan >= 0.)
		rstats_add_value(&entry->f_worst_splan, ctx->f_worst_splan);
	Assert(ctx->njoins >= 0);
	rstats_add_value(&entry->njoins, (double) ctx->njoins);

	/* Accumulate execution-level totals */
	Assert(ctx->totaltime >= 0.);
	rstats_add_value(&entry->exec_time, ctx->totaltime);
	entry->nexecs++;

	/*
	 * The in-memory state now differs from what is on disk, whether the
	 * entry was just created or merely updated.  Tracked accumulations on
	 * existing entries must reach the disk on exit just like new entries.
	 */
	pg_atomic_write_u32(&shared->need_syncing, 1);

	dshash_release_lock(htab, entry);

	return true;
}

static void
track_ExecutorEnd(QueryDesc *queryDesc)
{
	MemoryContext			oldcxt;
	double					normalized_error = -1.0;
	PlanEstimatorContext	ctx;

	track_attach_shmem();
#if PG_VERSION_NUM >= 190000
	if (!queryDesc->query_instr ||
		!(queryDesc->query_instr_options & INSTRUMENT_TIMER) ||
#else
	if (!queryDesc->totaltime ||
#endif
		!track_optimizer_enabled(queryDesc, queryDesc->estate->es_top_eflags) ||
		queryDesc->plannedstmt->queryId == 0)
		/*
		 * Just to remember: a stranger extension can decide somewhere in the
		 * middle to change queryId, eflags or another global variable. So,
		 * trust only local variables and check the state whenever possible.
		 */
		goto end;

	/* TODO: need shared state 'status' instead of assertions */
	Assert(queryDesc->planstate->instrument &&
		   queryDesc->instrument_options & INSTRUMENT_ROWS);

	/*
	 * Make sure we operate in the per-query context, so any cruft will be
	 * discarded later during ExecutorEnd.
	 */
	oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);

	/*
	 * Make sure stats accumulation is done.  (Note: it's okay if several
	 * levels of hook all do this.)
	 *
	 * On PG >= 19 the query-level Instrumentation is updated directly by
	 * core's InstrStart/InstrStop pair around ExecutorRun (InstrStop folds
	 * elapsed time straight into ->total), so there is no end-loop step
	 * here.  Pre-19 totaltime was a NodeInstrumentation and did need the
	 * end-loop call.
	 */
#if PG_VERSION_NUM < 190000
	InstrEndLoop(queryDesc->totaltime);
#endif

	/*
	 * Check that the plan was actually executed (not just a cursor declared
	 * and closed without fetching).  We check the root plan node's running
	 * flag (set by InstrStopNode during ExecutorRun) or nloops (already
	 * incremented if EXPLAIN ANALYZE called InstrEndLoop before us).
	 */
	if ((queryDesc->planstate->instrument->running ||
		queryDesc->planstate->instrument->nloops > 0) &&
#if PG_VERSION_NUM >= 190000
		!INSTR_TIME_IS_ZERO(queryDesc->query_instr->total)
#else
		queryDesc->totaltime->total > 0.0
#endif
		)
	{
		normalized_error = plan_error(queryDesc, &ctx);

		/*
		 * Store data in the hash table and/or print it to the log. Decision on what
		 * to do each routine makes individually.
		 */
		store_data(queryDesc, &ctx);
		_explain_statement(queryDesc, normalized_error);
	}

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

	DefineCustomEnumVariable("pg_track_optimizer.mode",
							 "Extension operation mode",
							 NULL,
							 &track_mode,
							 TRACK_MODE_DISABLED,
							 format_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("pg_track_optimizer.effort",
							 "Instrumentation effort spent on tracked queries",
							 "rows: row counters only - cheapest, estimation-error"
							 " metrics still collected; timing: adds per-node timing"
							 " and the time-weighted metrics (default); full: adds"
							 " per-node buffer accounting to the logged EXPLAIN",
							 &track_effort,
							 TRACK_EFFORT_TIMING,
							 effort_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomRealVariable("pg_track_optimizer.log_min_error",
							 "Sets the minimum planning error above which plans will be logged",
							 "Zero prints all plans; -1 turns this feature off",
							 &log_min_error,
							 -1.0,
							 -1.0, INT_MAX, /* Looks like such a huge error, as INT_MAX, doesn't make sense */
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("pg_track_optimizer.hash_mem",
							"Maximum size of DSM memory for the hash table",
							NULL,
							&hash_mem,
							4096,
							0, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_KB,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("pg_track_optimizer.auto_flush",
							 "Automatically flush statistics to disk on backend shutdown",
							 NULL,
							 &auto_flush,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_track_optimizer");

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = explain_ExecutorStart;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = track_ExecutorEnd;

	/*
	 * NB: the on-exit flush callback is registered per backend in
	 * track_attach_shmem(), not here - _PG_init() may be running in the
	 * postmaster, whose exit-callback list does not propagate to children.
	 */
}

/* -----------------------------------------------------------------------------
 *
 * UI routines
 *
 */

PG_FUNCTION_INFO_V1(pg_track_optimizer);
PG_FUNCTION_INFO_V1(pg_track_optimizer_status);
PG_FUNCTION_INFO_V1(to_reset);

/*
 * Return the current status of the extension.
 */
Datum
pg_track_optimizer_status(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3];
	HeapTuple	tuple;
	const char *mode_str;
	uint32		entries_count;
	uint32		entries_max;
	bool		is_synced;

	track_attach_shmem();

	/* Get current mode as string */
	switch (track_mode)
	{
		case TRACK_MODE_NORMAL:
			mode_str = "normal";
			break;
		case TRACK_MODE_FORCED:
			mode_str = "forced";
			break;
		case TRACK_MODE_DISABLED:
			mode_str = "disabled";
			break;
		default:
			elog(ERROR, "unexpected mode value %d", track_mode);
			break;
	}

	/* Get entry counts */
	entries_count = pg_atomic_read_u32(&shared->htab_counter);
	entries_max = hashtable_elements_max();

	/* Check sync status */
	is_synced = (pg_atomic_read_u32(&shared->need_syncing) == 0);

	/* Build tuple descriptor */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context that cannot accept type record")));

	memset(nulls, 0, sizeof(nulls));
	values[0] = CStringGetTextDatum(mode_str);
	values[1] = UInt32GetDatum(entries_max - entries_count);
	values[2] = BoolGetDatum(is_synced);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * Return all tracked query statistics for the current database.
 *
 * This exposes query texts of all users and databases, so EXECUTE is revoked
 * from PUBLIC in the install script; the DBA delegates access with GRANT.
 * There are no per-row visibility checks - whoever is granted access sees
 * everything - which keeps the extension lightweight and simple.
 */
Datum
pg_track_optimizer(PG_FUNCTION_ARGS)
{
	ReturnSetInfo			   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum						values[DATATBL_NCOLS];
	bool						nulls[DATATBL_NCOLS];
	dshash_seq_status			stat;
	DSMOptimizerTrackerEntry   *entry;

	track_attach_shmem();

	InitMaterializedSRF(fcinfo, 0);

	dshash_seq_init(&stat, htab, false);
	while ((entry = dshash_seq_next(&stat)) != NULL)
	{
		int		i = 0;
		char   *str;

		CHECK_FOR_INTERRUPTS();

		/* Skip entries whose initialization did not complete */
		if (!DsaPointerIsValid(entry->query_ptr))
			continue;

		Assert(entry->key.queryId != UINT64CONST(0) &&
			   OidIsValid(entry->key.dbOid));

		memset(nulls, 0, DATATBL_NCOLS);
		values[i++] = ObjectIdGetDatum(entry->key.dbOid);
		values[i++] = Int64GetDatum(entry->key.queryId);

		/* Query string (validity established by the skip above) */
		str = (char *) dsa_get_address(htab_dsa, entry->query_ptr);
		values[i++] = CStringGetTextDatum(str);

		/* Fill-in cumulative statistics fields */
		values[i++] = RStatsPGetDatum(&entry->avg_error);
		values[i++] = RStatsPGetDatum(&entry->rms_error);
		values[i++] = RStatsPGetDatum(&entry->twa_error);
		values[i++] = RStatsPGetDatum(&entry->wca_error);
		values[i++] = RStatsPGetDatum(&entry->blks_accessed);
		values[i++] = RStatsPGetDatum(&entry->temp_blks);
		values[i++] = RStatsPGetDatum(&entry->exec_time);
		values[i++] = RStatsPGetDatum(&entry->f_join_filter);
		values[i++] = RStatsPGetDatum(&entry->f_scan_filter);
		values[i++] = RStatsPGetDatum(&entry->f_worst_splan);
		values[i++] = RStatsPGetDatum(&entry->njoins);

		values[i++] = Int32GetDatum(entry->evaluated_nodes);
		values[i++] = Int32GetDatum(entry->plan_nodes);
		values[i++] = Int64GetDatum(entry->nexecs);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		Assert(i == DATATBL_NCOLS);
	}
	dshash_seq_term(&stat);

	return (Datum) 0;
}

/*
 * Reset the state of this extension to default. This will clean up all additionally
 * allocated resources and reset static and global state variables.
 */
static uint32
reset_htab(void)
{
	dshash_seq_status			stat;
	DSMOptimizerTrackerEntry   *entry;
	uint32						counter = 0;

	track_attach_shmem();

	/*
	 * Destroying shared hash table is a bit dangerous procedure. Without full
	 * understanding of the dshash_destroy() technique, delete elements more
	 * simply, one by one.
	 */
	dshash_seq_init(&stat, htab, true);
	while ((entry = dshash_seq_next(&stat)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * An entry whose initialization did not complete owns no query text
		 * and was never added to htab_counter: just drop it.
		 */
		if (DsaPointerIsValid(entry->query_ptr))
		{
			Assert(entry->key.queryId != UINT64CONST(0) &&
				   OidIsValid(entry->key.dbOid));

			/* At first, free memory, allocated for the query text */
			dsa_free(htab_dsa, entry->query_ptr);
			pg_atomic_fetch_sub_u32(&shared->htab_counter, 1);

			/*
			 * htab_counter may be changes simultaneously. So, calculate how
			 * much entries we removed using a local variable.
			 */
			counter++;
		}

		dshash_delete_current(&stat);
	}
	dshash_seq_term(&stat);

	if (counter == 0)
		PG_RETURN_UINT32(0);

	/*
	 * Flush final state of the HTAB to the disk. There are some records might
	 * be added simultaneously. Lock is needed to prevent parallel file
	 * operations.
	 */
	LWLockAcquire(&shared->lock, LW_EXCLUSIVE);

	/*
	 * Commands, attaching file must take exclusive lock beforehand and can't
	 * concur here. HTAB writer must change disk_synced after the actual write.
	 * So, the only flaw is that later we do an extra flush of the same table.
	 * I think it is good trade-off with code complexity.
	 */
	pg_atomic_write_u32(&shared->need_syncing, 0);
	(void) _flush_hash_table();

	LWLockRelease(&shared->lock);

	/*
	 * Let user know how much records we actually removed (hash table might be
	 * filled in parallel)
	 */
	return counter;
}

/*
 * No hardcoded superuser check here: EXECUTE is revoked from PUBLIC in the
 * install script, and the DBA may delegate the privilege with GRANT.
 */
Datum
to_reset(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT32(reset_htab());
}

/* -----------------------------------------------------------------------------
 *
 * Disk operations
 *
 * -------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(to_flush);

static const uint32 DATA_FILE_HEADER	= 12354678;
static const uint32 DATA_FORMAT_VERSION = 20260118; /* EOF marker entry instead of upfront count */
static const char *DATA_PG_VERSION_STR = PG_VERSION_STR;

#define EXTENSION_NAME "pg_track_optimizer"
static const char *filename = PG_STAT_TMP_DIR "/" EXTENSION_NAME ".stat";

/*
 * IMPLEMENTATION NOTES:
 * dump/restore statistics is an optional procedure that is executed in
 * an infrequent and non-concurrent mode. Also, it is not any critical for
 * production - it should be executed in testing environment, on replica or
 * in a maintenance window. So, for the sake of laconic and clear code, use
 * simplistic coding approach with a single fsync if the flush operation has
 * been done successfully.
 *
 * NOTE: query execution statistics inherently platform-dependent. So, skip
 * reading the data file if PG_VERSION_STR has been changed.
 */

/*
 * Specifics of storing the dshash table:
 * We don't block the table entirely, so we don't know how many records
 * will be eventually stored. We write an EOF marker entry (with queryId=0
 * and dbOid=InvalidOid) after all records, followed by the actual count.
 * Returns the number of records written.
 */
static uint32
_flush_hash_table(void)
{
	dshash_seq_status			stat;
	DSMOptimizerTrackerEntry   *entry;
	DSMOptimizerTrackerEntry	eof_entry;
	const char				   *tmpfile = PG_STAT_TMP_DIR "/" EXTENSION_NAME".tmp";
	File						file = -1;
	ssize_t						written;
	uint32						counter = 0;
	const uint32				verstr_len = strlen(DATA_PG_VERSION_STR);
	off_t						filepos = 0;
	pg_crc32c					crc;
	int							save_errno;

	file = PathNameOpenFile(tmpfile, O_CREAT | O_WRONLY | O_TRUNC | PG_BINARY);
	if (file < 0)
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("[%s] could not open file \"%s\" for writing: %m",
			 EXTENSION_NAME, tmpfile)));

	/* Initialize CRC32C checksum computation */
	INIT_CRC32C(crc);

	/* Add a header to the file for more reliable identification of the data */
	written = FileWrite(file, &DATA_FILE_HEADER, sizeof(uint32), filepos,
						WAIT_EVENT_DATA_FILE_WRITE);
	if (written != sizeof(uint32))
		goto error;
	COMP_CRC32C(crc, &DATA_FILE_HEADER, sizeof(uint32));
	filepos += written;

	written = FileWrite(file, &DATA_FORMAT_VERSION, sizeof(uint32), filepos,
						WAIT_EVENT_DATA_FILE_WRITE);
	if (written != sizeof(uint32))
		goto error;
	COMP_CRC32C(crc, &DATA_FORMAT_VERSION, sizeof(uint32));
	filepos += written;

	written = FileWrite(file, &verstr_len, sizeof(uint32), filepos,
						WAIT_EVENT_DATA_FILE_WRITE);
	if (written != sizeof(uint32))
		goto error;
	COMP_CRC32C(crc, &verstr_len, sizeof(uint32));
	filepos += written;

	written = FileWrite(file, DATA_PG_VERSION_STR, verstr_len, filepos,
						WAIT_EVENT_DATA_FILE_WRITE);
	if (written != verstr_len)
		goto error;
	COMP_CRC32C(crc, DATA_PG_VERSION_STR, verstr_len);
	filepos += written;

	dshash_seq_init(&stat, htab, false);
	while ((entry = dshash_seq_next(&stat)) != NULL)
	{
		char   *str;
		uint32	len;

		CHECK_FOR_INTERRUPTS();

		/* Never persist entries whose initialization did not complete */
		if (!DsaPointerIsValid(entry->query_ptr))
			continue;

		Assert(entry->key.queryId != UINT64CONST(0) &&
			   OidIsValid(entry->key.dbOid));

		str = (char *) dsa_get_address(htab_dsa, entry->query_ptr);
		len = strlen(str);

		/*
		 * Write data into the file. It is more or less stable procedure:
		 * We declare this extension has no support of dump/restore on different
		 * hardware/OS platforms. So, it is safe.
		 */
		written = FileWrite(file, entry, sizeof(DSMOptimizerTrackerEntry),
							filepos, WAIT_EVENT_DATA_FILE_WRITE);
		if (written != sizeof(DSMOptimizerTrackerEntry))
			goto error;
		COMP_CRC32C(crc, entry, sizeof(DSMOptimizerTrackerEntry));
		filepos += written;

		written = FileWrite(file, &len, sizeof(uint32), filepos,
							WAIT_EVENT_DATA_FILE_WRITE);
		if (written != sizeof(uint32))
			goto error;
		COMP_CRC32C(crc, &len, sizeof(uint32));
		filepos += written;

		written = FileWrite(file, str, len, filepos,
							WAIT_EVENT_DATA_FILE_WRITE);
		if (written != len)
			goto error;
		COMP_CRC32C(crc, str, len);
		filepos += written;

		counter++;
	}
	dshash_seq_term(&stat);

	/*
	 * Write EOF marker entry: an entry with queryId=0 and dbOid=InvalidOid
	 * signals end of records. This allows the reader to detect end of data
	 * without knowing the record count upfront.
	 */
	memset(&eof_entry, 0, sizeof(DSMOptimizerTrackerEntry));

	written = FileWrite(file, &eof_entry, sizeof(DSMOptimizerTrackerEntry),
						filepos, WAIT_EVENT_DATA_FILE_WRITE);
	if (written != sizeof(DSMOptimizerTrackerEntry))
		goto error;
	COMP_CRC32C(crc, &eof_entry, sizeof(DSMOptimizerTrackerEntry));
	filepos += written;

	/* Write the actual record count after EOF marker for verification */
	written = FileWrite(file, &counter, sizeof(uint32), filepos,
						WAIT_EVENT_DATA_FILE_WRITE);
	if (written != sizeof(uint32))
		goto error;
	COMP_CRC32C(crc, &counter, sizeof(uint32));
	filepos += written;

	/* Finalize CRC32C computation and write it to the file */
	FIN_CRC32C(crc);
	written = FileWrite(file, &crc, sizeof(pg_crc32c), filepos,
						WAIT_EVENT_DATA_FILE_WRITE);
	if (written != sizeof(pg_crc32c))
		goto error;
	filepos += written;

	/*
	 * Sync the file to disk before making it visible via rename.
	 * This ensures crash safety - data must be durable before directory update.
	 */
	if (FileSync(file, WAIT_EVENT_DATA_FILE_SYNC) != 0)
		goto error;

	(void) durable_rename(tmpfile, filename, LOG);
	elog(LOG, "[%s] %u records stored in file \"%s\"",
		 EXTENSION_NAME, counter, filename);
	return counter;

	/*
	 * Before throwing an error we should remove (potentially) inconsistent
	 * temporary file.  Save errno first: FileClose()/unlink() may clobber it
	 * and %m below must report the original write failure.
	 */
error:
	save_errno = errno;

	FileClose(file);
	unlink(tmpfile);

	errno = save_errno;
	ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("[%s] could not write file \"%s\": %m",
			 EXTENSION_NAME, tmpfile)));

	/* Keep compiler quiet */
	return -1;
}

static void
recreate_htab(TODSMRegistry *state)
{
	if (htab)
		dshash_destroy(htab);
	if (htab_dsa)
	{
		dsa_unpin(htab_dsa);
		dsa_detach(htab_dsa);
	}

	htab_dsa = dsa_create(state->tranche_id);
	dsa_pin(htab_dsa);
	htab = dshash_create(htab_dsa, &dsh_params, 0);
	state->dsah = dsa_get_handle(htab_dsa);
	state->dshh = dshash_get_hash_table_handle(htab);
	pg_atomic_init_u32(&state->htab_counter, 0);
	pg_atomic_init_u32(&state->need_syncing, 0);
}

/*
 * Read data file record by record and add each record into the new table.
 * Provide reference to the shared area because the local pointer is still not
 * initialized.
 * NOTE: Must be executed in a safe state where no concurrency is present. Right
 * now it is executed under the internal DSM lock. Identify it by checking that
 * the 'shared' variable is NULL.
 * TODO: we may add 'reload' option if user wants to fix a problem.
 *
 * Records are read until an EOF marker entry is encountered (queryId=0 and
 * dbOid=InvalidOid). After the EOF marker, a stored record count is read
 * for verification.
 *
 * IMPORTANT: this runs inside the DSM initialization callback, i.e. during
 * the ExecutorStart hook of whatever user query happens to touch the
 * extension first after a restart.  The statistics file is an optional
 * cache: no defect in it - corruption, truncation, version mismatch - may
 * ever abort that innocent query.  Every failure path below reports the
 * reason at WARNING, throws away whatever fraction was loaded and continues
 * with an empty hash table.
 */
static uint32
_load_hash_table(TODSMRegistry *state)
{
	File						file;
	ssize_t						nbytes;
	uint32						header;
	int32						fmtver;
	char					   *ver_str;
	uint32						verstr_len;
	DSMOptimizerTrackerEntry	disk_entry;
	DSMOptimizerTrackerEntry   *entry;
	uint32						stored_nrecs;
	uint32						counter = 0;
	off_t						filepos = 0;
	pg_crc32c					crc;
	pg_crc32c					stored_crc;

	if (shared != NULL)
	{
		elog(WARNING,
			 "[%s] unexpected state of shared memory; data not loaded",
			 EXTENSION_NAME);
		return -1;
	}

	/* Must load data into an empty hash table */
	if (pg_atomic_read_u32(&state->htab_counter) != 0)
	{
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("[%s] hash table is unexpectedly not empty; data not loaded",
						EXTENSION_NAME),
				 errhint("Reset the statistics to clean up the hash table.")));
		return -1;
	}

	file = PathNameOpenFile(filename, O_RDONLY | PG_BINARY);
	if (file < 0)
	{
		if (errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("[%s] could not open file \"%s\": %m",
							EXTENSION_NAME, filename)));
		/* Nothing to load */
		return -1;
	}

	/* Initialize CRC32C checksum computation */
	INIT_CRC32C(crc);

	nbytes = FileRead(file, &header, sizeof(uint32), filepos,
					  WAIT_EVENT_DATA_FILE_READ);
	if (nbytes != sizeof(uint32))
		goto read_error;
	if (header != DATA_FILE_HEADER)
		goto data_header_error;
	COMP_CRC32C(crc, &header, sizeof(uint32));
	filepos += sizeof(uint32);

	nbytes = FileRead(file, &fmtver, sizeof(uint32), filepos,
					  WAIT_EVENT_DATA_FILE_READ);
	if (nbytes != sizeof(uint32))
		goto read_error;
	if (fmtver != DATA_FORMAT_VERSION)
		goto data_version_error;
	COMP_CRC32C(crc, &fmtver, sizeof(uint32));
	filepos += nbytes;

	nbytes = FileRead(file, &verstr_len, sizeof(uint32), filepos,
					  WAIT_EVENT_DATA_FILE_READ);
	if (nbytes != sizeof(uint32))
		goto read_error;

	/*
	 * Length fields come from an unverified file (the CRC is only checked at
	 * the very end), so bound them before using them for allocations: a
	 * corrupted length must not drive a multi-gigabyte palloc.
	 */
	if (verstr_len == 0 || verstr_len > 1024)
		goto length_error;
	COMP_CRC32C(crc, &verstr_len, sizeof(uint32));
	filepos += nbytes;

	ver_str = palloc0(verstr_len + 1);

	nbytes = FileRead(file, ver_str, verstr_len, filepos,
					  WAIT_EVENT_DATA_FILE_READ);
	if (nbytes != verstr_len)
		goto read_error;
	if (strcmp(ver_str, DATA_PG_VERSION_STR) != 0)
	{
		ereport(WARNING,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("[%s] file \"%s\" has been written on different platform",
			 EXTENSION_NAME, filename),
			 errdetail("skip data file load for safety"),
			 errhint("remove the file manually or reset statistics in advance")));

		/* No entries yet added to HTAB. simple exit path */
		pfree(ver_str);
		goto end;
	}
	COMP_CRC32C(crc, ver_str, verstr_len);
	pfree(ver_str);
	filepos += nbytes;

	/*
	 * Read records until we encounter the EOF marker entry (queryId=0 and
	 * dbOid=InvalidOid).
	 */
	for (;;)
	{
		char   *str;
		uint32	len;
		bool	found;

		/* Read the entry header */
		nbytes = FileRead(file, &disk_entry, sizeof(DSMOptimizerTrackerEntry),
						  filepos, WAIT_EVENT_DATA_FILE_READ);
		if (nbytes != sizeof(DSMOptimizerTrackerEntry))
			goto read_error;
		COMP_CRC32C(crc, &disk_entry, sizeof(DSMOptimizerTrackerEntry));
		filepos += nbytes;

		/* Check for EOF marker: queryId=0 and dbOid=InvalidOid */
		if (disk_entry.key.queryId == UINT64CONST(0) &&
			!OidIsValid(disk_entry.key.dbOid))
			break;

		/* Validate entry has valid key */
		if (disk_entry.key.queryId == UINT64CONST(0) ||
			!OidIsValid(disk_entry.key.dbOid))
			goto data_corrupted_error;

		/* Check if we're exceeding hash table capacity */
		if (counter >= (uint32) hashtable_elements_max())
		{
			ereport(WARNING,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("[%s] file \"%s\" contains more records than hash table may consume (%d)",
				 EXTENSION_NAME, filename, hashtable_elements_max()),
				 errdetail("skip data file load for safety"),
				 errhint("remove the file manually or reset statistics in advance")));
			goto soft_failed_end;
		}

		/* Load query string */
		nbytes = FileRead(file, &len, sizeof(uint32), filepos,
						  WAIT_EVENT_DATA_FILE_READ);
		if (nbytes != sizeof(uint32))
			goto read_error;
		/* Bound the length before allocating: see verstr_len above */
		if (len >= MaxAllocSize)
			goto length_error;
		COMP_CRC32C(crc, &len, sizeof(uint32));
		filepos += nbytes;

		/*
		 * No-OOM allocation: an out-of-memory condition while loading an
		 * optional cache should degrade to an empty table, not abort the
		 * user query we are riding on.
		 */
		disk_entry.query_ptr = dsa_allocate_extended(htab_dsa, len + 1,
													 DSA_ALLOC_NO_OOM | DSA_ALLOC_ZERO);
		if (!DsaPointerIsValid(disk_entry.query_ptr))
		{
			ereport(WARNING,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("[%s] out of shared memory while loading file \"%s\"",
				 EXTENSION_NAME, filename),
				 errdetail("skip data file load for safety")));
			goto soft_failed_end;
		}
		str = (char *) dsa_get_address(htab_dsa, disk_entry.query_ptr);
		nbytes = FileRead(file, str, len, filepos, WAIT_EVENT_DATA_FILE_READ);
		if (nbytes != len)
			goto read_error;
		COMP_CRC32C(crc, str, len);
		filepos += nbytes;

		entry = dshash_find_or_insert(htab, &disk_entry.key, &found);
		if (found)
		{
			/* Release the lock before recreate_htab() destroys the table */
			dshash_release_lock(htab, entry);
			ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("[%s] file \"%s\" has duplicate record with dbOid %u and queryId "UINT64_FORMAT,
				 EXTENSION_NAME, filename, disk_entry.key.dbOid, disk_entry.key.queryId)));
			goto soft_failed_end;
		}

		/*
		 * TODO: copy all data in one operation. At least we will not do
		 * annoying copy DSM pointer.
		 */
		entry->evaluated_nodes = disk_entry.evaluated_nodes;
		entry->plan_nodes = disk_entry.plan_nodes;
		memcpy(&entry->avg_error, &disk_entry.avg_error, sizeof(RStats));
		memcpy(&entry->rms_error, &disk_entry.rms_error, sizeof(RStats));
		memcpy(&entry->twa_error, &disk_entry.twa_error, sizeof(RStats));
		memcpy(&entry->wca_error, &disk_entry.wca_error, sizeof(RStats));
		memcpy(&entry->blks_accessed, &disk_entry.blks_accessed, sizeof(RStats));
		memcpy(&entry->temp_blks, &disk_entry.temp_blks, sizeof(RStats));
		memcpy(&entry->exec_time, &disk_entry.exec_time, sizeof(RStats));
		memcpy(&entry->f_join_filter, &disk_entry.f_join_filter, sizeof(RStats));
		memcpy(&entry->f_scan_filter, &disk_entry.f_scan_filter, sizeof(RStats));
		memcpy(&entry->f_worst_splan, &disk_entry.f_worst_splan, sizeof(RStats));
		memcpy(&entry->njoins, &disk_entry.njoins, sizeof(RStats));

		entry->nexecs = disk_entry.nexecs;

		/* Publishes the entry, exactly as in store_data(): keep it last */
		entry->query_ptr = disk_entry.query_ptr;

		dshash_release_lock(htab, entry);
		counter++;
	}

	/* Read stored record count for verification */
	nbytes = FileRead(file, &stored_nrecs, sizeof(uint32), filepos,
					  WAIT_EVENT_DATA_FILE_READ);
	if (nbytes != sizeof(uint32))
		goto read_error;
	COMP_CRC32C(crc, &stored_nrecs, sizeof(uint32));
	filepos += nbytes;

	/* Verify record count matches */
	if (counter != stored_nrecs)
	{
		ereport(WARNING,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" record count mismatch",
			 EXTENSION_NAME, filename),
			 errdetail("Read %u records, but file claims %u", counter, stored_nrecs),
			 errhint("File may be corrupted")));
		goto soft_failed_end;
	}

	/*
	 * Finalize CRC computation and verify against stored checksum.
	 * This detects any corruption from disk errors, partial writes, or bit flips.
	 */
	FIN_CRC32C(crc);

	nbytes = FileRead(file, &stored_crc, sizeof(pg_crc32c), filepos,
					  WAIT_EVENT_DATA_FILE_READ);
	if (nbytes != sizeof(pg_crc32c))
		goto crc_read_error;
	filepos += nbytes;

	if (!EQ_CRC32C(crc, stored_crc))
	{
		ereport(WARNING,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" has incorrect CRC32C checksum",
			 EXTENSION_NAME, filename),
			 errdetail("Expected %08X, found %08X", stored_crc, crc),
			 errhint("File is corrupted - skipping load for safety")));
		goto soft_failed_end;
	}

	/* Verify we're at EOF - no extra data after the checksum. */
	nbytes = FileRead(file, &disk_entry, 1, filepos, WAIT_EVENT_DATA_FILE_READ);
	if (nbytes == 1)
	{
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("[%s] file \"%s\" contains more data than expected",
				 EXTENSION_NAME, filename)));
		goto soft_failed_end;
	}

	FileClose(file);
	pg_atomic_write_u32(&state->htab_counter, counter);
	elog(LOG, "[%s] %u records loaded from file \"%s\"",
		 EXTENSION_NAME, counter, filename);
	return counter;

/*
 * Failure exits.  Everything lands at WARNING: an unusable statistics file
 * must degrade to an empty hash table, never abort the user query whose
 * ExecutorStart we are running in.  Paths that may have inserted entries
 * already go through soft_failed_end to rebuild the table from scratch.
 */
read_error:
	ereport(WARNING,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] could not read file \"%s\": it is possibly truncated",
			 EXTENSION_NAME, filename),
			 errdetail("skip data file load for safety"),
			 errhint("remove the file manually or reset statistics in advance")));
	goto soft_failed_end;
data_header_error:
	ereport(WARNING,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" has incompatible header version %d instead of %d",
			 EXTENSION_NAME, filename, header, DATA_FILE_HEADER),
			 errdetail("skip data file load for safety"),
			 errhint("remove the file manually or reset statistics in advance")));
	goto soft_failed_end;
data_version_error:
	ereport(WARNING,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" has incompatible data format version %d instead of %d",
			 EXTENSION_NAME, filename, fmtver, DATA_FORMAT_VERSION),
			 errdetail("skip data file load for safety"),
			 errhint("remove the file manually or reset statistics in advance")));
	goto soft_failed_end;
data_corrupted_error:
	ereport(WARNING,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" contains invalid entry with queryId "UINT64_FORMAT" and dbOid %u",
			 EXTENSION_NAME, filename, disk_entry.key.queryId, disk_entry.key.dbOid),
			 errhint("File may be corrupted")));
	goto soft_failed_end;
length_error:
	ereport(WARNING,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" contains an implausible string length",
			 EXTENSION_NAME, filename),
			 errdetail("skip data file load for safety"),
			 errhint("remove the file manually or reset statistics in advance")));
	goto soft_failed_end;
crc_read_error:
	ereport(WARNING,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" is missing CRC32C checksum",
			 EXTENSION_NAME, filename),
			 errdetail("File may be truncated or corrupted"),
			 errhint("Remove the file manually or reset statistics in advance")));
	/* FALLTHROUGH */
soft_failed_end:
	recreate_htab(state);
end:
	if (file >= 0)
		FileClose(file);

	return -1;
}

/*
 * Backstop wrapper around _load_hash_table().
 *
 * All anticipated failure modes are handled softly inside _load_hash_table()
 * itself (WARNING + empty table), so ordinarily nothing is thrown here.  If
 * something genuinely unexpected escapes anyway, make sure the half-loaded
 * hash table is rebuilt before the error propagates, so that later attaches
 * find a consistent (empty) state.
 */
static uint32
_load_hash_table_safe(TODSMRegistry *state)
{
	volatile uint32 nrecs = -1;
	PG_TRY();
	{
		nrecs = _load_hash_table(state);
	}
	PG_CATCH();
	{
		recreate_htab(state);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return nrecs;
}

/*
 * No hardcoded superuser check here: EXECUTE is revoked from PUBLIC in the
 * install script, and the DBA may delegate the privilege with GRANT.
 */
Datum
to_flush(PG_FUNCTION_ARGS)
{
	uint32 counter;

	track_attach_shmem();

	LWLockAcquire(&shared->lock, LW_EXCLUSIVE);
	pg_atomic_write_u32(&shared->need_syncing, 0);
	counter = _flush_hash_table();
	LWLockRelease(&shared->lock);

	PG_RETURN_UINT32(counter);
}

static void
pto_before_shmem_exit(int code, Datum arg)
{
	MemoryContext	oldcontext = CurrentMemoryContext;
	volatile bool	success = false;

	/*
	 * Flush only on the clean exit of a regular backend.  Parallel workers
	 * attach to the hash table too, but flushing the whole file on every
	 * worker exit would be pure overhead - their leader's exit covers it.
	 */
	if (!IsUnderPostmaster || IsParallelWorker() || code != 0 ||
		htab == NULL || !auto_flush)
		return;

	/* On the backend shutdown flush the data only if something new arrived */
	if (pg_atomic_read_u32(&shared->need_syncing) == 0)
		return;

	elog(DEBUG1, "[%s] saving hash table to the disk", EXTENSION_NAME);

	/*
	 * If another backend is flushing right now, skip our flush instead of
	 * queueing for the lock.  Under connection churn, exiting backends
	 * otherwise convoy behind this exclusive lock while each rewrites the
	 * whole file with an fsync; the half-exited backends keep their PGPROC
	 * slots, pile up, and new connections start failing with "sorry, too
	 * many clients already".  Leaving need_syncing set delegates the write
	 * to the next exiting backend (or an explicit flush): whatever the
	 * current lock holder's table scan has already passed will be picked up
	 * by that later flush.
	 */
	if (!LWLockConditionalAcquire(&shared->lock, LW_EXCLUSIVE))
	{
		elog(DEBUG1, "[%s] skipping on-exit flush: another flush is in progress",
			 EXTENSION_NAME);
		return;
	}

	/* On backend shutdown be more careful and ignore errors */
	PG_TRY();
	{
		/*
		 * Write need_syncing in advance. That means - no second attempt after
		 * failed write will be done. Is it voluntary trade-off.
		 */
		pg_atomic_write_u32(&shared->need_syncing, 0);
		_flush_hash_table();
		success = true;
	}
	PG_FINALLY();
	{
		LWLockRelease(&shared->lock);

		if (!success)
		{
			/* Just ignore the error */
			MemoryContextSwitchTo(oldcontext);
			FlushErrorState();
			elog(LOG, "[%s] On-shutdown statistic flush has been unsuccessful",
				 EXTENSION_NAME);
		}
	}
	PG_END_TRY();
}
