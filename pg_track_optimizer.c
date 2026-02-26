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

#include "plan_error.h"
#include "rstats.h"

#if (PG_VERSION_NUM < 180000)
PG_MODULE_MAGIC;
#else
PG_MODULE_MAGIC_EXT(
					.name = "pg_track_optimizer",
					.version = "0.9.1"
);
#endif

#define DATATBL_NCOLS	(17)

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
	RStats					local_blks;			/* Local blocks (read + written + dirtied) - work_mem indicator */
	RStats					exec_time;			/* Execution time per query - running stats (milliseconds) */
	RStats					f_join_filter;	/* Maximum filtered rows (nfiltered1+nfiltered2) across JOIN nodes */
	RStats					f_scan_filter;	/* Maximum nfiltered1 for leaf nodes in the query plan */
	RStats					f_worst_splan;	/* Worst SubPlan factor: (nloops/log(nloops+1)) * (time/total_time) */
	RStats					njoins;			/* Number of JOIN nodes per execution - running stats */
	int64					nexecs;				/* Number of executions tracked */

	/* Metadata */
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

static int track_mode = TRACK_MODE_DISABLED;
static double log_min_error = -1.0;
static int hash_mem = 4096;
static bool auto_flush = true;

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
}

/*
 * Here we need it to enable instrumentation.
 */
static void
explain_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	track_attach_shmem();

	if (track_optimizer_enabled(queryDesc, eflags))
		queryDesc->instrument_options |= INSTRUMENT_TIMER | INSTRUMENT_ROWS | INSTRUMENT_BUFFERS;

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (!track_optimizer_enabled(queryDesc, eflags))
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

	if (log_min_error < 0. || normalized_error < log_min_error)
		return;
#if PG_VERSION_NUM >= 190000
	msec = INSTR_TIME_GET_MILLISEC(queryDesc->totaltime->total);
#else
	msec = queryDesc->totaltime->total * 1000.0;
#endif
	/*
	 * We are triggered by an estimation error. So, show only the options which
	 * can be useful to determine a possible solution.
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
hashtable_elements_max()
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

	/*
	 * Store per-execution statistics (most recent execution only).
	 * These values are overwritten on each execution, showing only the latest
	 * query execution metrics.
	 */
	entry->evaluated_nodes = ctx->nnodes;
	entry->plan_nodes = ctx->counter;

	if (!found)
	{
		size_t	len = strlen(queryDesc->sourceText);
		char   *strptr;

		/* Allocate and store the query string in shared memory */
		entry->query_ptr = dsa_allocate0(htab_dsa, len + 1);
		Assert(DsaPointerIsValid(entry->query_ptr));
		strptr = (char *) dsa_get_address(htab_dsa, entry->query_ptr);
		strlcpy(strptr, queryDesc->sourceText, len + 1);

		/*
		 * Initialize cumulative statistics fields to empty state.
		 * These will be populated incrementally as values are added via
		 * rstats_add_value(). The empty state uses sentinel values (-1)
		 * to indicate no data has been accumulated yet.
		 */
		rstats_set_empty(&entry->avg_error);
		rstats_set_empty(&entry->rms_error);
		rstats_set_empty(&entry->twa_error);
		rstats_set_empty(&entry->wca_error);
		rstats_set_empty(&entry->blks_accessed);
		rstats_set_empty(&entry->local_blks);
		rstats_set_empty(&entry->exec_time);
		rstats_set_empty(&entry->f_join_filter);
		rstats_set_empty(&entry->f_scan_filter);
		rstats_set_empty(&entry->f_worst_splan);
		rstats_set_empty(&entry->njoins);

		entry->nexecs = 0;

		pg_atomic_fetch_add_u32(&shared->htab_counter, 1);
		pg_atomic_write_u32(&shared->need_syncing, 1); /* New data arrived */
	}

	/*
	 * Accumulate cumulative statistics across executions.
	 *
	 * All error metrics (avg_error, rms_error, twa_error, wca_error) are only
	 * accumulated when non-negative. Negative values can occur legitimately when
	 * calculations produce undefined results (e.g., division by zero cost in wca_error).
	 *
	 * blks_accessed & local_blks: Always accumulated. Block access counts are
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
	Assert(ctx->local_blks >= 0);
	rstats_add_value(&entry->local_blks, (double) ctx->local_blks);
	Assert(ctx->f_join_filter >= 0.);
	rstats_add_value(&entry->f_join_filter, ctx->f_join_filter);
	Assert(ctx->f_scan_filter >= 0.);
	rstats_add_value(&entry->f_scan_filter, ctx->f_scan_filter);
	Assert(ctx->f_worst_splan >= 0.);
	rstats_add_value(&entry->f_worst_splan, ctx->f_worst_splan);
	Assert(ctx->njoins >= 0);
	rstats_add_value(&entry->njoins, (double) ctx->njoins);

	/* Accumulate execution-level totals */
	Assert(ctx->totaltime >= 0.);
	rstats_add_value(&entry->exec_time, ctx->totaltime);
	entry->nexecs++;

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

	if (!queryDesc->totaltime ||
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

	/*
	 * Check that the plan was actually executed (not just a cursor declared
	 * and closed without fetching).  We check the root plan node's running
	 * flag (set by InstrStopNode during ExecutorRun) or nloops (already
	 * incremented if EXPLAIN ANALYZE called InstrEndLoop before us).
	 */
	if ((queryDesc->planstate->instrument->running ||
		queryDesc->planstate->instrument->nloops > 0) &&
#if PG_VERSION_NUM >= 190000
		!INSTR_TIME_IS_ZERO(queryDesc->totaltime->total)
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

	before_shmem_exit(pto_before_shmem_exit, (Datum) 0);
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
 * Access control for this view is intentionally left to the DBA (e.g., via
 * GRANT/REVOKE on the function or view). This keeps the extension lightweight
 * and simple, avoiding the overhead of per-row visibility checks.
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

		Assert(entry->key.queryId != UINT64CONST(0) &&
			   OidIsValid(entry->key.dbOid));

		CHECK_FOR_INTERRUPTS();

		memset(nulls, 0, DATATBL_NCOLS);
		values[i++] = ObjectIdGetDatum(entry->key.dbOid);
		values[i++] = Int64GetDatum(entry->key.queryId);

		/* Query string */
		Assert(DsaPointerIsValid(entry->query_ptr));
		str = (char *) dsa_get_address(htab_dsa, entry->query_ptr);
		values[i++] = CStringGetTextDatum(str);

		/* Fill-in cumulative statistics fields */
		values[i++] = RStatsPGetDatum(&entry->avg_error);
		values[i++] = RStatsPGetDatum(&entry->rms_error);
		values[i++] = RStatsPGetDatum(&entry->twa_error);
		values[i++] = RStatsPGetDatum(&entry->wca_error);
		values[i++] = RStatsPGetDatum(&entry->blks_accessed);
		values[i++] = RStatsPGetDatum(&entry->local_blks);
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
reset_htab()
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
		Assert(entry->key.queryId != UINT64CONST(0) &&
			   OidIsValid(entry->key.dbOid));

		CHECK_FOR_INTERRUPTS();

		/* At first, free memory, allocated for the query text */
		Assert(DsaPointerIsValid(entry->query_ptr));
		dsa_free(htab_dsa, entry->query_ptr);

		dshash_delete_current(&stat);
		pg_atomic_fetch_sub_u32(&shared->htab_counter, 1);

		/*
		 * htab_counter may be changes simultaneously. So, calculate how much
		 * entries we removed using a local variable.
		 */
		counter++;
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

Datum
to_reset(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to reset pg_track_optimizer statistics")));

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

		Assert(entry->key.queryId != UINT64CONST(0) &&
			   OidIsValid(entry->key.dbOid) &&
			   DsaPointerIsValid(entry->query_ptr));

		CHECK_FOR_INTERRUPTS();

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
	 * temporary file.
	 */
error:
	FileClose(file);
	unlink(tmpfile);

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
		return false;
	}

	/* Must load data into an empty hash table */
	if (pg_atomic_read_u32(&state->htab_counter) != 0)
	{
		/*
		 * Production behaviour. Don't do anything, just give a clue what
		 * the calling user may do to safely fix the problem.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("The pg_track_optimizer HTAB is not empty"),
				 errhint("Reset hash table in advance")));
	}

	file = PathNameOpenFile(filename, O_RDONLY | PG_BINARY);
	if (file < 0)
	{
		if (errno != ENOENT)
			goto read_error;
		/* File does not exist */
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
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("[%s] file \"%s\" contains more records than hash table may consume (%d)",
				 EXTENSION_NAME, filename, hashtable_elements_max()),
				 errdetail("skip data file load for safety"),
				 errhint("remove the file manually or reset statistics in advance")));
		}

		/* Load query string */
		nbytes = FileRead(file, &len, sizeof(uint32), filepos,
						  WAIT_EVENT_DATA_FILE_READ);
		if (nbytes != sizeof(uint32))
			goto read_error;
		COMP_CRC32C(crc, &len, sizeof(uint32));
		filepos += nbytes;

		disk_entry.query_ptr = dsa_allocate0(htab_dsa, len + 1);
		Assert(DsaPointerIsValid(disk_entry.query_ptr));
		str = (char *) dsa_get_address(htab_dsa, disk_entry.query_ptr);
		nbytes = FileRead(file, str, len, filepos, WAIT_EVENT_DATA_FILE_READ);
		if (nbytes != len)
			goto read_error;
		COMP_CRC32C(crc, str, len);
		filepos += nbytes;

		entry = dshash_find_or_insert(htab, &disk_entry.key, &found);
		if (found)
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("[%s] file \"%s\" has duplicate record with dbOid %u and queryId "UINT64_FORMAT,
				 EXTENSION_NAME, filename, disk_entry.key.dbOid, disk_entry.key.queryId)));

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
		memcpy(&entry->local_blks, &disk_entry.local_blks, sizeof(RStats));
		memcpy(&entry->exec_time, &disk_entry.exec_time, sizeof(RStats));
		memcpy(&entry->f_join_filter, &disk_entry.f_join_filter, sizeof(RStats));
		memcpy(&entry->f_scan_filter, &disk_entry.f_scan_filter, sizeof(RStats));
		memcpy(&entry->f_worst_splan, &disk_entry.f_worst_splan, sizeof(RStats));
		memcpy(&entry->njoins, &disk_entry.njoins, sizeof(RStats));

		entry->nexecs = disk_entry.nexecs;
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

	/*
	 * Verify we're at EOF - no extra data after the checksum.
	 * Use assertion to identify the issue during development cycle.
	 */
	nbytes = FileRead(file, &disk_entry, 1, filepos, WAIT_EVENT_DATA_FILE_READ);
	if (nbytes == 1)
	{
		Assert(0);
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("[%s] file \"%s\" contains more data than expected",
				 EXTENSION_NAME, filename)));
	}

	FileClose(file);
	pg_atomic_write_u32(&state->htab_counter, counter);
	elog(LOG, "[%s] %u records loaded from file \"%s\"",
		 EXTENSION_NAME, counter, filename);
	return counter;

/* Upper TRY/CATCH section must guarantee HTAB cleanup on ERROR */
read_error:
	ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("[%s] could not read file \"%s\": %m",
			 EXTENSION_NAME, filename)));
data_header_error:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" has incompatible header version %d instead of %d",
			 EXTENSION_NAME, filename, header, DATA_FILE_HEADER)));
data_version_error:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" has incompatible data format version %d instead of %d",
			 EXTENSION_NAME, filename, fmtver, DATA_FORMAT_VERSION)));
data_corrupted_error:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" contains invalid entry with queryId "UINT64_FORMAT" and dbOid %u",
			 EXTENSION_NAME, filename, disk_entry.key.queryId, disk_entry.key.dbOid),
			 errhint("File may be corrupted")));
crc_read_error:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" is missing CRC32C checksum",
			 EXTENSION_NAME, filename),
			 errdetail("File may be truncated or corrupted"),
			 errhint("Remove the file manually or reset statistics in advance")));
soft_failed_end:
	recreate_htab(state);
end:
	if (file >= 0)
		FileClose(file);

	return -1;
}

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

Datum
to_flush(PG_FUNCTION_ARGS)
{
	uint32 counter;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to flush pg_track_optimizer statistics")));

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

	if (!IsUnderPostmaster || code != 0 || htab == NULL || !auto_flush)
		return;

	/* On the backend shutdown flush the data only if something new arrived */
	if (pg_atomic_read_u32(&shared->need_syncing) == 0)
		return;

	elog(DEBUG1, "[%s] saving hash table to the disk", EXTENSION_NAME);

	/* Take exclusive lock to be sure no one flushes the data in parallel */
	LWLockAcquire(&shared->lock, LW_EXCLUSIVE);

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
