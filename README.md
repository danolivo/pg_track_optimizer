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

Four error metrics are computed:
- **avg_error**: Simple average across all plan nodes
- **rms_error**: Root Mean Square (RMS) - emphasises large errors
- **twa_error**: Time-Weighted Average (TWA) - highlights errors in time-consuming nodes
- **wca_error**: Cost-Weighted Average (WCA) - highlights errors in high-cost nodes according to the planner

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
    query,
    avg_error,
    rms_error,
    twa_error,
    wca_error,
    evaluated_nodes,
    plan_nodes,
    exec_time,
    nexecs
FROM pg_track_optimizer()
ORDER BY avg_error DESC
LIMIT 10;
```

**Example output:**
```
                    query                         | avg_error | rms_error | twa_error | wca_error | evaluated_nodes | nexecs
--------------------------------------------------+-----------+-----------+-----------+-----------+-----------------+--------
 SELECT * FROM orders WHERE customer_id = $1      |      4.23 |      4.89 |      4.56 |      3.87 |               5 |    142
 SELECT COUNT(*) FROM products WHERE category...  |      3.87 |      4.12 |      3.95 |      4.21 |               3 |     23
```

### Column Descriptions

- **query**: The SQL query (normalised, with literals replaced by `$1`, `$2`, etc.)
- **avg_error**: Simple average of log-scale errors across plan nodes
- **rms_error**: Root Mean Square (RMS) error - emphasises large estimation errors
- **twa_error**: Time-Weighted Average (TWA) error - highlights estimation errors in time-consuming nodes
- **wca_error**: Cost-Weighted Average (WCA) error - highlights estimation errors in nodes the planner considered expensive
- **evaluated_nodes**: Number of plan nodes analysed
- **plan_nodes**: Total plan nodes (some may be skipped, e.g., never-executed branches)
- **exec_time**: Total execution time across all executions (milliseconds). Divide by `nexecs` to get average per execution
- **nexecs**: Number of times the query was executed
- **blks_accessed**: Total number of blocks accessed (sum of shared, local, and temporary blocks hit, read, and written) across all executions

### Managing Statistics

```sql
-- Save current statistics to disk
SELECT pg_track_optimizer_flush();

-- Clear all tracked statistics
SELECT pg_track_optimizer_reset();
```

Statistics persist in shared memory until `pg_track_optimizer_reset()` is called or PostgreSQL restarts. Use `pg_track_optimizer_flush()` to save snapshots for historical analysis.

## Interpreting Results

### High `avg_error`
Indicates consistent estimation errors across the plan. Possible causes:
- **Outdated statistics**: Run `ANALYZE` on affected tables
- **Data correlation**: Planner assumes independence between columns
- **Complex predicates**: Non-linear filters that statistics can't capture

### High `rms_error`
Suggests a few plan nodes with very large errors. Often indicates:
- **Wrong join order**: Planner underestimated join result size
- **Poor index choice**: Estimated selectivity was far off
- **Partition pruning failure**: Planner scanned more partitions than needed

### High `twa_error`
Shows that estimation errors occurred in the most time-consuming parts of the plan:
- **Sequential scans with wrong row estimates**: Should have used an index
- **Nested loops with underestimated inner rows**: Should have used hash join
- **Sorts with wrong cardinality**: Allocated insufficient work_mem

### High `wca_error`
Indicates estimation errors in nodes the planner considered expensive. This can reveal:
- **Misalignment between cost model and reality**: Planner's cost estimates don't match actual execution patterns
- **Index vs sequential scan decisions**: Wrong cost estimates led to poor access method choices
- **Join method selection**: Planner overestimated cost of certain join types

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
    avg_error,
    nexecs,
    exec_time / nexecs AS avg_time_ms
FROM pg_track_optimizer()
WHERE avg_error > 2.0
ORDER BY exec_time DESC;
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
