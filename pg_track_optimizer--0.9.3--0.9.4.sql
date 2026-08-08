/* contrib/pg_track_optimizer/pg_track_optimizer--0.9.3--0.9.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_track_optimizer UPDATE TO '0.9.4'" to load this file. \quit

/*
 * pg_track_optimizer.hash_mem now bounds the memory the hash table actually
 * consumes - the query texts included - instead of a notional entry count, so
 * "entries_left" no longer has a meaning: how many more queries fit depends on
 * how long their texts are.  Report what is actually known instead: the number
 * of tracked queries and the state of the memory budget.
 */
DROP VIEW pg_track_optimizer_status;
DROP FUNCTION pg_track_optimizer_status();

CREATE FUNCTION pg_track_optimizer_status(
	OUT mode       text,
	OUT entries    bigint,
	OUT mem_used   bigint,
	OUT mem_free   bigint,
	OUT dsa_size   bigint,
	OUT is_synced  boolean
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_track_optimizer_status'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_track_optimizer_status AS
  SELECT * FROM pg_track_optimizer_status();

COMMENT ON VIEW pg_track_optimizer_status IS
  'Current status of the pg_track_optimizer extension';

-- Status contains no query texts and may stay generally readable.
GRANT SELECT ON pg_track_optimizer_status TO PUBLIC;
