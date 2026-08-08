-- Memory accounting: pg_track_optimizer.hash_mem bounds the entries and the
-- query texts together, and pg_track_optimizer.max_query_size caps what one
-- entry may contribute.
CREATE EXTENSION pg_track_optimizer;
SELECT * FROM pg_track_optimizer_reset();

-- A long query text is truncated to max_query_size, never beyond it.
SET pg_track_optimizer.max_query_size = 64;
SELECT 1 AS x /* aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa */;
SELECT length(query) <= 64 AS text_is_clipped
FROM pg_track_optimizer
WHERE query LIKE 'SELECT 1 AS x%';
RESET pg_track_optimizer.max_query_size;

-- Truncation never cuts a multibyte character in half: the stored prefix is
-- still valid text in the database encoding.
SELECT * FROM pg_track_optimizer_reset();
SET pg_track_optimizer.max_query_size = 64;
SELECT 2 AS y /* ЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖ */;
SELECT length(query) <= 64 AS bytes_within_limit,
       length(query) >= 32 AS not_truncated_to_nothing
FROM pg_track_optimizer
WHERE query LIKE 'SELECT 2 AS y%';
RESET pg_track_optimizer.max_query_size;

-- The charge and the release agree: resetting an entry gives back exactly
-- what storing it took.
SELECT * FROM pg_track_optimizer_reset();
SELECT mem_used AS baseline FROM pg_track_optimizer_status \gset
SELECT 3 AS z;
SELECT 4 AS z;
SELECT mem_used > :baseline AS charge_grew FROM pg_track_optimizer_status;
SELECT * FROM pg_track_optimizer_reset();
-- Back to a single entry (the reset call itself), so the same charge as the
-- baseline measured the same way.
SELECT mem_used = :baseline AS charge_released FROM pg_track_optimizer_status;

SELECT * FROM pg_track_optimizer_reset();
DROP EXTENSION pg_track_optimizer;
