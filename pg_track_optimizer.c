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
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "plan_error.h"

PG_MODULE_MAGIC;

#define track_optimizer_enabled(eflags) \
	( \
	IsQueryIdEnabled() && !IsParallelWorker() && \
	queryDesc->plannedstmt->utilityStmt == NULL && \
	(log_min_error >= 0. || track_mode == TRACK_MODE_FORCED) && \
	track_mode != TRACK_MODE_DISABLED && \
	((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0) \
	)

#define DATATBL_NCOLS	(11)

typedef struct TODSMRegistry
{
	LWLock				lock;
	dshash_table	   *htab;
	dsa_handle			dsah;
	dshash_table_handle	dshh;

	pg_atomic_uint32	htab_counter;
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

typedef struct DSMOptimizerTrackerEntry
{
	DSMOptimizerTrackerKey	key;

	double					avg_error;
	double					rms_error;
	double					twa_error;
	double					wca_error;
	dsa_pointer				query_ptr;
	int32					evaluated_nodes;
	int32					plan_nodes;
	double					exec_time;
	int64					nexecs; /* Number of executions taken into account */
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

void _PG_init(void);

static bool _load_hash_table(TODSMRegistry *state);
static bool _flush_hash_table(void);

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
	int					tranche_id; /* dshash tranche */

	Assert(htab_dsa == NULL && htab == NULL);

#if PG_VERSION_NUM < 190000
	LWLockInitialize(&state->lock, LWLockNewTrancheId());
	tranche_id = LWLockNewTrancheId();
	LWLockRegisterTranche(tranche_id, "pgto_dshash_tranche");
#else
	LWLockInitialize(&state->lock,
					 LWLockNewTrancheId("pgto_lock_tranche"));
	tranche_id = LWLockNewTrancheId("pgto_dshash_tranche");
#endif
	htab_dsa = dsa_create(tranche_id);
	state->dsah = dsa_get_handle(htab_dsa);
	dsa_pin(htab_dsa);

	htab = dshash_create(htab_dsa, &dsh_params, 0);
	state->dshh = dshash_get_hash_table_handle(htab);
	pg_atomic_init_u32(&state->htab_counter, 0);

	(void) _load_hash_table(state);
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

	if (log_min_error < 0 || normalized_error < log_min_error)
		return;

	msec = queryDesc->totaltime->total * 1000.0;

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
	 */
	ereport(LOG,
			(errmsg("duration: %.3f ms, relative error: %.4lf, plan:\n%s",
					msec, normalized_error, es->str->data),
			 errhidestmt(true)));
}

/*
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

	counter = pg_atomic_read_u32(&shared->htab_counter);

	if (counter == UINT32_MAX ||
		counter > (uint32)(hash_mem / sizeof(DSMOptimizerTrackerEntry)))
		/* TODO: set status of full hash table */
		return false;

	memset(&key, 0, sizeof(DSMOptimizerTrackerKey));
	key.dbOid = MyDatabaseId;
	key.queryId = queryDesc->plannedstmt->queryId;
	entry = dshash_find_or_insert(htab, &key, &found);
	entry->avg_error = ctx->avg_error;
	entry->rms_error = ctx->rms_error;
	entry->twa_error = ctx->twa_error;
	entry->wca_error = ctx->wca_error;
	entry->evaluated_nodes = ctx->nnodes;
	entry->plan_nodes = ctx->counter;
	entry->exec_time = ctx->totaltime;

	if (!found)
	{
		size_t	len = strlen(queryDesc->sourceText);
		char   *strptr;

		/* Put the query string into the shared memory too */
		entry->query_ptr = dsa_allocate0(htab_dsa, len + 1);
		Assert(DsaPointerIsValid(entry->query_ptr));
		strptr = (char *) dsa_get_address(htab_dsa, entry->query_ptr);
		strlcpy(strptr, queryDesc->sourceText, len + 1);

		entry->nexecs = 0;
		pg_atomic_fetch_add_u32(&shared->htab_counter, 1);
	}

	entry->nexecs++;

	dshash_release_lock(htab, entry);

	return true;
}

static void
track_ExecutorEnd(QueryDesc *queryDesc)
{
	MemoryContext	oldcxt;
	double			normalized_error = -1.0;
	PlanEstimatorContext	ctx;

	track_attach_shmem();

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

	normalized_error = plan_error(queryDesc, &ctx);

	/*
	 * Store data in the hash table and/or print it to the log. Decision on what
	 * to do each routine makes individually.
	 */
	store_data(queryDesc, &ctx);
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
							 -1.0, INT_MAX, /* Looks like such a huge error, as INT_MAX doesn't make sense */
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

	MarkGUCPrefixReserved("pg_track_optimizer");

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = explain_ExecutorStart;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = track_ExecutorEnd;
}

/* -----------------------------------------------------------------------------
 *
 * UI routines
 *
 */

static void
_init_rsinfo(PG_FUNCTION_ARGS, ReturnSetInfo *rsinfo, int total_ncols)
{
	MemoryContext	 oldcontext;
	TupleDesc		 tup_desc;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tup_desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (tup_desc->natts != total_ncols)
		elog(ERROR, "incorrect number of output arguments");

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->setDesc = tup_desc;

	MemoryContextSwitchTo(oldcontext);
}

PG_FUNCTION_INFO_V1(to_show_data);
PG_FUNCTION_INFO_V1(to_reset);

Datum
to_show_data(PG_FUNCTION_ARGS)
{
	ReturnSetInfo			   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum						values[DATATBL_NCOLS];
	bool						nulls[DATATBL_NCOLS];
	dshash_seq_status			stat;
	DSMOptimizerTrackerEntry   *entry;

	track_attach_shmem();

	LWLockAcquire(&shared->lock, LW_SHARED);
	_init_rsinfo(fcinfo, rsinfo, DATATBL_NCOLS);

	dshash_seq_init(&stat, htab, true);
	while ((entry = dshash_seq_next(&stat)) != NULL)
	{
		int		i = 0;
		char   *str;

		Assert(entry->key.queryId != UINT64CONST(0) &&
			   OidIsValid(entry->key.dbOid));

		memset(nulls, 0, DATATBL_NCOLS);
		values[i++] = ObjectIdGetDatum(entry->key.dbOid);
		values[i++] = Int64GetDatum(entry->key.queryId);

		/* Query string */
		Assert(DsaPointerIsValid(entry->query_ptr));
		str = (char *) dsa_get_address(htab_dsa, entry->query_ptr);
		values[i++] = CStringGetTextDatum(str);

		values[i++] = Float8GetDatum(entry->avg_error);
		values[i++] = Float8GetDatum(entry->rms_error);
		values[i++] = Float8GetDatum(entry->twa_error);
		values[i++] = Float8GetDatum(entry->wca_error);
		values[i++] = Int32GetDatum(entry->evaluated_nodes);
		values[i++] = Int32GetDatum(entry->plan_nodes);
		values[i++] = Float8GetDatum(entry->exec_time * 1000.); /* sec -> msec */
		values[i++] = Int64GetDatum(entry->nexecs);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		Assert(i == DATATBL_NCOLS);
	}
	dshash_seq_term(&stat);
	LWLockRelease(&shared->lock);
	return (Datum) 0;
}

/*
 * Reset the state of this extension to default. This will clean up all additionally
 * allocated resources and reset static and global state variables.
 */
Datum
to_reset(PG_FUNCTION_ARGS)
{
	dshash_seq_status			stat;
	DSMOptimizerTrackerEntry   *entry;

	track_attach_shmem();

	LWLockAcquire(&shared->lock, LW_EXCLUSIVE);

	/*
	 * Destroying shared hash table is a bit dangerous procedure. Without full
	 * understanding of the dshash_destroy() technique, delete elements more
	 * simply, one by one.
	 */
	dshash_seq_init(&stat, htab, true);
	while ((entry = dshash_seq_next(&stat)) != NULL)
	{
		uint32 pre;

		Assert(entry->key.queryId != UINT64CONST(0) &&
			   OidIsValid(entry->key.dbOid));

		/* At first, free memory, allocated for the query text */
		Assert(DsaPointerIsValid(entry->query_ptr));
		dsa_free(htab_dsa, entry->query_ptr);

		dshash_delete_current(&stat);
		pre = pg_atomic_fetch_sub_u32(&shared->htab_counter, 1);

		if (pre <= 0)
		{
			/* Trigger a reboot to clean up the state. I see no other solution */
			elog(PANIC, "Inconsistency in the pg_track_optimizer hash table state");
		}
	}
	dshash_seq_term(&stat);

	/* Clean disk storage too */
	(void) _flush_hash_table();

	LWLockRelease(&shared->lock);

	PG_RETURN_VOID();
}

/* -----------------------------------------------------------------------------
 *
 * Disk operations
 *
 * -------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(to_flush);

static const uint32 DATA_FILE_HEADER	= 12354678;
static const uint32 DATA_FORMAT_VERSION = 1;

static const DSMOptimizerTrackerEntry EOFEntry = {
											.key.dbOid = 0,
											.key.queryId = 0,
											.avg_error = -2.,
											.rms_error = -2.,
											.twa_error = -2.,
											.wca_error = -2.,
											.query_ptr = 0,
											.evaluated_nodes = -1,
											.plan_nodes = -1,
											.exec_time = -1.,
											.nexecs = -1
											};
#define IsEOFEntry(entry) ( \
	(entry)->key.dbOid == EOFEntry.key.dbOid && \
	(entry)->key.queryId == EOFEntry.key.queryId && \
	(entry)->query_ptr == EOFEntry.query_ptr && \
	(entry)->evaluated_nodes == EOFEntry.evaluated_nodes && \
	(entry)->plan_nodes == EOFEntry.plan_nodes && \
	(entry)->nexecs == EOFEntry.nexecs \
)

#define EXTENSION_NAME "pg_track_optimizer"
static const char *filename = EXTENSION_NAME".stat";

/*
 * Specifics of the storage procedure of dshash table:
 * we don't block the table entirely, so we don't know how many records
 * will be eventually stored.
 * Return true in the case of success.
 */
static bool
_flush_hash_table(void)
{
	dshash_seq_status			stat;
	DSMOptimizerTrackerEntry   *entry;
	char					   *tmpfile = psprintf("%s.tmp", EXTENSION_NAME);
	FILE					   *file;
	uint32						counter = 0;

	if (!IsUnderPostmaster)
		return false;

	file = AllocateFile(tmpfile, PG_BINARY_W);
	if (file == NULL)
		goto error;

	/* Add a header to the file for more reliable identification of the data */
	if (fwrite(&DATA_FILE_HEADER, sizeof(uint32), 1, file) != 1 ||
		fwrite(&DATA_FORMAT_VERSION, sizeof(uint32), 1, file) != 1)
		goto error;

	dshash_seq_init(&stat, htab, true);
	while ((entry = dshash_seq_next(&stat)) != NULL)
	{
		char   *str;
		uint32	len;

		Assert(entry->key.queryId != UINT64CONST(0) &&
			   OidIsValid(entry->key.dbOid) &&
			   DsaPointerIsValid(entry->query_ptr));

		str = (char *) dsa_get_address(htab_dsa, entry->query_ptr);
		len = strlen(str);

		/*
		 * Write data into the file. It is more or less stable procedure:
		 * I don't think someone will try to save the data on one platform and
		 * restore it on very different another one.
		 */
		if (fwrite(entry, sizeof(DSMOptimizerTrackerEntry), 1, file) != 1 ||
			fwrite(&len, sizeof(uint32), 1, file) != 1 ||
			fwrite(str, len, 1, file) != 1)
			goto error;

		counter++;
	}
	dshash_seq_term(&stat);

	/* As the last record, write an EOF record and the number of records that have been written */
	if (fwrite(&EOFEntry, sizeof(DSMOptimizerTrackerEntry), 1, file) != 1 ||
		fwrite(&counter, sizeof(uint32), 1, file) != 1)
		goto error;

	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

	(void) durable_rename(tmpfile, filename, LOG);
	pfree(tmpfile);
	elog(LOG, "[%s] %u records stored in file \"%s\"",
		 EXTENSION_NAME, counter, filename);
	return true;

error:
	ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("[%s] could not write file \"%s\": %m",
			 EXTENSION_NAME, tmpfile)));

	if (file)
		FreeFile(file);
	unlink(tmpfile);
	pfree(tmpfile);
	return false;
}

/*
 * Read data file record by record and add each record into the new table
 * Provide reference to the shared area because the local pointer still not
 * initialized.
 */
static bool
_load_hash_table(TODSMRegistry *state)
{
	FILE					   *file;
	uint32						header;
	int32						pgver;
	uint32						counter = 0;
	DSMOptimizerTrackerEntry	disk_entry;
	DSMOptimizerTrackerEntry   *entry;

	track_attach_shmem();

	file = AllocateFile(filename, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;
		/* File does not exist */
		return false;
	}

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		fread(&pgver, sizeof(uint32), 1, file) != 1)
		goto read_error;
	if (header != DATA_FILE_HEADER)
		goto data_header_error;
	if (pgver != DATA_FORMAT_VERSION)
		goto data_version_error;

	while (!feof(file))
	{
		char   *str;
		uint32	len;
		bool	found;

		/* First step - read the record */
		if (fread(&disk_entry, sizeof(DSMOptimizerTrackerEntry), 1, file) != 1)
			goto read_error;

		/* Check last record */
		if (IsEOFEntry(&disk_entry))
		{
			uint32 cnt;

			if (fread(&cnt, sizeof(uint32), 1, file) != 1)
				goto read_error;

			if (cnt != counter)
				elog(ERROR,
					 "[%s] incorrect number of records read: %u instead of %u",
					 EXTENSION_NAME, counter, cnt);

			/* Correct finish of the load operation */
			break;
		}

		/* The case of next entry */

		Assert(disk_entry.key.queryId != UINT64CONST(0) &&
			   OidIsValid(disk_entry.key.dbOid));

		/* Load query string */
		if (fread(&len, sizeof(uint32), 1, file) != 1)
			goto read_error;
		disk_entry.query_ptr = dsa_allocate0(htab_dsa, len + 1);
		Assert(DsaPointerIsValid(disk_entry.query_ptr));
		str = (char *) dsa_get_address(htab_dsa, disk_entry.query_ptr);
		if (fread(str, len, 1, file) != 1)
			goto read_error;

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
		entry->avg_error = disk_entry.avg_error;
		entry->rms_error = disk_entry.rms_error;
		entry->twa_error = disk_entry.twa_error;
		entry->wca_error = disk_entry.wca_error;
		entry->evaluated_nodes = disk_entry.evaluated_nodes;
		entry->plan_nodes = disk_entry.plan_nodes;
		entry->exec_time = disk_entry.exec_time;
		entry->nexecs = disk_entry.nexecs;
		entry->query_ptr = disk_entry.query_ptr;

		dshash_release_lock(htab, entry);
		counter++;
	}

	FreeFile(file);
	pg_atomic_write_u32(&state->htab_counter, counter);
	elog(LOG, "[%s] %u records loaded from file \"%s\"",
		 EXTENSION_NAME, counter, filename);
	return true;

read_error:
	ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("[%s] could not read file \"%s\": %m",
			 EXTENSION_NAME, filename)));
	goto fail;
data_header_error:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" has incompatible header version %d instead of %d",
			 EXTENSION_NAME, filename, header, DATA_FILE_HEADER)));
	goto fail;
data_version_error:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("[%s] file \"%s\" has incompatible data format version %d instead of %d",
			 EXTENSION_NAME, filename, pgver, DATA_FORMAT_VERSION)));
fail:
	if (file)
		FreeFile(file);

	return false;
}

Datum
to_flush(PG_FUNCTION_ARGS)
{
	track_attach_shmem();

	_flush_hash_table();

	PG_RETURN_VOID();
}
