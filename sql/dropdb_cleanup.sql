-- DROP DATABASE must purge this extension's tracked entries for the
-- database being dropped, instead of leaving them to linger forever in the
-- shared hash table - or worse, resurface as if they belonged to some
-- future database that happens to reuse the same OID (see track_ProcessUtility()
-- in pg_track_optimizer.c).
--
-- pg_track_optimizer_status()'s "entries" figure is deliberately cluster-wide
-- (see its own comment), which is exactly what makes it usable here: it is
-- the one place we can observe the shared hash table shrink from outside the
-- database being dropped, without needing to peek at another database's
-- private state.
CREATE EXTENSION pg_track_optimizer;
SELECT pg_track_optimizer_reset() >= 0 AS cleaned;

CREATE DATABASE pgto_dropdb_probe;
\c pgto_dropdb_probe
CREATE EXTENSION pg_track_optimizer;
-- Two distinct, never-elsewhere-used query shapes: enough for the "before"
-- snapshot taken back in the original database to be strictly greater than
-- the "after" one, whatever unrelated activity is also being tracked.
SELECT 8675309 AS dropdb_probe_marker_one;
SELECT 'dropdb_probe_marker_two'::text;

\c contrib_regression
SELECT entries AS entries_before_drop FROM pg_track_optimizer_status(); \gset

DROP DATABASE pgto_dropdb_probe;

SELECT entries AS entries_after_drop FROM pg_track_optimizer_status(); \gset
SELECT :entries_before_drop > :entries_after_drop AS entries_were_purged;

-- The dropped database can no longer be reached by name, so its entries -
-- purged or not - are unreachable through any of this extension's own
-- user-facing views from here on. That is exactly the gap this test closes:
-- confirm they are actually gone from the shared table, not merely
-- unreachable.
SELECT pg_track_optimizer_reset() >= 0 AS cleaned;
DROP EXTENSION pg_track_optimizer;
