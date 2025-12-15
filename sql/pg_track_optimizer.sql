CREATE EXTENSION pg_track_optimizer;

-- Cleanup history of previous tests
SELECT * FROM pg_track_optimizer_reset();

-- Test flushing zero hash table
SELECT * FROM pg_track_optimizer_flush();

CREATE TABLE pto_test(x integer, y integer, z integer);
ANALYZE pto_test;

EXPLAIN (COSTS OFF) SELECT * FROM pto_test WHERE x < 1;
SELECT querytext,relative_error>=0,nodes_assessed,nodes_total,exec_time>0,nexecs
FROM pg_track_optimizer()
ORDER BY querytext COLLATE "C"; -- Nothing to track for plain explain.

EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT * FROM pto_test WHERE x < 1;
SELECT querytext,relative_error>=0,nodes_assessed,nodes_total,exec_time>0,nexecs
FROM pg_track_optimizer()
ORDER BY querytext; -- Must see it.
-- TODO: Disable storing of queries, involving the extension UI objects

SELECT * FROM pto_test WHERE x < 1;
-- Must see second execution of the query in nexecs (don't mind EXPLAIN)
SELECT querytext,relative_error>=0,nodes_assessed,nodes_total,exec_time>0,nexecs
FROM pg_track_optimizer()
ORDER BY querytext;

/*
 * Tests for parallel workers.
 */

SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0.0001;
SET parallel_tuple_cost = 0.0000001;
SET min_parallel_table_scan_size = 0;

CREATE TABLE t1 (x numeric) WITH (parallel_workers = 4);
INSERT INTO t1 (x) SELECT random() FROM generate_series(1, 100000);
VACUUM t1;

SET pg_track_optimizer.mode = 'forced';

-- Error must be zero in this case.
-- XXX: Is there a case when number of parallel workers will be less than 4?
EXPLAIN (COSTS OFF, ANALYZE, BUFFERS OFF, TIMING OFF, SUMMARY OFF)
SELECT * FROM t1;

SELECT querytext,relative_error,error2,nodes_assessed,nodes_total,nexecs
FROM pg_track_optimizer() WHERE querytext LIKE '%FROM t1%';

DROP EXTENSION pg_track_optimizer;
