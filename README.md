# pg_track_optimizer

[![PostgreSQL Extension Tests](https://github.com/danolivo/pg_track_optimizer/actions/workflows/test.yml/badge.svg)](https://github.com/danolivo/pg_track_optimizer/actions/workflows/test.yml)
[![PostgreSQL Extension Installcheck](https://github.com/danolivo/pg_track_optimizer/actions/workflows/installcheck.yml/badge.svg)](https://github.com/danolivo/pg_track_optimizer/actions/workflows/installcheck.yml)

A lightweight PostgreSQL extension for detecting suboptimal query plans by analysing the gap between planner estimates and actual execution statistics.

**Compatible with PostgreSQL 17 and above.**

## Overview

PostgreSQL's query planner makes sophisticated predictions about row counts, selectivity, and execution costs to choose optimal query plans. However, these predictions may be incorrect due to outdated statistics, data skew, correlation, or complex predicates. As a result, the planner may choose suboptimal execution strategies.

In large databases with thousands of queries, identifying which plans suffer from poor cardinality estimates is like finding a needle in a haystack. **pg_track_optimizer** solves this by automatically tracking the estimation error for every query and surfacing the worst offenders.

## How It Works

The extension hooks into PostgreSQL's executor to compare:
- **Estimated rows** (what the planner predicted)
- **Actual rows** (what execution produced)

For each plan node, it calculates the relative error using logarithmic scale:
```
error = |log(actual_rows / estimated_rows)|
```

Four error metrics are computed (all as rstats cumulative types tracking running statistics across executions):
- **avg_error**: Simple average across all plan nodes
- **rms_error**: Root Mean Square (RMS) - emphasises large errors
- **twa_error**: Time-Weighted Average (TWA) - highlighting errors in time-consuming nodes
- **wca_error**: Cost-Weighted Average (WCA) - highlighting errors in high-cost nodes according to the planner

Queries with high error values are candidates for investigation: missing indexes, outdated statistics, or planner limitations.

## Features

-  **Automatic detection** of queries with poor cardinality estimates
-  **Multiple error metrics** to identify different types of issues
-  **Shared memory tracking** - zero disk overhead during operation
-  **Minimal performance impact** - efficient executor hooks
-  **Query logging** - automatically log EXPLAIN for problematic queries
-  **Persistent storage** - optional flush to disk for long-term analysis
-  **Flexible modes** - track all queries or only problematic ones

## Installation

### Building from Source

```bash
# Clone the repository
git clone https://github.com/danolivo/pg_track_optimizer.git
cd pg_track_optimizer

# Build and install (requires PostgreSQL dev packages)
make USE_PGXS=1
sudo make USE_PGXS=1 install
```

### Loading the Extension

Add to `postgresql.conf`:
```ini
shared_preload_libraries = 'pg_track_optimizer'
```

Restart PostgreSQL, then in your database:
```sql
CREATE EXTENSION pg_track_optimizer;
```

## Configuration

### GUC Parameters

#### `pg_track_optimizer.mode`
Controls when the extension collects statistics.

- **`disabled`** (default): Extension is inactive
- **`normal`**: Track only queries exceeding `log_min_error`
- **`forced`**: Track all queries

```sql
-- Track all queries during debugging
SET pg_track_optimizer.mode = 'forced';

-- Track only problematic queries in production
SET pg_track_optimizer.mode = 'normal';
```

#### `pg_track_optimizer.log_min_error`
Threshold for logging query plans to PostgreSQL log.

```sql
-- Log queries with relative error > 2.0
SET pg_track_optimizer.log_min_error = 2.0;
```

When a query exceeds this threshold in `normal` mode, its EXPLAIN ANALYZE output is written to the PostgreSQL log file.

#### `pg_track_optimizer.hash_mem`
Memory limit (in KB) for the shared memory hash table.

```sql
-- Allow 10MB for query tracking
SET pg_track_optimizer.hash_mem = 10240;
```

## Usage

### Viewing Tracked Queries

```sql
SELECT
    queryid,
    query,
    avg_avg, avg_min, avg_max,  -- avg_error expanded fields
    rms_avg, rms_min, rms_max,  -- rms_error expanded fields
    twa_avg, twa_min, twa_max,  -- twa_error expanded fields
    wca_avg, wca_min, wca_max,  -- wca_error expanded fields
    blks_avg, blks_min, blks_max,  -- blks_accessed expanded fields
    local_avg, local_min, local_max,  -- local_blks expanded fields
    time_avg, time_min, time_max,  -- exec_time expanded fields
    evaluated_nodes,
    plan_nodes,
    nexecs
FROM pg_track_optimizer
ORDER BY avg_avg DESC
LIMIT 10;
```

**Example output:**
```
   queryid   |                    query                         | avg_avg | time_avg | blks_avg | evaluated_nodes | plan_nodes | nexecs
-------------+--------------------------------------------------+---------+----------+----------+-----------------+------------+--------
 42387612345 | SELECT * FROM orders WHERE customer_id = $1      |    4.23 |   152.34 |    28456 |               5 |          7 |    142
 98765432109 | SELECT COUNT(*) FROM products WHERE category...  |    3.87 |    23.41 |     5632 |               3 |          4 |     23
```

### Column Descriptions

| Column | Type | Description |
|--------|------|-------------|
| `queryid` | bigint | Internal PostgreSQL query identifier (same as in pg_stat_statements) |
| `query` | text | The SQL query (normalised, with literals replaced by `$1`, `$2`, etc.) |
| `avg_error` | rstats | Simple average of log-scale errors across plan nodes per execution. Tracks running statistics of average error values across query executions. |
| `rms_error` | rstats | Root Mean Square (RMS) error per execution. Tracks running statistics of RMS error values across query executions. |
| `twa_error` | rstats | Time-Weighted Average (TWA) error per execution. Tracks running statistics of TWA error values across query executions. |
| `wca_error` | rstats | Cost-Weighted Average (WCA) error per execution. Tracks running statistics of WCA error values across query executions. |
| `blks_accessed` | rstats | Running statistics of blocks accessed per execution. |
| `local_blks` | rstats | Running statistics of local blocks (read + written + dirtied) per execution. Local blocks indicate temporary tables or sorts spilling to disk, signaling insufficient work_mem. |
| `exec_time` | rstats | Execution time per query in milliseconds. Tracks running statistics of execution times across query executions. |
| `max_jfiltered` | rstats | Maximum filtered rows (nfiltered1 + nfiltered2) across all JOIN nodes per execution. Tracks running statistics across executions. High values indicate JOINs that filter many tuples, suggesting potential for optimization with parameterized NestLoop or missing indexes. |
| `max_lfiltered` | rstats | Maximum nfiltered1 for leaf nodes (scan nodes) per execution. Tracks running statistics across executions. High values indicate many rows were fetched but filtered out at the scan level, suggesting potential for better indexes or more selective predicates. |
| `evaluated_nodes` | integer | Number of plan nodes analysed |
| `plan_nodes` | integer | Total plan nodes (some may be skipped, e.g., never-executed branches) |
| `nexecs` | bigint | Number of times the query was executed |

> **Note**: The columns `evaluated_nodes`, `plan_nodes`, `exec_time`, `nexecs`, `blks_accessed`, and `local_blks` provide query execution metrics similar to those found in `pg_stat_statements`. These are included directly in `pg_track_optimizer` for user convenience, providing additional criteria for query filtering and analysis without requiring installation of `pg_stat_statements` or other extensions that may introduce additional overhead.

### The RStats Type

The `RStats` type is a custom PostgreSQL type for tracking running statistics using Welford's algorithm for numerical stability (thanks [pg_running_stats](https://github.com/chanukyasds/pg_running_stats) for the idea and coding template). It's used for the `avg_error`, `rms_error`, `twa_error`, `wca_error`, `blks_accessed`, `local_blks`, and `exec_time` columns to provide detailed statistics about all error metrics, block access patterns, local block usage (work_mem indicator), and execution times across multiple query executions.

**Fields accessible via the `->` operator:**
- `count`: Number of observations
- `mean`: Average value
- `variance`: Statistical variance
- `stddev`: Standard deviation
- `min`: Minimum value observed
- `max`: Maximum value observed

**Example usage:**
```sql
-- Get average blocks accessed and WCA error statistics per execution
SELECT queryid,
       wca_error -> 'mean' as avg_wca_error,
       wca_error -> 'stddev' as stddev_wca_error,
       blks_accessed -> 'mean' as avg_blocks,
       blks_accessed -> 'stddev' as stddev_blocks
FROM pg_track_optimizer()
WHERE blks_accessed -> 'mean' > 1000
ORDER BY wca_error -> 'mean' DESC;

-- Use the view to get pre-calculated statistics
SELECT queryid, query,
       wca_avg, wca_dev, wca_min, wca_max,
       blks_avg, blks_dev
FROM pg_track_optimizer
WHERE wca_avg > 2.0
ORDER BY wca_avg DESC;
```

The RStats type maintains numerically stable incremental statistics, automatically updating mean, variance, min, and max as new values are accumulated. This provides richer statistical insight than simple totals or averages, especially useful for understanding the variability in query performance and cardinality estimation across multiple executions.

### Managing Statistics

```sql
-- Save current statistics to disk
SELECT pg_track_optimizer_flush();

-- Clear all tracked statistics
SELECT pg_track_optimizer_reset();
```

Statistics persist in shared memory until `pg_track_optimizer_reset()` is called or PostgreSQL restarts. Use `pg_track_optimizer_flush()` to save snapshots for historical analysis.

## Interpreting Results

### High `avg_avg` (average error)
Indicates consistent estimation errors across the plan. Possible causes:
- **Outdated statistics**: Run `ANALYZE` on affected tables
- **Data correlation**: Planner assumes independence between columns
- **Complex predicates**: Non-linear filters that statistics can't capture

### High `rms_avg` (RMS error)
Suggests a few plan nodes with huge errors.

### High `twa_avg` (time-weighted average error)
Normalises estimation error over execution time. Allows comparing queries with very different execution times.

### High `wca_avg` (cost-weighted average error)
Normalises estimation error over cost. It should help compare the estimation errors of queries with very different structures.

### High `local_avg` (local blocks)
Indicates queries using local blocks, which signal work_mem issues rather than optimisation problems:
- **Insufficient work_mem**: Sorts, hash joins, or aggregates spilling to disk
- **Memory-intensive operations**: Queries with high memory requirements exceeding work_mem
- **Quick fix**: Unlike optimisation errors, this can often be resolved by simply increasing work_mem

## Example Workflow

### 1. Enable Tracking in Production
```sql
ALTER SYSTEM SET pg_track_optimizer.mode = 'normal';
ALTER SYSTEM SET pg_track_optimizer.log_min_error = 3.0;
SELECT pg_reload_conf();
```

### 2. Let It Run
The extension will automatically track queries exceeding the error threshold. Problematic queries will appear in PostgreSQL logs with full EXPLAIN ANALYZE output.

### 3. Review Worst Offenders
```sql
SELECT
    query,
    avg_error -> 'mean' AS avg_avg,
    nexecs,
    exec_time -> 'mean' AS avg_time_ms
FROM pg_track_optimizer()
WHERE avg_error -> 'mean' > 2.0
ORDER BY exec_time -> 'mean' DESC;
```

### 4. Investigate and Fix
- Check if `ANALYZE` is running regularly
- Look for missing indexes
- Consider extended statistics for correlated columns
- Review complex WHERE clauses

### 5. Verify Improvements
```sql
-- Reset statistics
SELECT pg_track_optimizer_reset();

-- Run workload again
-- Re-check error metrics
```

## Understanding Error Metrics

The extension calculates estimation error in a node using logarithmic scale to handle the wide range of row count estimates:

```
error = |log(actual_rows / estimated_rows)|
```

**Why logarithmic?**
- An estimate of 10 when the actual is 100 has the same error magnitude as 100→10
- Prevents massive row counts from dominating the metric
- Aligns with how the planner internally handles costs

**Interpreting values (example classification - specific boundaries may vary for different databases and queries):**
- `error < 1.0`: Estimate within 3x of actual (acceptable)
- `error 1.0-2.0`: Estimate off by 3-7x (investigate if frequent)
- `error 2.0-3.0`: Estimate off by 7-20x (likely problematic)
- `error > 3.0`: Estimate off by 20x+ (requires attention)

## Implementation Notes

- **Leaf node filtering**: Filtered tuples (`nfiltered1`, `nfiltered2`) and heap fetches (`ntuples2`) are included in error calculations only for leaf nodes (scans). This reveals hidden disk I/O costs from massive page fetches that PostgreSQL's planner estimates don't expose. For non-leaf nodes like joins, the planner already estimates both input and output cardinality, so filtered tuples are implicitly visible in the estimate.
- **Never-executed nodes**: Nodes in unexecuted branches (e.g., alternative index paths in conditional plans) are skipped. Without actual execution, we cannot determine how many tuples these subtrees would have produced, even for a single hypothetical call, making error calculation impossible for these paths.
- **Memory overhead**: Large query texts consume shared memory
- **No automatic cleanup**: Statistics must be manually reset or flushed

## Performance Impact

The extension is designed for production use with minimal overhead:
- **Hook overhead**: ~1-2% in `forced` mode, negligible in `normal` mode
- **Memory**: Configurable via `hash_mem`, typically 1-10 MB
- **I/O**: None during operation (only on explicit flush)

**Important note on queryId generation**: Approximately 95% of the overhead comes from queryId computation. If you already have `compute_query_id` enabled (e.g., by using `pg_stat_statements` or other extensions), the additional overhead from pg_track_optimizer becomes nearly undetectable.

In `normal` mode with a reasonable threshold (e.g., 2.0), only a small fraction of queries are tracked, making the overhead virtually undetectable.

## Contributing

Contributions are welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Add tests for new functionality
4. Ensure CI passes
5. Submit a pull request

## Licence

This project is licenced under the MIT Licence. See the LICENCE file for details.
