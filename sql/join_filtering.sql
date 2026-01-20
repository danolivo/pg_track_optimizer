create or replace function portable_explain_analyze(query text)
returns table (out_line text) language plpgsql
as
$$
declare
  line text;
begin
  for line in
    execute 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)' || query
  loop
    out_line := regexp_replace(line, '\d+kB', 'NNkB', 'g');
    out_line := regexp_replace(out_line, 'rows=(\d+)\.00', 'rows=\1', 'g');
    return next;
  end loop;
end;
$$;

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
SELECT portable_explain_analyze('
SELECT * FROM join_outer
JOIN join_inner ON join_outer.id = join_inner.id
WHERE join_outer.val + join_inner.val > 5;
');

-- Check that we tracked the query
-- The evaluated_nodes should include the join node with filtered tuples
SELECT
  query,
  ROUND((avg_error -> 'mean')::numeric,1) AS error,
  evaluated_nodes,
  plan_nodes,
  nexecs
FROM pg_track_optimizer()
WHERE query LIKE '%FROM join_outer%' AND
  query NOT LIKE '%portable_explain_analyze%' AND
  query NOT LIKE '%pg_track_optimizer%';

-- Cleanup
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

/*
 * Just for the demo:
 * XXX: How to detect such cases: we hash or sort the whole source but use only
 * minor part of it - no filters, just earlier stop or low usage of a hash table
 */

-- First pass: no signs of filtered tuples or estimation errors
SELECT portable_explain_analyze('SELECT * FROM join_inner JOIN join_outer USING (id);');

SELECT
  ROUND((avg_avg::numeric), 2) AS err,
  floor(jf_max) AS jf_max, floor(lf_max) AS lf_max,
  evaluated_nodes en, plan_nodes pn, nexecs nex
FROM pg_track_optimizer
WHERE query LIKE '%join_inner JOIN join_outer USING (id)%' AND
  query NOT LIKE '%portable_explain_analyze%' AND
  query NOT LIKE '%pg_track_optimizer%';

SELECT portable_explain_analyze('SELECT * FROM join_inner JOIN join_outer USING (id) LIMIT 1;');

SELECT
  ROUND((avg_avg::numeric), 2) AS err,
  floor(jf_max) AS jf_max, floor(lf_max) AS lf_max,
  evaluated_nodes en, plan_nodes pn, nexecs nex
FROM pg_track_optimizer
WHERE query LIKE '%join_inner JOIN join_outer USING (id)%' AND
  query NOT LIKE '%portable_explain_analyze%' AND
  query NOT LIKE '%pg_track_optimizer%';

CREATE INDEX join_outer_id_idx ON join_outer (id);
-- Second pass: only MergeJoin runtime optimisation detects an estimation error
SELECT portable_explain_analyze('SELECT * FROM join_inner JOIN join_outer USING (id);');

SELECT
  ROUND((avg_avg::numeric), 2) AS err,
  floor(jf_max) AS jf_max, floor(lf_max) AS lf_max,
  evaluated_nodes en,
  plan_nodes pn,
  nexecs nex
FROM pg_track_optimizer
WHERE query LIKE '%join_inner JOIN join_outer USING (id)%' AND
  query NOT LIKE '%portable_explain_analyze%' AND
  query NOT LIKE '%pg_track_optimizer%';

DROP INDEX join_outer_id_idx;
TRUNCATE join_inner;
INSERT INTO join_inner SELECT i, i % 10 FROM generate_series(-50, 50) i;
VACUUM ANALYZE join_inner;

SET enable_mergejoin = off;
SET enable_material = off;

SELECT portable_explain_analyze('SELECT * FROM join_inner JOIN join_outer USING (id);');

SET enable_hashjoin = off;

SELECT portable_explain_analyze('SELECT * FROM join_inner JOIN join_outer USING (id);');

RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

DROP FUNCTION portable_explain_analyze;
DROP TABLE join_inner,join_outer;
SELECT * FROM pg_track_optimizer_reset();
DROP EXTENSION pg_track_optimizer;
