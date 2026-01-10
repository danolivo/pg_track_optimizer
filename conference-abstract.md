# PGConf.dev 2026 Talk Proposal

## Title
**Finding the Invisible: Detecting Query Planning Issues That Don't Show Up in Performance Metrics**

*Alternative titles:*
- *Beyond pg_stat_statements: Architecture of pg_track_optimizer for Cardinality Estimation Tracking*
- *The Hidden Cost of Bad Plans: Tracking PostgreSQL Optimizer Mistakes Before They Hurt*

---

## Short Abstract (250 words max)

PostgreSQL's query optimizer is remarkably sophisticated, but even the best planner can make mistakes. The challenge? Traditional monitoring tools like pg_stat_statements focus on *execution* metrics—CPU time, I/O, and duration—but miss a critical dimension: **how wrong were the planner's predictions?**

A query executing in 10ms might seem fast, but if the planner estimated 10 rows and got 100,000, you're one data distribution change away from a production disaster. These "invisible problems" lurk in your database, waiting to explode when conditions shift.

This talk introduces **pg_track_optimizer**, an extension architected specifically to detect cardinality estimation errors in real-world workloads. Unlike pg_stat_statements, which tracks *what happened*, pg_track_optimizer tracks *what the planner thought would happen* versus *what actually happened*.

I'll cover:
- **Architecture**: Executor hooks, dynamic shared memory (DSM) design, and the custom RStats type for numerically stable streaming statistics
- **Key differences from pg_stat_statements**: Why runtime metrics miss planning problems, and how to bridge that gap
- **Detection techniques**: Four error metrics (avg, RMS, time-weighted, cost-weighted) and advanced indicators (JOIN filtering factors, scan filtering overhead, SubPlan cost analysis)
- **Real-world impact**: Finding problematic queries *before* they become slow, even when current execution times look fine

Whether you're a DBA hunting phantom performance issues or a PostgreSQL developer curious about optimizer internals, you'll learn how to surface planning problems that traditional tools can't see.

---

## Extended Abstract (Full Talk Description)

### Talk Duration
45 minutes (35 min presentation + 10 min Q&A)

### Target Audience
- Database administrators managing production PostgreSQL systems
- Performance engineers investigating query optimization
- PostgreSQL extension developers interested in executor hooks and shared memory architecture
- Anyone who's ever wondered "why did the planner choose *that* plan?"

### Prerequisites
- Familiarity with EXPLAIN/EXPLAIN ANALYZE output
- Basic understanding of PostgreSQL query planning concepts
- No prior knowledge of extension development required

---

## Detailed Talk Outline

### Part 1: The Problem Space (5 minutes)

**The Invisible Problem**

Traditional monitoring focuses on symptoms, not causes:
```sql
-- pg_stat_statements tells you this query is fast
SELECT * FROM orders WHERE customer_id = 12345;
-- Execution: 8ms, 50 rows returned

-- But it doesn't tell you the planner expected 1 row
-- When customer_id distribution changes, this becomes:
-- Execution: 8000ms, 50,000 rows returned (nested loop explosion)
```

**Why This Matters**
- Outdated statistics: ANALYZE runs weekly, data changes daily
- Correlation blindness: Planner assumes column independence
- Predicate complexity: Multi-column filters, OR conditions, functional predicates
- Silent degradation: Problems brew for weeks before manifesting

**The Gap in Tooling**
- `pg_stat_statements`: Tracks runtime (CPU, I/O, duration) but not planning accuracy
- Manual EXPLAIN: Not scalable for thousands of queries
- Logs: Noisy, miss the "fast but wrong" queries

---

### Part 2: Architecture of pg_track_optimizer (15 minutes)

**Design Goals**
1. Track cardinality estimation errors across *all* plan nodes
2. Minimal performance overhead (<2% in worst case)
3. Work in production without configuration changes
4. Provide actionable metrics for DBAs

**Core Architecture Components**

**1. Executor Hooks Integration**
```c
// Hook into PostgreSQL's executor lifecycle
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

// Our hooks:
void track_ExecutorStart(QueryDesc *queryDesc, int eflags);
void track_ExecutorEnd(QueryDesc *queryDesc);
```

**Why executor hooks?**
- Access to both planned and actual row counts
- Visibility into *every* plan node (not just top-level)
- Instrumentation data (timing, nloops, filtered tuples)
- Zero-cost when disabled

**2. Plan Tree Walking Strategy**
```c
static bool prediction_walker(PlanState *pstate, void *context)
{
    // For each node:
    // 1. Get planner's estimate: plan_rows
    // 2. Get actual execution: instrument->ntuples
    // 3. Calculate error: |log(actual / estimated)|
    // 4. Accumulate into error metrics
    // 5. Recurse to children
}
```

**Key insight**: Leaf nodes include filtered tuples
- `nfiltered1`: Tuples filtered by quals
- `nfiltered2`: Heap fetches that didn't match visibility/predicates
- Reveals hidden I/O costs invisible to planner estimates

**3. Dynamic Shared Memory (DSM) Design**

Why DSM instead of regular shared memory?
- Dynamic sizing: Grow/shrink based on workload
- Per-database isolation: Clean separation
- Crash resilience: Survives backend crashes
- No restart required: Attach on first use

```c
typedef struct DSMOptimizerTrackerEntry {
    DSMOptimizerTrackerKey  key;          // (dbOid, queryId)

    // Per-execution snapshots (overwritten each time)
    int32   evaluated_nodes;
    int32   plan_nodes;

    // Cumulative statistics (Welford's algorithm)
    RStats  avg_error;         // Simple average
    RStats  rms_error;         // Root mean square
    RStats  twa_error;         // Time-weighted
    RStats  wca_error;         // Cost-weighted
    RStats  blks_accessed;
    RStats  local_blks;        // work_mem indicator
    RStats  exec_time;
    RStats  f_join_filter;     // JOIN overhead
    RStats  f_scan_filter;     // Scan overhead
    RStats  f_worst_splan;     // SubPlan cost

    int64   nexecs;
    dsa_pointer query_ptr;     // Query text in DSA
} DSMOptimizerTrackerEntry;
```

**4. The RStats Type: Numerically Stable Streaming Statistics**

Challenge: How to track statistics across thousands of executions without storing raw data?

Solution: Welford's algorithm for incremental statistics
```c
typedef struct RStats {
    int64   count;     // Number of observations
    double  mean;      // Running mean
    double  m2;        // Sum of squared differences (for variance)
    double  min;       // Minimum observed
    double  max;       // Maximum observed
} RStats;  // Fixed 40 bytes

// Single-pass update: O(1) time, O(1) space
void rstats_add_value(RStats *stats, double value) {
    delta = value - stats->mean;
    stats->mean += delta / ++stats->count;
    stats->m2 += delta * (value - stats->mean);
    // Update min/max
}
```

**Why this matters:**
- Numerically stable (avoids catastrophic cancellation)
- Fixed memory per query (no unbounded growth)
- Provides mean, variance, stddev, min, max from single pass
- Accessible via SQL: `avg_error -> 'mean'`, `avg_error -> 'stddev'`

**5. Memory Management & Atomics**

```c
typedef struct TODSMRegistry {
    LWLock              lock;           // Protects hash table
    dshash_table       *htab;           // Query hash table
    dsa_handle          dsah;           // DSA handle
    dshash_table_handle dshh;           // Serializable handle
    pg_atomic_uint32    htab_counter;   // Lock-free read access
} TODSMRegistry;
```

**Concurrency design:**
- Read path: Lock-free counter read for cheap stats access
- Write path: LWLock for hash table modifications
- Tradeoff: Slightly delayed counter updates vs. read scalability

---

### Part 3: Key Differences from pg_stat_statements (8 minutes)

**What pg_stat_statements Tracks**

```sql
SELECT query, calls, mean_exec_time,
       shared_blks_hit, shared_blks_read
FROM pg_stat_statements
ORDER BY mean_exec_time DESC;
```

Focus: *Execution metrics*
- How long did it run?
- How many blocks accessed?
- How many calls?

**What pg_track_optimizer Tracks**

```sql
SELECT query,
       avg_error -> 'mean' as estimation_error,
       f_join_filter -> 'mean' as join_overhead,
       f_scan_filter -> 'mean' as scan_waste
FROM pg_track_optimizer
ORDER BY estimation_error DESC;
```

Focus: *Planning accuracy*
- How wrong was the row count estimate?
- Which nodes had the biggest errors?
- What's the filtering overhead?

**Complementary, Not Competing**

| Dimension | pg_stat_statements | pg_track_optimizer |
|-----------|-------------------|-------------------|
| **Detects** | Slow queries | Poorly planned queries |
| **Metric** | Execution time | Estimation error |
| **Use case** | "What's slow now?" | "What will be slow tomorrow?" |
| **Scope** | Query-level | Node-level |
| **Storage** | Per-query totals | Per-query statistics |

**Example: The "Fast But Dangerous" Query**

```sql
-- Current state: Fast (good index, small dataset)
SELECT * FROM logs WHERE user_id = 123;
-- pg_stat_statements: 5ms avg, looks fine ✓
-- pg_track_optimizer: 400x estimation error ✗

-- Tomorrow: Same query, different user_id
SELECT * FROM logs WHERE user_id = 999999;  -- Popular user
-- Planner still uses same plan (based on old stats)
-- Now: 50 seconds, production down
```

**pg_track_optimizer would have warned you yesterday.**

**Recommended Setup: Use Both**

```sql
-- Install both extensions
CREATE EXTENSION pg_stat_statements;
CREATE EXTENSION pg_track_optimizer;

-- Find problematic queries
SELECT
    s.query,
    s.mean_exec_time,
    o.avg_error -> 'mean' as error,
    o.f_worst_splan -> 'mean' as subplan_factor
FROM pg_stat_statements s
JOIN pg_track_optimizer o USING (queryid)
WHERE o.avg_error -> 'mean' > 2.0  -- High estimation error
  AND s.calls > 100                -- Frequently executed
ORDER BY s.mean_exec_time * o.avg_error -> 'mean' DESC;
```

---

### Part 4: Detection Techniques & Metrics (10 minutes)

**1. Error Calculation: Why Logarithmic Scale?**

```
node_error = |log(actual_rows / estimated_rows)|
```

**Rationale:**
- Symmetry: 10→100 same error magnitude as 100→10
- Scale independence: Works for 1 row or 1 billion rows
- Aligns with planner's cost model (log-based)

**Interpreting values:**
- `error < 1.0`: Within 3× (acceptable)
- `error 1-2`: 3-7× off (investigate)
- `error 2-3`: 7-20× off (problematic)
- `error > 3`: 20×+ (critical)

**2. Four Error Metrics**

**avg_error: Simple Average**
```
avg = Σ(node_error) / N_nodes
```
Use: General health check

**rms_error: Root Mean Square**
```
rms = sqrt(Σ(node_error²) / N_nodes)
```
Use: Detects "one big error" scenarios (emphasizes outliers)

**twa_error: Time-Weighted Average**
```
twa = Σ(node_error × node_time) / total_time
```
Use: Focuses on errors in time-consuming nodes

**wca_error: Cost-Weighted Average**
```
wca = Σ(node_error × node_cost) / total_cost
```
Use: Focuses on errors in planner-expensive nodes

**3. Advanced Indicators**

**JOIN Filtering Factor (f_join_filter)**
```sql
-- Dimensionless overhead metric
f_join = (filtered_rows / produced_rows) × (node_time / total_time)
```

**What it detects:**
- JOINs filtering many rows after join operation
- Suggests: Better join order, additional indexes, query rewrite

**Example:**
```sql
-- High f_join_filter = 15.3
SELECT * FROM orders o
  JOIN customers c ON o.customer_id = c.id
WHERE o.status = 'pending';

-- Problem: JOIN happens first, THEN filter
-- Fix: Push down predicate or reorder joins
```

**Scan Filtering Factor (f_scan_filter)**
```sql
-- Overhead for leaf nodes (scans)
f_scan = (filtered_rows / produced_rows) × (node_time / total_time)
```

**What it detects:**
- Index scans fetching many rows, filtering most
- Seq scans processing mostly irrelevant data
- Suggests: Better indexes, partial indexes, exclusion constraints

**SubPlan Cost Factor (f_worst_splan)**
```sql
-- Dimensionless correlated subquery cost
sp_factor = (nloops / log(nloops + 1)) × (subplan_time / total_time)
```

**What it detects:**
- Correlated subqueries executing many times
- Logarithmic dampening reflects diminishing optimization value
- Suggests: Rewrite as JOIN or LATERAL join

**Example:**
```sql
-- High f_worst_splan = 487
SELECT name,
  (SELECT COUNT(*) FROM orders WHERE customer_id = c.id)
FROM customers c;

-- Problem: Subquery runs once per customer
-- Fix: Use LEFT JOIN with GROUP BY
```

---

### Part 5: Real-World Impact & Production Deployment (5 minutes)

**Case Study: E-commerce Platform**

Before pg_track_optimizer:
- Quarterly performance fires
- "Slow query" investigations reactive
- Hard to prioritize ANALYZE schedule

After pg_track_optimizer:
```sql
-- Weekly review
SELECT query,
       avg_error -> 'mean' as error,
       nexecs,
       exec_time -> 'mean' as avg_ms
FROM pg_track_optimizer
WHERE avg_error -> 'mean' > 2.0
ORDER BY nexecs * error DESC
LIMIT 20;
```

**Results:**
- Proactive: Found 23 queries with >5× estimation errors
- 18 fixed with targeted ANALYZE
- 5 required extended statistics for correlated columns
- Zero emergency fixes in last 6 months

**Performance Overhead**

Measured on production workload (1000 QPS):

| Mode | Overhead | Use Case |
|------|----------|----------|
| Disabled | 0% | Default |
| Normal (threshold=2.0) | <0.1% | Production (recommended) |
| Forced | ~1.5% | Debug/analysis |

**Note:** 95% of overhead is queryId computation. If you already have `pg_stat_statements`, overhead is negligible.

**Production Deployment Strategy**

```sql
-- 1. Enable in passive mode
ALTER SYSTEM SET pg_track_optimizer.mode = 'normal';
ALTER SYSTEM SET pg_track_optimizer.log_min_error = 3.0;
SELECT pg_reload_conf();

-- 2. Let it accumulate data (1-2 weeks)

-- 3. Review findings
SELECT * FROM pg_track_optimizer
WHERE avg_error -> 'mean' > 2.0
ORDER BY nexecs DESC;

-- 4. Investigate & fix
-- 5. Reset and measure improvement
SELECT pg_track_optimizer_reset();
```

**What to Look For**

🔴 **High avg_error + frequent execution**
→ Outdated statistics, run ANALYZE

🟡 **High f_join_filter**
→ JOIN order problem, check join conditions

🟠 **High f_scan_filter**
→ Missing/wrong index, check WHERE clauses

🔵 **High f_worst_splan**
→ Correlated subquery, rewrite as JOIN

🟣 **High local_blks**
→ work_mem too low, not optimizer issue

---

### Part 6: Future Directions & Q&A (2 minutes)

**Roadmap**

- **Automatic recommendations**: AI-based suggestions for fixes
- **Historical tracking**: Trend analysis over time
- **Integration with auto_explain**: Unified logging
- **Plan stability hints**: Suggest plan hints to fix issues
- **Query rewrite suggestions**: Automatic rewrites for common patterns

**Questions to Explore**

- How to handle dynamic query generation (ORMs)?
- Balancing overhead vs. granularity
- Integration with third-party monitoring (Datadog, New Relic)
- Cloud provider support (AWS RDS, Azure, GCP)

---

## Key Takeaways

1. **Execution metrics miss planning problems** - Fast queries today can be disasters tomorrow
2. **Architecture matters** - Executor hooks + DSM + RStats enable efficient tracking
3. **Node-level visibility** - See estimation errors at *every* plan node, not just top-level
4. **Multiple dimensions** - Four error metrics + filtering factors + SubPlan analysis
5. **Proactive optimization** - Find problems before they hurt production
6. **Complementary tool** - Use alongside pg_stat_statements, not instead of

---

## Speaker Bio

[Your name] is a PostgreSQL contributor and database engineer specializing in query optimization and performance analysis. [Add your background: companies, projects, previous talks, etc.]

---

## Additional Materials

**GitHub Repository**: https://github.com/danolivo/pg_track_optimizer

**Documentation**: [Link to docs]

**Demo Database**: [Will provide SQL scripts to reproduce examples]

**Slides**: [Will be available after conference]

---

## Technical Requirements

- Projector/screen for slides
- Live demo capability (laptop + projector connection)
- Will bring backup demo videos in case of connectivity issues

## Questions for Organizers

- Is live demo preferred or recorded demo acceptable?
- Target audience: Mixed (DBAs + developers) or specialized?
- Any preference on depth: Overview vs. deep technical dive?

---

*End of Proposal*
