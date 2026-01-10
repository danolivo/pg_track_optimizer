# PGConf.dev 2026 - Talk Proposal

## Title
pg_track_optimizer: Tracking Cardinality Estimation Errors in Production

## Format
Talk (45 minutes)

## Track
Performance & Internals

## Level
Intermediate

---

## Abstract

PostgreSQL's query planner relies heavily on cardinality estimates to choose execution plans. When these estimates are significantly wrong—due to outdated statistics, column correlation, or complex predicates—the planner may choose suboptimal plans. However, identifying which queries suffer from poor estimates is difficult in production environments with thousands of unique queries.

Existing tools like pg_stat_statements track execution metrics (runtime, I/O, CPU), but don't expose the gap between estimated and actual row counts at the plan node level. A query that runs quickly today may carry a 100× estimation error that becomes a performance problem when data distribution changes.

This talk presents **pg_track_optimizer**, an extension that tracks cardinality estimation accuracy by comparing planner predictions against actual execution statistics for every plan node. The extension uses PostgreSQL's executor hooks to instrument query execution and stores per-query error metrics in dynamic shared memory.

**Technical topics covered:**

**Architecture & Implementation**
- Integration with ExecutorStart/ExecutorEnd hooks for instrumentation
- Plan tree walking with planstate_tree_walker() to analyze every node
- Dynamic shared memory (DSM) and dshash for scalable query tracking
- Custom base type (RStats) implementing Welford's algorithm for incremental statistics
- Handling parallel workers and never-executed plan branches

**Beyond pg_stat_statements**
- Why execution metrics don't capture planning problems
- Node-level vs. query-level tracking
- Complementary deployment strategies
- Performance overhead analysis (<2% measured)

**Error Metrics & Detection**
- Logarithmic error calculation: |log(actual_rows / estimated_rows)|
- Four aggregation methods: simple average, RMS, time-weighted, cost-weighted
- Additional indicators: JOIN/scan filtering overhead, SubPlan execution patterns
- Identifying optimization opportunities vs. work_mem issues

**Production Considerations**
- Shared memory sizing and hash table management
- Concurrent access patterns and atomic operations
- Query text storage in DSA (Dynamic Shared Areas)
- Interaction with auto_explain and compute_query_id
- Optional disk persistence and statistics reset

The extension is open source and compatible with PostgreSQL 17+. I'll demonstrate how to deploy it in production environments to proactively identify queries that need ANALYZE, extended statistics, or query rewrites—before they cause outages.

---

## Target Audience

DBAs and performance engineers working with production PostgreSQL systems. Familiarity with EXPLAIN output and basic query planning concepts assumed. Some knowledge of PostgreSQL internals helpful but not required.

---

## Talk Outline

1. Problem: Why cardinality estimation matters (5 min)
2. Implementation: Executor hooks, DSM, RStats type (15 min)
3. Comparison with pg_stat_statements (5 min)
4. Error metrics and interpretation (10 min)
5. Production deployment experience (7 min)
6. Q&A (3 min)

---

## About the Speaker

Andrei Lepikhov - PostgreSQL contributor, query optimization specialist

[Add affiliations, previous PostgreSQL work, mailing list contributions, etc.]

---

## Links

- GitHub: https://github.com/danolivo/pg_track_optimizer
- Compatible: PostgreSQL 17+
- License: MIT
