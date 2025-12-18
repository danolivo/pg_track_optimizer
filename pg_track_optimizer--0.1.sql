
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_track_optimizer" to load this file. \quit

CREATE FUNCTION pg_track_optimizer(
	OUT dboid			Oid,
	OUT queryid			bigint,
	OUT query           text,
	OUT avg_error		float8,
	OUT rms_error		float8,
	OUT twa_error		float8,
	OUT wca_error		float8,
	OUT evaluated_nodes integer,
	OUT plan_nodes      integer,
	OUT exec_time       float8,
	OUT nexecs          bigint,
	OUT blks_accessed   bigint
)
RETURNS setof record
AS 'MODULE_PATHNAME', 'to_show_data'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_track_optimizer AS SELECT
  t.queryid, t.query, t.avg_error, t.rms_error, t.twa_error,
  t.wca_error, t.evaluated_nodes, t.plan_nodes, t.exec_time, t.nexecs,
  t.blks_accessed
FROM pg_track_optimizer() t, pg_database d
WHERE t.dboid = d.oid AND datname = current_database();
COMMENT ON VIEW pg_track_optimizer IS 'query tracking data for current database';

CREATE FUNCTION pg_track_optimizer_flush()
RETURNS VOID
AS 'MODULE_PATHNAME', 'to_flush'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION pg_track_optimizer_reset()
RETURNS VOID
AS 'MODULE_PATHNAME', 'to_reset'
LANGUAGE C STRICT VOLATILE;
