CREATE EXTENSION pg_track_optimizer;

\d pg_track_optimizer
\df pg_track_optimizer_flush
\df pg_track_optimizer_reset

SELECT * FROM pg_track_optimizer_reset();
DROP EXTENSION pg_track_optimizer;
