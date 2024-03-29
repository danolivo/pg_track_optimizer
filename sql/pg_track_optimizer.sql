CREATE EXTENSION pg_track_optimizer;

-- Cleanup history of previous tests
SELECT * FROM pg_track_optimizer_reset();

-- Test flushing zero hash table
SELECT * FROM pg_track_optimizer_flush();

CREATE TABLE pto_test(x integer, y integer, z integer);
ANALYZE pto_test;

EXPLAIN (COSTS OFF) SELECT * FROM pto_test WHERE x < 1;
SELECT querytext,relative_error>=0,nodes_assessed,nodes_total,exec_time>0,nexecs
FROM pg_track_optimizer(); -- Nothing to track for plain explain.

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

DROP EXTENSION pg_track_optimizer;
