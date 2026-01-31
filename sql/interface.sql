CREATE EXTENSION pg_track_optimizer;

\d pg_track_optimizer
\df pg_track_optimizer_flush
\df pg_track_optimizer_reset
\d pg_track_optimizer_status

SELECT * FROM pg_track_optimizer_reset();
SELECT mode, entries_count >= 0 AS has_count, entries_max > 0 AS has_max, is_synced FROM pg_track_optimizer_status;
DROP EXTENSION pg_track_optimizer;
