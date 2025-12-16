CREATE EXTENSION pg_track_optimizer;

-- Cleanup history
SELECT * FROM pg_track_optimizer_reset();

SET pg_track_optimizer.mode = 'forced';

/*
 * Test that filtered tuples from join nodes are counted correctly.
 * Join nodes can produce nfiltered1 values when they have join quals
 * that filter tuples (e.g., additional conditions beyond the hash/merge key).
 */

-- Create test tables
CREATE TABLE join_outer (
    id INTEGER,
    val INTEGER
);

CREATE TABLE join_inner (
    id INTEGER,
    val INTEGER
);

-- Insert data
INSERT INTO join_outer SELECT i, i % 10 FROM generate_series(1, 1000) i;
INSERT INTO join_inner SELECT i, i % 10 FROM generate_series(1, 100) i;

ANALYZE join_outer;
ANALYZE join_inner;

-- Force a nested loop join with a filter condition
-- The condition "join_outer.val = join_inner.val" will cause filtering
-- at the join level, producing nfiltered1 values
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;

-- This query will produce a NestLoop with join filtering
-- The join condition includes both id match and val match,
-- where val match acts as a filter
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT * FROM join_outer
JOIN join_inner ON join_outer.id = join_inner.id
WHERE join_outer.val + join_inner.val > 5;

-- Check that we tracked the query
-- The nodes_assessed should include the join node with filtered tuples
SELECT
  querytext,
  ROUND(relative_error::numeric,1) AS error,
  nodes_assessed,
  nodes_total,
  nexecs
FROM pg_track_optimizer()
WHERE querytext LIKE '%FROM join_outer%';

-- Cleanup
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

DROP EXTENSION pg_track_optimizer;
