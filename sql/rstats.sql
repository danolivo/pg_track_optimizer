-- Test rstats base type
CREATE EXTENSION pg_track_optimizer;

\set VERBOSITY terse

-- Test 1: Basic rstats creation and initialization
SELECT 42.5::rstats;
SELECT (42.1::double precision)::rstats;
SELECT (NULL::double precision)::rstats;
SELECT rstats(); -- Check empty state
SELECT NULL::rstats -> 'mean';

SELECT rstats('abc'); -- ERROR, rstats_in have a different format
SELECT rstats(0), rstats(-1), rstats(36.6);
SELECT rstats('12'::int2);

SELECT q.s, q.s + 1.0, q.s + 2.0, q.s + NULL
  FROM (VALUES (1.0::rstats)) AS q(s);
SELECT q.s, q.s + 1.0, q.s + NULL FROM (VALUES (NULL::rstats)) AS q(s);
SELECT q.s, q.s + 1.0 FROM
  (VALUES (('+Infinity'::double precision)::rstats)) AS q(s);

-- Test 2: Create table with rstats column
CREATE TABLE sensor_data (
  sensor_id integer,
  measurements rstats
);

-- Test 3: Insert initial rstats values
INSERT INTO sensor_data VALUES
  (1, 10.0::rstats), (2, 20.0::rstats), (3, 15.5::rstats);

-- Test 4: Add values to rstats using the + operator
UPDATE sensor_data SET measurements = measurements + 20.0 WHERE sensor_id = 1;
UPDATE sensor_data SET measurements = measurements + 25.0 WHERE sensor_id = 2;
UPDATE sensor_data SET measurements = measurements + 18.5 WHERE sensor_id = 3;

-- Add more values to build up rstats
UPDATE sensor_data SET measurements = measurements + 30.0 WHERE sensor_id = 1;
UPDATE sensor_data SET measurements = measurements + 22.0 WHERE sensor_id = 2;

-- Test 5: Query rstats properties
-- NOTE: don't forget to stabilise output rounding double variables
SELECT
    sensor_id,
    measurements -> 'count' as count,
    ROUND((measurements -> 'mean')::numeric, 2) as mean,
    measurements -> 'min' as min,
    measurements -> 'max' as max,
    ROUND((measurements -> 'stddev')::numeric, 2) as stddev2,
    ROUND((measurements -> 'stddev')::numeric, 2) as stddev
FROM sensor_data
ORDER BY sensor_id;

-- Test 6: Equality comparison
-- Create two identical rstats
SELECT (10.0::rstats + 20.0 + 30.0) = (10.0::rstats + 20.0 + 30.0) as equal_stats;

-- Create two different rstats
SELECT (10.0::rstats + 20.0) = (15.0::rstats + 15.0) as different_stats;

-- Test 7: Compare rstats in table
INSERT INTO sensor_data VALUES (4, 10.0::rstats + 11.0 + 12.0);
INSERT INTO sensor_data VALUES (5, 10.0::rstats + 11.0 + 12.0);

SELECT s1.sensor_id as sensor1, s2.sensor_id as sensor2,
       s1.measurements = s2.measurements as are_equal
FROM sensor_data s1, sensor_data s2
WHERE s1.sensor_id = 4 AND s2.sensor_id = 5;

-- Test 8: Text representation
SELECT sensor_id, measurements::text
FROM sensor_data
WHERE sensor_id <= 1
ORDER BY sensor_id;

-- Test 10: Field accessor using -> operator
SELECT
    sensor_id,
    measurements -> 'count' as count,
    ROUND((measurements -> 'mean')::numeric, 2) as mean,
    measurements -> 'min' as min,
    measurements -> 'max' as max,
    ROUND((measurements -> 'stddev')::numeric, 2) as stddev2,
    ROUND((measurements -> 'stddev')::numeric, 2) as stddev
FROM sensor_data
WHERE sensor_id <= 3
ORDER BY sensor_id;

-- Test 11: Using -> operator in WHERE clause
SELECT sensor_id, ROUND((measurements -> 'mean')::numeric, 2) as mean
FROM sensor_data
WHERE (measurements -> 'mean') > 15
ORDER BY sensor_id;

-- Test 12: Invalid field name (should error)
-- SELECT measurements -> 'invalid_field' FROM sensor_data LIMIT 1;

-- Test 13: Sentinel Value Validation (from rstats.md Testing Recommendations)

-- Test 13.1: Valid empty state - should succeed
SELECT '(count:0,mean:0,min:0,max:0,stddev:0)'::rstats;

-- Test 13.2: Reject corrupt empty state - count=0 with non-zero mean
SELECT '(count:0,mean:1,min:0,max:0,stddev:0)'::rstats;

-- Test 13.3: Reject corrupt empty state - count=0 with non-zero min
SELECT '(count:0,mean:0,min:-1,max:0,stddev:0)'::rstats;

-- Test 13.4: Reject corrupt empty state - count=0 with non-zero max
SELECT '(count:0,mean:0,min:0,max:5,stddev:0)'::rstats;

-- Test 13.5: Reject corrupt empty state - count=0 with non-zero stddev
SELECT '(count:0,mean:0,min:0,max:0,stddev:1.5)'::rstats;

-- Test 13.6: Binary round-trip
SELECT rstats()::bytea::rstats;

-- Test 13.7: Init with zero vs. empty - should have different counts
SELECT (rstats(0.0) -> 'count')::int AS init_zero_count,
       (rstats() -> 'count')::int AS empty_count;

-- Test 13.8: Empty state equality - canonical representation
SELECT rstats() = '(count:0,mean:0,min:0,max:0,stddev:0)'::rstats AS empty_equals;

-- Test 13.9: Empty state field access - all should be zero
SELECT
    (rstats() -> 'count')::int as count,
    (rstats() -> 'mean')::numeric as mean,
    (rstats() -> 'min')::numeric as min,
    (rstats() -> 'max')::numeric as max,
    (rstats() -> 'stddev')::numeric as stddev;

-- Test 14: Indexing on RStats fields
-- Demonstrates that expression indexes work on extracted RStats fields

-- Test 14.1: Single-column index on mean for range queries
CREATE INDEX sd_idx_mean ON sensor_data ((measurements -> 'mean'));

SET enable_seqscan = 'off';

-- Should use index scan for range query
EXPLAIN (COSTS OFF) SELECT sensor_id, measurements -> 'mean' as mean
FROM sensor_data
WHERE measurements -> 'mean' > 15
ORDER BY measurements -> 'mean';

RESET enable_seqscan;
DROP INDEX sd_idx_mean;

-- Test 14.2: Multi-column index for compound queries
-- Index column order: (count, mean) allows filtering by count and sorting by mean
CREATE INDEX sd_idx_compound
  ON sensor_data ((measurements -> 'count'), (measurements -> 'mean'));

SET enable_seqscan = 'off';

-- Should use index for WHERE clause (but may still need Sort for ORDER BY)
EXPLAIN (COSTS OFF) SELECT sensor_id FROM sensor_data
WHERE measurements -> 'count' > 0
ORDER BY measurements -> 'mean';

RESET enable_seqscan;
DROP INDEX sd_idx_compound;

-- Test 14.3: Index for statistics-based filtering
-- Useful for finding queries with high stddev or outliers
CREATE INDEX sd_idx_stddev ON sensor_data ((measurements -> 'stddev'));

SET enable_seqscan = 'off';

-- Find measurements with high stddev
EXPLAIN (COSTS OFF) SELECT sensor_id
FROM sensor_data
WHERE measurements -> 'stddev' > 5;

RESET enable_seqscan;
DROP INDEX sd_idx_stddev;

-- Next step: can we forbid such an update somehow? We only should allow
-- explicit initialization and INSERT of a new value.
CREATE TEMP TABLE tmp AS (SELECT * FROM sensor_data);
UPDATE sensor_data SET measurements = rstats();
SELECT * FROM sensor_data;

UPDATE sensor_data s SET measurements = v.measurements FROM tmp v
WHERE v.sensor_id = s.sensor_id;
SELECT * FROM sensor_data;

-- Test 15: rstats_agg aggregate function
CREATE TABLE agg_test (grp int, val double precision);
INSERT INTO agg_test VALUES (1, 10), (1, 20), (1, 30);
INSERT INTO agg_test VALUES (2, 5), (2, 15);
INSERT INTO agg_test VALUES (3, 100);

-- Basic aggregation by group
SELECT grp, rstats_agg(val) FROM agg_test GROUP BY grp ORDER BY grp;

-- Verify statistics are correct
SELECT grp,
       (rstats_agg(val) -> 'count')::int as count,
       (rstats_agg(val) -> 'mean')::numeric as mean,
       (rstats_agg(val) -> 'min')::numeric as min,
       (rstats_agg(val) -> 'max')::numeric as max
FROM agg_test GROUP BY grp ORDER BY grp;

-- Test with NULL values (should be skipped)
INSERT INTO agg_test VALUES (4, NULL), (4, 50), (4, NULL), (4, 100);
SELECT grp, rstats_agg(val) FROM agg_test WHERE grp = 4 GROUP BY grp;

-- Test aggregation without GROUP BY
SELECT rstats_agg(val) FROM agg_test WHERE grp <= 2;

DROP TABLE agg_test;

-- Distance operator <-> tests
-- The <-> operator calculates Mahalanobis distance between two distributions

-- Identical distributions should have distance 0
SELECT ROUND((
    (rstats_agg(val) <-> rstats_agg(val))::numeric
), 2) as identical_distance
FROM (VALUES (10.0), (20.0), (30.0)) AS t(val);

-- Very similar distributions (small distance)
-- Distribution 1: mean=20, values around 20
-- Distribution 2: mean=21, values around 21 (slightly shifted)
WITH dist1 AS (
    SELECT rstats_agg(val) as stats
    FROM (VALUES (18.0), (20.0), (22.0), (19.0), (21.0)) AS t(val)
),
dist2 AS (
    SELECT rstats_agg(val) as stats
    FROM (VALUES (19.0), (21.0), (23.0), (20.0), (22.0)) AS t(val)
)
SELECT ROUND((d1.stats <-> d2.stats)::numeric, 4) as similar_distance
FROM dist1 d1, dist2 d2;

-- Very different distributions (large distance)
-- Distribution 1: mean=10, low values
-- Distribution 2: mean=100, high values
WITH dist1 AS (
    SELECT rstats_agg(val) as stats
    FROM (VALUES (8.0), (10.0), (12.0), (9.0), (11.0)) AS t(val)
),
dist2 AS (
    SELECT rstats_agg(val) as stats
    FROM (VALUES (98.0), (100.0), (102.0), (99.0), (101.0)) AS t(val)
)
SELECT ROUND((d1.stats <-> d2.stats)::numeric, 2) as different_distance
FROM dist1 d1, dist2 d2;

-- Distributions with different variances
-- Distribution 1: tight (low variance)
-- Distribution 2: spread (high variance)
WITH tight_dist AS (
    SELECT rstats_agg(val) as stats
    FROM (VALUES (50.0), (50.5), (49.5), (50.2), (49.8)) AS t(val)
),
spread_dist AS (
    SELECT rstats_agg(val) as stats
    FROM (VALUES (30.0), (50.0), (70.0), (40.0), (60.0)) AS t(val)
)
SELECT ROUND((t.stats <-> s.stats)::numeric, 4) as variance_distance
FROM tight_dist t, spread_dist s;

-- Edge case - insufficient samples (count < 2)
-- Should return INFINITY
SELECT (rstats(42.0) <-> rstats(50.0)) as insufficient_samples;

-- Edge case - constant distributions (zero variance)
-- Same constant: distance = 0
-- Different constants: distance = INFINITY
SELECT
    ROUND(((rstats(10.0) + 10.0 + 10.0) <-> (rstats(10.0) + 10.0 + 10.0))::numeric, 2)
        as same_constant,
    ((rstats(10.0) + 10.0 + 10.0) <-> (rstats(20.0) + 20.0 + 20.0))
        as different_constants;

-- Real-world scenario - comparing query execution statistics
-- Simulate two executions of similar queries with slightly different characteristics
CREATE TABLE query_stats (
    query_id int,
    execution_time double precision
);

INSERT INTO query_stats VALUES
    (1, 95.0), (1, 100.0), (1, 105.0), (1, 98.0), (1, 102.0),
    (1, 99.0), (1, 101.0), (1, 97.0), (1, 103.0), (1, 100.0);

INSERT INTO query_stats VALUES
    (2, 105.0), (2, 110.0), (2, 115.0), (2, 108.0), (2, 112.0),
    (2, 109.0), (2, 111.0), (2, 107.0), (2, 113.0), (2, 110.0);

INSERT INTO query_stats VALUES
    (3, 480.0), (3, 500.0), (3, 520.0), (3, 490.0), (3, 510.0),
    (3, 495.0), (3, 505.0), (3, 485.0), (3, 515.0), (3, 500.0);

-- Compare execution time distributions
SELECT
    q1.query_id as query1,
    q2.query_id as query2,
    ROUND((rstats_agg(q1.execution_time) <-> rstats_agg(q2.execution_time))::numeric, 4)
        as distance
FROM query_stats q1, query_stats q2
WHERE q1.query_id < q2.query_id
GROUP BY q1.query_id, q2.query_id
ORDER BY q1.query_id, q2.query_id;

-- Using distance for finding similar query patterns
-- Find queries with similar execution time patterns (distance < 5.0)
WITH stats_by_query AS (
    SELECT query_id, rstats_agg(execution_time) as stats
    FROM query_stats
    GROUP BY query_id
)
SELECT
    s1.query_id as query1,
    s2.query_id as query2,
    ROUND((s1.stats <-> s2.stats)::numeric, 4) as distance
FROM stats_by_query s1, stats_by_query s2
WHERE s1.query_id < s2.query_id
  AND (s1.stats <-> s2.stats) < 5.0
ORDER BY distance;

DROP TABLE query_stats;

-- Clean up
DROP TABLE sensor_data,tmp;
SELECT * FROM pg_track_optimizer_reset();
DROP EXTENSION pg_track_optimizer;
