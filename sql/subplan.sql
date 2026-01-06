CREATE EXTENSION pg_track_optimizer;

-- Cleanup history
SELECT * FROM pg_track_optimizer_reset();

SET pg_track_optimizer.mode = 'forced';

/*
 * Test Subplan evaluation inside JOIN clause.
 * The Subplan should appear in the second part of an AND clause
 * with number of loops > 10.
 */

-- Create test tables
CREATE TABLE outer_table (
  id       INTEGER,
  category INTEGER,
  val      INTEGER
);

CREATE TABLE inner_table (
  id  INTEGER,
  val INTEGER
);

CREATE TABLE reference_table (
  category  INTEGER,
  threshold INTEGER
);

-- Insert data to ensure > 10 loops
-- outer_table: 20 rows across 5 categories
INSERT INTO outer_table
SELECT i, (i % 5) + 1, i * 10
FROM generate_series(1, 20) i;

-- inner_table: matching ids
INSERT INTO inner_table
SELECT i, i * 5
FROM generate_series(1, 20) i;

-- reference_table: thresholds for each category
INSERT INTO reference_table VALUES
  (1, 50), (2, 100), (3, 150), (4, 200), (5, 250);

VACUUM ANALYZE outer_table, inner_table, reference_table;

-- Force nested loop to ensure the SubPlan executes multiple times
SET enable_hashjoin = off;
SET enable_mergejoin = off;

/*
 * This query demonstrates:
 * 1. JOIN with two-part AND condition
 * 2. Second part contains a correlated subquery (becomes SubPlan)
 * 3. SubPlan evaluates multiple times for each outer row that passes first
 * condition
 */
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT o.id, o.category, o.val, i.val as inner_val
FROM outer_table o
JOIN inner_table i ON
  o.id = i.id OR
  o.val > (
    SELECT threshold
    FROM reference_table r
    WHERE r.category = o.category
  );

-- Verify the SubPlan executed multiple times
-- The plan should show "SubPlan" with loops > 10
SELECT
  ROUND((avg_error -> 'mean')::numeric, 2) AS error,
  ROUND((max_jfiltered -> 'mean')::numeric, 2) AS jf,
  ROUND((max_lfiltered -> 'mean')::numeric, 2) AS lf,
  ROUND((worst_splan_factor -> 'mean')::numeric, 2) AS sp_factor,
  evaluated_nodes,
  plan_nodes,
  nexecs
FROM pg_track_optimizer()
WHERE query LIKE '%FROM outer_table%';

/*
 * Don't care for now about parallel query plan for now - upstream doesn't
 * support this feature yet. But this test should fail if they implement it and
 * we would have to fix it too.
 */
RESET enable_hashjoin;
RESET enable_mergejoin;

ALTER TABLE outer_table SET (parallel_workers = 4);
ALTER TABLE inner_table SET (parallel_workers = 4);
ALTER TABLE reference_table SET (parallel_workers = 4);

SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0.0001;
SET parallel_tuple_cost = 0.0000001;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET enable_material = 'off';

EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT o.id, o.category, o.val, i.val as inner_val
FROM outer_table o
JOIN inner_table i ON
  o.id = i.id OR
  o.val > (
    SELECT threshold
    FROM reference_table r
    WHERE r.category = o.category
  );

RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET enable_material;

-- Cleanup
DROP TABLE outer_table, inner_table, reference_table;

DROP EXTENSION pg_track_optimizer;
