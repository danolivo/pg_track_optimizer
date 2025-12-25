-- Test flush and load functionality to expose EOF handling bug
-- This test creates queries, flushes them to disk, then reloads to trigger the bug

CREATE EXTENSION pg_track_optimizer;

-- Enable forced tracking mode
SET pg_track_optimizer.mode = 'forced';

-- Cleanup any previous state
SELECT * FROM pg_track_optimizer_reset();

-- Create a test table
CREATE TABLE flush_test(x integer, y integer);
INSERT INTO flush_test SELECT i, i*2 FROM generate_series(1, 100) i;
ANALYZE flush_test;

-- Execute some queries to generate tracking data
-- We need at least one query to trigger the bug
SELECT * FROM flush_test WHERE x < 10;
SELECT * FROM flush_test WHERE y > 50;
SELECT * FROM flush_test WHERE x BETWEEN 20 AND 30;

-- Verify we have tracked data
SELECT
    COUNT(*) as tracked_queries,
    SUM(nexecs) as total_executions
FROM pg_track_optimizer();

-- Flush the data to disk
SELECT * FROM pg_track_optimizer_flush();

-- Now drop and recreate the extension to force a reload from disk
-- This is where the EOF bug will manifest
DROP EXTENSION pg_track_optimizer CASCADE;

-- Recreate extension - this triggers _load_hash_table()
-- In debug builds: Assert(counter == nrecs) will fail
-- In production: Will get read_error due to feof() bug
CREATE EXTENSION pg_track_optimizer;

-- Enable tracking again
SET pg_track_optimizer.mode = 'forced';

-- Verify the data was loaded correctly
-- If the bug exists, this will either fail or show incorrect counts
SELECT
    COUNT(*) as loaded_queries,
    SUM(nexecs) as loaded_executions
FROM pg_track_optimizer();

-- Execute one of the original queries again
SELECT * FROM flush_test WHERE x < 10;

-- Should now have 4 total executions for this query (3 before flush + 1 after)
-- But due to the bug, we might have 0 queries loaded
SELECT
    query,
    nexecs,
    avg_error -> 'count' as error_samples
FROM pg_track_optimizer()
WHERE query LIKE '%flush_test%'
ORDER BY query;

-- Cleanup
SELECT * FROM pg_track_optimizer_reset();
DROP TABLE flush_test;
DROP EXTENSION pg_track_optimizer;
