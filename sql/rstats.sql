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
    ROUND((measurements -> 'variance')::numeric, 2) as variance,
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
    ROUND((measurements -> 'variance')::numeric, 2) as variance,
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
SELECT '(count:0,mean:0,min:0,max:0,variance:0)'::rstats;

-- Test 13.2: Reject corrupt empty state - count=0 with non-zero mean
SELECT '(count:0,mean:1,min:0,max:0,variance:0)'::rstats;

-- Test 13.3: Reject corrupt empty state - count=0 with non-zero min
SELECT '(count:0,mean:0,min:-1,max:0,variance:0)'::rstats;

-- Test 13.4: Reject corrupt empty state - count=0 with non-zero max
SELECT '(count:0,mean:0,min:0,max:5,variance:0)'::rstats;

-- Test 13.5: Reject corrupt empty state - count=0 with non-zero variance
SELECT '(count:0,mean:0,min:0,max:0,variance:1.5)'::rstats;

-- Test 13.6: Binary round-trip
SELECT rstats()::bytea::rstats;

-- Test 13.7: Init with zero vs. empty - should have different counts
SELECT (rstats(0.0) -> 'count')::int AS init_zero_count,
       (rstats() -> 'count')::int AS empty_count;

-- Test 13.8: Empty state equality - canonical representation
SELECT rstats() = '(count:0,mean:0,min:0,max:0,variance:0)'::rstats AS empty_equals;

-- Test 13.9: Empty state field access - all should be zero
SELECT
    (rstats() -> 'count')::int as count,
    (rstats() -> 'mean')::numeric as mean,
    (rstats() -> 'min')::numeric as min,
    (rstats() -> 'max')::numeric as max,
    (rstats() -> 'variance')::numeric as variance;

-- Clean up
DROP TABLE sensor_data;
DROP EXTENSION pg_track_optimizer;
