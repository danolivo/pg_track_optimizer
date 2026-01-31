CREATE EXTENSION pg_track_optimizer;
SELECT * FROM pg_track_optimizer_reset();

SELECT mode,entries_left,is_synced
FROM pg_track_optimizer_status;

\d pg_track_optimizer
\df pg_track_optimizer_flush
\df pg_track_optimizer_reset
\d pg_track_optimizer_status

SELECT * FROM pg_track_optimizer_reset();
DROP EXTENSION pg_track_optimizer;
