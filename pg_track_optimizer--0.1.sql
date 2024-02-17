
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_optimizer" to load this file. \quit

CREATE OR REPLACE FUNCTION pg_stat_optimizer(
	OUT dboid			Oid,
	OUT queryid			bigint,
	OUT querytext       text,
	OUT relative_error	float8,
	OUT nodes_assessed  integer,
	OUT nodes_total     integer,
	OUT exec_time       float8,
	OUT nexecs          bigint
)
RETURNS setof record
AS 'MODULE_PATHNAME', 'to_show_data'
LANGUAGE C STRICT VOLATILE;

CREATE OR REPLACE FUNCTION pg_stat_optimizer_flush()
RETURNS VOID
AS 'MODULE_PATHNAME', 'to_flush'
LANGUAGE C STRICT VOLATILE;

CREATE OR REPLACE FUNCTION pg_stat_optimizer_reset()
RETURNS VOID
AS 'MODULE_PATHNAME', 'to_reset'
LANGUAGE C STRICT VOLATILE;
