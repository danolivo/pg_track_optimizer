-- Collapsing temporary relation identities in the query id
-- (pg_track_optimizer.queryid_mask_temp_names).
--
-- Generated SQL numbers its temporary tables per session, so the same logical
-- statement arrives under a different name every time.  With the setting off
-- each name is its own entry; with it on they accumulate into one.  The tests
-- count entries rather than compare query ids, because a query id is not
-- portable across builds.
--
-- The filters below always exclude the probing query itself, whose text
-- mentions both the table names it searches for and the extension.
CREATE EXTENSION pg_track_optimizer;

CREATE TEMP TABLE maskone AS SELECT gs AS x, gs::numeric AS y
    FROM generate_series(1, 100) gs;
CREATE TEMP TABLE masktwo AS SELECT gs AS x, gs::numeric AS y
    FROM generate_series(1, 100) gs;
CREATE TABLE maskperm1 AS SELECT gs AS x, gs::numeric AS y
    FROM generate_series(1, 100) gs;
CREATE TABLE maskperm2 AS SELECT gs AS x, gs::numeric AS y
    FROM generate_series(1, 100) gs;
ANALYZE maskone, masktwo, maskperm1, maskperm2;

SET pg_track_optimizer.mode = 'forced';

-- Off by default.
SHOW pg_track_optimizer.queryid_mask_temp_names;

--
-- Off: two temporary relations, two entries.
--
SELECT pg_track_optimizer_reset() >= 0 AS reset_ok;
SELECT count(*) FROM maskone WHERE y > 1;
SELECT count(*) FROM masktwo WHERE y > 1;

SELECT count(*) AS entries, sum(nexecs) AS execs
FROM pg_track_optimizer
WHERE (query LIKE '%FROM maskone%' OR query LIKE '%FROM masktwo%')
  AND query NOT LIKE '%pg_track_optimizer%';

--
-- On: the same two statements share one entry, and it sees both executions.
--
SET pg_track_optimizer.queryid_mask_temp_names = on;
SELECT pg_track_optimizer_reset() >= 0 AS reset_ok;
SELECT count(*) FROM maskone WHERE y > 1;
SELECT count(*) FROM masktwo WHERE y > 1;

SELECT count(*) AS entries, sum(nexecs) AS execs
FROM pg_track_optimizer
WHERE (query LIKE '%FROM maskone%' OR query LIKE '%FROM masktwo%')
  AND query NOT LIKE '%pg_track_optimizer%';

--
-- Permanent relations keep their own identity even while the setting is on.
--
SELECT pg_track_optimizer_reset() >= 0 AS reset_ok;
SELECT count(*) FROM maskperm1 WHERE y > 1;
SELECT count(*) FROM maskperm2 WHERE y > 1;

SELECT count(*) AS entries
FROM pg_track_optimizer
WHERE query LIKE '%FROM maskperm%'
  AND query NOT LIKE '%pg_track_optimizer%';

--
-- Collapsing the name does not collapse the statement: a different target
-- list over a temporary relation is still a different entry.
--
SELECT pg_track_optimizer_reset() >= 0 AS reset_ok;
SELECT count(*) FROM maskone WHERE y > 1;
SELECT sum(x) FROM masktwo WHERE y > 1;

SELECT count(*) AS entries
FROM pg_track_optimizer
WHERE (query LIKE '%FROM maskone%' OR query LIKE '%FROM masktwo%')
  AND query NOT LIKE '%pg_track_optimizer%';

--
-- Temporary relations reached through a subquery and through a CTE are masked
-- as well: the walker has to descend into both.  Four statements, one entry
-- per shape.
--
SELECT pg_track_optimizer_reset() >= 0 AS reset_ok;
SELECT count(*) FROM (SELECT x FROM maskone WHERE y > 1) s;
SELECT count(*) FROM (SELECT x FROM masktwo WHERE y > 1) s;
WITH c AS (SELECT x FROM maskone WHERE y > 1) SELECT count(*) FROM c;
WITH c AS (SELECT x FROM masktwo WHERE y > 1) SELECT count(*) FROM c;

SELECT count(*) AS entries, sum(nexecs) AS execs
FROM pg_track_optimizer
WHERE (query LIKE '%FROM maskone%' OR query LIKE '%FROM masktwo%')
  AND query NOT LIKE '%pg_track_optimizer%';

--
-- A join masks both sides.  That the query runs at all is the check that
-- matters: an unrestored parse tree would reach the planner carrying OID 0.
--
SELECT count(*) FROM maskone a JOIN masktwo b USING (x);

-- Clean up
SET pg_track_optimizer.mode = 'disabled';
RESET pg_track_optimizer.queryid_mask_temp_names;
DROP TABLE maskone, masktwo, maskperm1, maskperm2;
SELECT pg_track_optimizer_reset() >= 0 AS reset_ok;
DROP EXTENSION pg_track_optimizer;
