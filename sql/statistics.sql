-- Test statistics base type
CREATE EXTENSION pg_track_optimizer;

-- Test 1: Basic statistics creation and initialization
SELECT 42.5::statistics;
SELECT (42.1::double precision)::statistics;

-- Test 2: Create table with statistics column
CREATE TABLE sensor_data (
  sensor_id integer,
  measurements statistics
);

-- Test 3: Insert initial statistics values
INSERT INTO sensor_data VALUES
  (1, 10.0::statistics), (2, 20.0::statistics), (3, 15.5::statistics);

-- Test 4: Add values to statistics using the + operator
UPDATE sensor_data SET measurements = measurements + 20.0 WHERE sensor_id = 1;
UPDATE sensor_data SET measurements = measurements + 25.0 WHERE sensor_id = 2;
UPDATE sensor_data SET measurements = measurements + 18.5 WHERE sensor_id = 3;

-- Add more values to build up statistics
UPDATE sensor_data SET measurements = measurements + 30.0 WHERE sensor_id = 1;
UPDATE sensor_data SET measurements = measurements + 22.0 WHERE sensor_id = 2;

-- Test 5: Query statistics properties
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
-- Create two identical statistics
SELECT (10.0::statistics + 20.0 + 30.0) = (10.0::statistics + 20.0 + 30.0) as equal_stats;

-- Create two different statistics
SELECT (10.0::statistics + 20.0) = (15.0::statistics + 15.0) as different_stats;

-- Test 7: Compare statistics in table
INSERT INTO sensor_data VALUES (4, 10.0::statistics + 11.0 + 12.0);
INSERT INTO sensor_data VALUES (5, 10.0::statistics + 11.0 + 12.0);

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

-- Clean up
DROP TABLE sensor_data;
DROP EXTENSION pg_track_optimizer;
