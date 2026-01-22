# SubPlan Detection and Cost Tracking

## Overview

The `pg_track_optimizer` extension now detects and tracks expensive SubPlans (correlated subqueries) by calculating a "worst SubPlan factor" metric. This helps identify queries where correlated subqueries execute many times, causing performance issues.

## How It Works

### SubPlans in PostgreSQL

SubPlans are correlated subqueries that appear in expression contexts (WHERE clauses, SELECT lists, etc.). Unlike regular plan nodes, SubPlans:

- Are not direct children in the plan tree
- Execute once per outer row (stored in `pstate->subPlan` list)
- Have their effective cost multiplied by the number of executions

### Detection Algorithm

When walking the plan tree in `prediction_walker()` (plan_error.c:35-77):

1. After recursively walking child nodes, check if the current node has SubPlans: `if (pstate->subPlan != NIL)`

2. For each SubPlan attached to this node:
   - Get instrumentation data: `instr = sps->planstate->instrument`
   - Get execution loops: `nloops = instr->nloops`
   - Get planned cost: `cost = sps->planstate->plan->total_cost`
   - Calculate effective cost: `nloops × cost`

3. Track the worst (maximum) SubPlan factor across all SubPlans in the query

### Cost Factor Calculation

```c
f_worst_splan = max(nloops × total_cost)
```

This metric represents:
- **nloops**: How many times the SubPlan executed (once per outer row)
- **total_cost**: PostgreSQL's estimated cost for the SubPlan
- **Product**: The actual cost burden of the SubPlan on query performance

## Database Schema

The new field appears in three places:

### 1. C Structure (pg_track_optimizer.c:110)

```c
RStats f_worst_splan; /* Worst SubPlan cost factor (nloops × cost) */
```

### 2. SQL Function (pg_track_optimizer--0.1.sql:173)

```sql
OUT f_worst_splan rstats
```

### 3. View Columns (pg_track_optimizer--0.1.sql:234-237)

```sql
t.f_worst_splan -> 'min' AS sp_min,
t.f_worst_splan -> 'max' AS sp_max,
t.f_worst_splan -> 'count' AS sp_cnt,
t.f_worst_splan -> 'mean' AS sp_avg,
t.f_worst_splan -> 'stddev' AS sp_dev
```

## Usage Examples

### Example 1: Find Queries with Expensive SubPlans

```sql
SELECT
  queryid,
  LEFT(query, 50) AS query_preview,
  ROUND((f_worst_splan -> 'mean')::numeric, 2) AS avg_sp_factor,
  ROUND((f_worst_splan -> 'max')::numeric, 2) AS max_sp_factor,
  nexecs
FROM pg_track_optimizer()
WHERE (f_worst_splan -> 'mean')::numeric > 1000
ORDER BY (f_worst_splan -> 'mean')::numeric DESC
LIMIT 10;
```

This finds queries where SubPlans have an average cost factor over 1000, indicating they execute many times with significant cost.

### Example 2: Compare SubPlan Cost to Total Execution Time

```sql
SELECT
  queryid,
  ROUND((f_worst_splan -> 'mean')::numeric, 2) AS sp_factor,
  ROUND((exec_time -> 'mean')::numeric, 2) AS avg_time_ms,
  nexecs
FROM pg_track_optimizer()
WHERE (f_worst_splan -> 'mean')::numeric > 0
ORDER BY (f_worst_splan -> 'mean')::numeric DESC;
```

Queries with high `sp_factor` relative to `exec_time` suggest the SubPlan dominates execution time.

### Example 3: Detect SubPlan Loops from Test

From `sql/subplan.sql` regression test:

```sql
SELECT
  ROUND((avg_error -> 'mean')::numeric, 2) AS error,
  ROUND((f_join_filter -> 'mean')::numeric, 2) AS jf,
  ROUND((f_scan_filter -> 'mean')::numeric, 2) AS lf,
  ROUND((f_worst_splan -> 'mean')::numeric, 2) AS sp_factor,
  evaluated_nodes,
  plan_nodes,
  nexecs
FROM pg_track_optimizer()
WHERE query LIKE '%FROM outer_table%';
```

Expected output shows `sp_factor` of ~1900 for a SubPlan that executed 380 loops.

## Interpreting Results

### Low Values (< 100)

SubPlans are not a significant performance concern. The correlated subquery either:
- Executes few times (low nloops)
- Has low individual cost

### Medium Values (100 - 1000)

SubPlans contribute measurably to query cost. Consider:
- Can the subquery be rewritten as a JOIN?
- Is the SubPlan indexed properly?
- Could LATERAL join help?

### High Values (> 1000)

SubPlans are likely a major performance bottleneck. Optimization strategies:

1. **Rewrite as JOIN**:
   ```sql
   -- Before (SubPlan)
   SELECT * FROM orders o
   WHERE total > (SELECT AVG(total) FROM orders WHERE customer_id = o.customer_id);

   -- After (JOIN)
   SELECT o.* FROM orders o
   JOIN (SELECT customer_id, AVG(total) as avg_total FROM orders GROUP BY customer_id) avg
   ON o.customer_id = avg.customer_id
   WHERE o.total > avg.avg_total;
   ```

2. **Use LATERAL join** (PostgreSQL 9.3+):
   ```sql
   SELECT * FROM outer_table o
   JOIN LATERAL (
     SELECT threshold FROM reference_table r
     WHERE r.category = o.category
   ) sub ON o.val > sub.threshold;
   ```

3. **Add indexes** to SubPlan's inner table scan

4. **Materialize subquery** if results are reused:
   ```sql
   WITH thresholds AS MATERIALIZED (
     SELECT category, threshold FROM reference_table
   )
   SELECT * FROM outer_table o
   JOIN thresholds t ON o.category = t.category
   WHERE o.val > t.threshold;
   ```

## Implementation Details

### Key Code Locations

- **Detection**: `plan_error.c:35-77` - Iterates through `pstate->subPlan` list
- **Context**: `plan_error.h:59` - Field in `PlanEstimatorContext`
- **Storage**: `pg_track_optimizer.c:110` - Field in `DSMOptimizerTrackerEntry`
- **Initialization**: `plan_error.c:312` - Set to 0.0 before tree walk
- **Accumulation**: `pg_track_optimizer.c:447-448` - Add value to running stats

### Special Cases

1. **InitPlans**: SubPlans with `setParam != NIL` execute once before the main query. These are not currently tracked separately but would show `nloops = 1`.

2. **Parallel Workers**: Current implementation asserts `sps->worker_instrument == NULL`, meaning SubPlans don't parallelize (as expected - they're parallel-restricted).

3. **No SubPlans**: Queries without SubPlans have `f_worst_splan = 0.0`.

## Testing

The `sql/subplan.sql` regression test demonstrates SubPlan detection:

- Creates tables with correlated subquery in JOIN clause
- SubPlan executes 380 times (shown in EXPLAIN)
- `f_worst_splan` captures the cost burden
- Test validates the metric appears in output

Run the test:
```bash
make installcheck REGRESS=subplan
```

## Future Enhancements

Potential improvements to SubPlan tracking:

1. **Track SubPlan depth**: Nested SubPlans (SubPlans within SubPlans)
2. **Separate InitPlan stats**: Different metrics for once-executed InitPlans
3. **Per-SubPlan breakdown**: Track multiple SubPlans individually, not just worst
4. **Correlation type**: Distinguish EXISTS, ANY, ALL, scalar subqueries
5. **Execution time**: Track actual SubPlan execution time, not just cost estimate

## References

- PostgreSQL SubPlan documentation: https://www.postgresql.org/docs/current/using-explain.html
- Correlated subquery optimization: https://wiki.postgresql.org/wiki/Subquery_scan_removal
- Parallel query limitations: https://www.postgresql.org/docs/current/parallel-safety.html
