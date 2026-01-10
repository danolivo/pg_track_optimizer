# pg_track_optimizer: Technical Talk Outline
## PGConf.dev 2026 - 45 minutes

---

## Slide 1: Title (1 min)
**pg_track_optimizer: Tracking Cardinality Estimation Errors in Production**

Andrei Lepikhov

---

## Slide 2-3: The Problem (4 min)

**Current state of monitoring:**
```sql
-- pg_stat_statements shows execution metrics
SELECT query, calls, mean_exec_time,
       shared_blks_hit, shared_blks_read
FROM pg_stat_statements;
```

**What's missing:**
- Planner predicted 10 rows, actually got 10,000 → SeqScan instead of IndexScan
- Query is fast today, but one ANALYZE away from disaster
- No visibility into node-level estimation errors

**Example:**
```sql
EXPLAIN (ANALYZE) SELECT * FROM orders WHERE status = 'pending';
                                  QUERY PLAN
------------------------------------------------------------------------------
 Seq Scan on orders  (cost=0.00..1829.00 rows=100 width=180)
                     (actual time=0.123..45.231 rows=95234 loops=1)
```
Estimated 100, got 95,234 → 952× error. But mean_exec_time = 45ms, looks fine.

---

## Slide 4-6: Architecture Overview (5 min)

**Component stack:**
```
PostgreSQL Backend
    ├─ Executor (execMain.c)
    │   ├─ ExecutorStart_hook  ← pg_track_optimizer hooks here
    │   └─ ExecutorEnd_hook    ← and here
    ├─ Plan nodes (execnodes.h)
    │   └─ Instrumentation data
    └─ Dynamic Shared Memory
        └─ dshash: (dbOid, queryId) → DSMOptimizerTrackerEntry
```

**Key design choices:**

1. **Why executor hooks?**
   - Access to PlanState tree with instrumentation
   - See all plan nodes, not just top level
   - InstrEndLoop() gives us ntuples, nloops, nfiltered1/2
   - Zero cost when disabled

2. **Why DSM instead of fixed shared memory?**
   - No restart required (dsm_create/attach on demand)
   - Dynamic sizing based on workload
   - Crash-safe (survives backend crashes)
   - Per-database isolation via TODSMRegistry

3. **Why dshash instead of regular hash?**
   - Scalable shared hash table
   - Handles concurrent access
   - Memory comes from DSA (Dynamic Shared Area)

---

## Slide 7-10: Implementation Details (10 min)

**Executor hook integration:**
```c
void track_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    if (prev_ExecutorStart)
        prev_ExecutorStart(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);

    // Only track if enabled and has queryId
    if (track_mode == TRACK_MODE_DISABLED ||
        queryDesc->plannedstmt->queryId == UINT64CONST(0))
        return;

    // Enable instrumentation if needed
    queryDesc->instrument_options |= INSTRUMENT_ALL;
}
```

**Plan tree walking:**
```c
static bool prediction_walker(PlanState *pstate, void *context)
{
    PlanEstimatorContext *ctx = context;

    ctx->counter++;  // Track number of nodes
    planstate_tree_walker(pstate, prediction_walker, context);

    if (!pstate->instrument || pstate->instrument->nloops <= 0)
        return false;  // Skip never-executed nodes

    InstrEndLoop(pstate->instrument);  // Finalize instrumentation

    double plan_rows = pstate->plan->plan_rows;
    double real_rows = calculate_actual_rows(pstate);  // Handles parallel workers

    double error = fabs(log(real_rows / plan_rows));
    accumulate_error(ctx, error, pstate);

    return false;
}
```

**Handling parallel workers:**
```c
if (pstate->worker_instrument) {
    double divisor = pstate->worker_instrument->num_workers;

    // Account for leader participation (from explain.c)
    if (parallel_leader_participation) {
        double leader_contribution = 1.0 - (0.3 * divisor);
        if (leader_contribution > 0)
            divisor += leader_contribution;
    }

    plan_rows = pstate->plan->plan_rows * divisor;

    // Sum across all workers
    for (i = 0; i < num_workers; i++) {
        wnloops += worker_instrument[i].nloops;
        wntuples += worker_instrument[i].ntuples;
    }
}
```

**Including filtered tuples (leaf nodes only):**
```c
// This is key: planner estimates output rows, but filtered tuples
// represent real I/O cost that planner doesn't see
if (is_leaf_node(pstate)) {
    double nfiltered1 = pstate->instrument->nfiltered1;  // Qual filtering
    double nfiltered2 = pstate->instrument->nfiltered2;  // Visibility filtering
    real_rows += nfiltered1 + nfiltered2;
}
```

---

## Slide 11-13: Data Structures (5 min)

**Hash table entry:**
```c
typedef struct DSMOptimizerTrackerEntry {
    DSMOptimizerTrackerKey key;  // (dbOid, queryId)

    // Snapshots (overwritten each execution)
    int32 evaluated_nodes;
    int32 plan_nodes;

    // Cumulative statistics (RStats type)
    RStats avg_error;       // Simple average of errors
    RStats rms_error;       // sqrt(mean(error²))
    RStats twa_error;       // Σ(error × time) / total_time
    RStats wca_error;       // Σ(error × cost) / total_cost
    RStats blks_accessed;   // Block I/O
    RStats local_blks;      // work_mem indicator
    RStats exec_time;       // Milliseconds
    RStats f_join_filter;   // JOIN filtering factor
    RStats f_scan_filter;   // Scan filtering factor
    RStats f_worst_splan;   // SubPlan cost factor

    int64 nexecs;
    dsa_pointer query_ptr;  // Query text in DSA
} DSMOptimizerTrackerEntry;
```

**RStats: Welford's algorithm for streaming statistics:**
```c
typedef struct RStats {
    int64  count;
    double mean;
    double m2;     // Sum of squared differences
    double min;
    double max;
} RStats;  // Fixed 40 bytes

void rstats_add_value(RStats *stats, double value) {
    if (stats->count == 0) {
        stats->count = 1;
        stats->mean = value;
        stats->m2 = 0.0;
        stats->min = stats->max = value;
        return;
    }

    stats->count++;
    double delta = value - stats->mean;
    stats->mean += delta / stats->count;
    double delta2 = value - stats->mean;
    stats->m2 += delta * delta2;

    if (value < stats->min) stats->min = value;
    if (value > stats->max) stats->max = value;
}

// Variance = m2 / (count - 1)
// StdDev = sqrt(variance)
```

**Why Welford?**
- Numerically stable (avoids catastrophic cancellation)
- Single pass: O(1) time, O(1) space per update
- No need to store raw values
- Standard PostgreSQL pattern (used in pg_stats, aggregate functions)

---

## Slide 14-16: Comparison with pg_stat_statements (5 min)

**pg_stat_statements tracks:**
```c
typedef struct pgssEntry {
    uint64 queryid;
    int64  calls;
    double total_time;
    double min_time, max_time, mean_time;
    int64  shared_blks_hit, shared_blks_read;
    // ... more I/O and CPU metrics
} pgssEntry;
```

Focus: What happened during execution?

**pg_track_optimizer tracks:**
```c
typedef struct DSMOptimizerTrackerEntry {
    uint64 queryid;
    RStats avg_error;   // How wrong was the estimate?
    RStats f_join_filter;  // Where's the filtering overhead?
    // ... planning accuracy metrics
} DSMOptimizerTrackerEntry;
```

Focus: How accurate was the plan?

**They're complementary:**
```sql
-- Find queries with high estimation error AND frequent execution
SELECT s.query, s.calls, s.mean_exec_time,
       o.avg_error -> 'mean' as error,
       o.f_worst_splan -> 'mean' as splan
FROM pg_stat_statements s
JOIN pg_track_optimizer o USING (queryid)
WHERE o.avg_error -> 'mean' > 2.0
  AND s.calls > 100
ORDER BY s.calls * o.avg_error -> 'mean' DESC;
```

**Performance note:** ~95% of overhead is queryId computation. If you have `compute_query_id = on` (for pg_stat_statements), incremental overhead is <0.1%.

---

## Slide 17-20: Error Metrics (8 min)

**1. Why logarithmic scale?**
```
error = |log(actual / estimated)|
```

Rationale:
- Symmetry: 10→100 same as 100→10
- Scale invariance: works for 1 row or 1B rows
- Matches planner's log-based cost model
- Interpretation:
  - error 0.5 = 1.6× off
  - error 1.0 = 2.7× off
  - error 2.0 = 7.4× off
  - error 3.0 = 20× off

**2. Four aggregation methods:**

**Simple average:**
```c
ctx->avg_error = sum(error) / nnodes;
```
Baseline metric, equal weight per node.

**RMS (Root Mean Square):**
```c
ctx->rms_error = sqrt(sum(error²) / nnodes);
```
Emphasizes outliers (one huge error dominates).

**Time-Weighted Average:**
```c
ctx->twa_error = sum(error × node_time) / total_time;
```
Focus on errors in time-consuming nodes.

**Cost-Weighted Average:**
```c
ctx->wca_error = sum(error × node_cost) / total_cost;
```
Focus on errors in planner-expensive nodes.

**3. Advanced indicators:**

**JOIN filtering factor:**
```c
// Dimensionless metric: how much filtering overhead?
double join_filtered = (nfiltered1 + nfiltered2) / nloops;
double join_output = ntuples / nloops;
double relative_time = node_time / total_time;

if (join_output > 0) {
    f_join_filter = (join_filtered / join_output) × relative_time;
}
```

High values → JOIN producing many rows, then filtering them. Suggests join order or index issues.

**Scan filtering factor:**
```c
// For leaf nodes only
double scan_filtered = nfiltered1 / nloops;
double scan_output = ntuples / nloops;
double relative_time = node_time / total_time;

if (scan_output > 0) {
    f_scan_filter = (scan_filtered / scan_output) × relative_time;
}
```

High values → Index/SeqScan fetching many rows but filtering most. Classic missing index symptom.

**SubPlan cost factor:**
```c
// For correlated subqueries
foreach_node(SubPlanState, sps, pstate->subPlan) {
    double nloops = sps->planstate->instrument->nloops;
    double subplan_time = sps->planstate->instrument->total;
    double time_ratio = subplan_time / total_time;

    // Logarithmic dampening: nloops / log(nloops + 1)
    double loop_factor = nloops / log(nloops + 1.0);
    double cost_factor = loop_factor × time_ratio;

    if (cost_factor > ctx->f_worst_splan)
        ctx->f_worst_splan = cost_factor;
}
```

Detects expensive correlated subqueries. Logarithmic dampening: 10 loops → 4.3×, 100 → 21.7×, 1000 → 144.8×

---

## Slide 21-23: Production Deployment (7 min)

**Configuration:**
```ini
# postgresql.conf
shared_preload_libraries = 'pg_track_optimizer'

# After CREATE EXTENSION pg_track_optimizer:
pg_track_optimizer.mode = 'normal'           # Only track errors > threshold
pg_track_optimizer.log_min_error = 2.0       # Log EXPLAIN for error > 2.0
pg_track_optimizer.hash_mem = 10240          # 10MB shared memory
```

**Modes:**
- `disabled`: Extension inactive (default)
- `normal`: Track queries exceeding log_min_error (production)
- `forced`: Track all queries (debugging)

**Workflow:**
```sql
-- 1. Deploy and let it accumulate (1-2 weeks)
SELECT count(*) FROM pg_track_optimizer;

-- 2. Find worst offenders
SELECT query,
       avg_error -> 'mean' as error,
       nexecs,
       exec_time -> 'mean' as avg_ms
FROM pg_track_optimizer
WHERE avg_error -> 'mean' > 2.0
ORDER BY nexecs DESC
LIMIT 20;

-- 3. Investigate specific query
SELECT queryid, query,
       avg_error -> 'mean' as avg_err,
       avg_error -> 'stddev' as stddev_err,
       avg_error -> 'max' as max_err,
       f_join_filter -> 'mean' as join_flt,
       f_scan_filter -> 'mean' as scan_flt,
       f_worst_splan -> 'mean' as splan
FROM pg_track_optimizer
WHERE queryid = 42387612345;

-- 4. Fix (ANALYZE, extended stats, index, rewrite)

-- 5. Reset and verify improvement
SELECT pg_track_optimizer_reset();
```

**Measured overhead:**
- Disabled: 0%
- Normal mode (threshold=2.0): <0.1% (only ~5% of queries tracked)
- Forced mode: ~1.5%
- Note: If compute_query_id already enabled, overhead negligible

**Memory usage:**
- ~500 bytes per tracked query (entry + query text)
- 10MB = ~20,000 unique queries
- Configurable via hash_mem

**Common findings:**
1. Outdated statistics → ANALYZE
2. Column correlation → CREATE STATISTICS
3. Functional predicates → Expression indexes
4. Correlated subqueries → Rewrite as JOIN/LATERAL

---

## Slide 24-25: Real Example (3 min)

**Before:**
```sql
EXPLAIN (ANALYZE)
SELECT o.order_id, c.name, SUM(oi.amount)
FROM orders o
  JOIN customers c ON o.customer_id = c.id
  JOIN order_items oi ON oi.order_id = o.id
WHERE o.status = 'pending'
GROUP BY o.order_id, c.name;

-- pg_stat_statements: mean_exec_time = 125ms (acceptable)
-- pg_track_optimizer:
--   avg_error = 3.2 (20× estimation error)
--   f_join_filter = 8.5 (high JOIN filtering overhead)
```

Investigation:
```sql
SELECT * FROM pg_stats
WHERE tablename = 'orders' AND attname = 'status';
-- last_analyze: 2023-11-15 (3 months ago!)
```

**Fix:**
```sql
ANALYZE orders;
```

**After:**
```sql
-- pg_stat_statements: mean_exec_time = 18ms (7× faster)
-- pg_track_optimizer:
--   avg_error = 0.8 (2.2× error, acceptable)
--   f_join_filter = 0.3 (filtering minimal)
```

Planner now uses better join order because estimates are accurate.

---

## Slide 26: Limitations & Future Work (2 min)

**Current limitations:**
- No historical tracking (in-memory only, optional flush)
- No automatic fix suggestions
- Requires manual interpretation
- Query text in shared memory (not normalized further than pg_stat_statements)

**Future directions:**
- Integration with auto_explain
- Automatic ANALYZE recommendations
- Extended statistics suggestions
- Query rewrite hints
- Trend analysis over time

---

## Slide 27: Summary (1 min)

**Key points:**
1. Cardinality estimation errors are invisible to pg_stat_statements
2. Executor hooks + DSM enable efficient node-level tracking
3. RStats type provides rich statistical insight (mean, stddev, min, max)
4. Four error metrics + filtering factors identify different problem types
5. <2% overhead makes it viable for production
6. Complements pg_stat_statements, doesn't replace it

**Try it:**
- GitHub: https://github.com/danolivo/pg_track_optimizer
- PostgreSQL 17+
- MIT License

---

## Slide 28: Q&A (3 min)

Questions?

**Common questions to anticipate:**
- Q: Why not use EXPLAIN logging?
  A: Not scalable, no statistics across executions, too verbose

- Q: Interaction with JIT?
  A: JIT affects execution time but not cardinality estimates

- Q: What about prepared statements / plan caching?
  A: Tracks based on queryId, so same as pg_stat_statements

- Q: Can it cause OOM?
  A: Hash table size limited by hash_mem, no unbounded growth

---

*End of Presentation*

## Backup Slides (if time permits or for Q&A)

**Backup 1: DSM initialization code**
```c
void _PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
        return;

    // GUC registration
    DefineCustomEnumVariable("pg_track_optimizer.mode", ...);
    DefineCustomRealVariable("pg_track_optimizer.log_min_error", ...);
    DefineCustomIntVariable("pg_track_optimizer.hash_mem", ...);

    // Hook registration
    prev_ExecutorStart = ExecutorStart_hook;
    ExecutorStart_hook = track_ExecutorStart;
    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = track_ExecutorEnd;

    // Request shared memory (calculated based on hash_mem)
    RequestAddinShmemSpace(/* size */);
    RequestNamedLWLockTranche("pg_track_optimizer", 1);
}
```

**Backup 2: Concurrency & atomics**
```c
// Read path: lock-free
uint32 count = pg_atomic_read_u32(&shared->htab_counter);

// Write path: with lock
LWLockAcquire(&shared->lock, LW_EXCLUSIVE);
entry = dshash_find_or_insert(htab, &key, &found);
if (!found) {
    pg_atomic_fetch_add_u32(&shared->htab_counter, 1);
    initialize_entry(entry);
}
rstats_add_value(&entry->avg_error, ctx->avg_error);
LWLockRelease(&shared->lock);
```

**Backup 3: Handling never-executed nodes**
```c
// Skip nodes that never executed (e.g., alternative index paths)
if (!pstate->instrument || pstate->instrument->nloops <= 0.0)
    return false;

// Why? We can't calculate error without actual execution data.
// Planner's estimate is meaningless without comparison point.
```
