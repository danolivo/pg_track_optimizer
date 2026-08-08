CREATE EXTENSION pg_track_optimizer;
SELECT * FROM pg_track_optimizer_reset();

-- Absolute byte figures depend on the platform, so check the invariants that
-- must hold whatever they are.
SELECT mode, entries > 0 AS has_entries, mem_used > 0 AS has_used,
       mem_used + mem_free
         = pg_size_bytes(current_setting('pg_track_optimizer.hash_mem'))
         AS budget_adds_up,
       dsa_size >= mem_used AS dsa_covers_the_charge,
       is_synced
FROM pg_track_optimizer_status;

\d pg_track_optimizer
\df pg_track_optimizer_flush
\df pg_track_optimizer_reset
\d pg_track_optimizer_status

SELECT * FROM pg_track_optimizer_reset();
DROP EXTENSION pg_track_optimizer;
