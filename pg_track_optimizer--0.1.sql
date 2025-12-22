
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_track_optimizer" to load this file. \quit


CREATE TYPE rstats;

-- Input/Output functions
CREATE FUNCTION rstats_in(cstring)
    RETURNS rstats
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION rstats_out(rstats)
    RETURNS cstring
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

-- Binary I/O functions
CREATE FUNCTION rstats_recv(internal)
    RETURNS rstats
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION rstats_send(rstats)
    RETURNS bytea
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

-- Create the type
CREATE TYPE rstats (
    INTERNALLENGTH = 40,
    INPUT = rstats_in,
    OUTPUT = rstats_out,
    RECEIVE = rstats_recv,
    SEND = rstats_send,
    ALIGNMENT = double
);

COMMENT ON TYPE rstats IS 'Incremental statistics type using Welford''s algorithm';

--
-- Initialization operator (double precision -> rstats)
--

CREATE FUNCTION rstats_init_double(double precision)
    RETURNS rstats
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION rstats_init_numeric(numeric)
    RETURNS rstats
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

-- Cast to rstats
CREATE CAST (double precision AS rstats)
    WITH FUNCTION rstats_init_double(double precision)
    AS IMPLICIT;
CREATE CAST (numeric AS rstats)
    WITH FUNCTION rstats_init_numeric(numeric)
    AS IMPLICIT;

--
-- Addition operator (rstats + double precision)
--

CREATE FUNCTION rstats_add(rstats, double precision)
    RETURNS rstats
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;
COMMENT ON FUNCTION rstats_add(rstats, double precision) IS 'Add a new value to running statistics using Welford''s algorithm';

CREATE OPERATOR + (
    LEFTARG = rstats,
    RIGHTARG = double precision,
    FUNCTION = rstats_add,
    COMMUTATOR = +
);
COMMENT ON OPERATOR + (rstats, double precision) IS 'Add a value to running statistics';

-- Equality comparison operator
CREATE FUNCTION rstats_eq(rstats, rstats)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'rstats_eq'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION rstats_eq(rstats, rstats) IS 'Check equality of two rstats objects';

CREATE OPERATOR = (
    LEFTARG = rstats,
    RIGHTARG = rstats,
    FUNCTION = rstats_eq,
    COMMUTATOR = =
);

COMMENT ON OPERATOR = (rstats, rstats) IS 'Equality operator for rstats type';

-- Field accessor operator
CREATE FUNCTION rstats_get_field(rstats, text)
    RETURNS double precision
    AS 'MODULE_PATHNAME', 'rstats_get_field'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION rstats_get_field(rstats, text) IS 'Access rstats field by name using -> operator';

CREATE OPERATOR -> (
    LEFTARG = rstats,
    RIGHTARG = text,
    FUNCTION = rstats_get_field
);

COMMENT ON OPERATOR -> (rstats, text) IS 'Field accessor operator for rstats type (e.g., stats -> ''mean'')';

CREATE FUNCTION pg_track_optimizer(
	OUT dboid			Oid,
	OUT queryid			bigint,
	OUT query           text,
	OUT avg_error		float8,
	OUT rms_error		float8,
	OUT twa_error		float8,
	OUT wca_error		rstats,
	OUT evaluated_nodes integer,
	OUT plan_nodes      integer,
	OUT exec_time       float8,
	OUT nexecs          bigint,
	OUT blks_accessed   rstats
)
RETURNS setof record
AS 'MODULE_PATHNAME', 'to_show_data'
LANGUAGE C STRICT VOLATILE;

/*
 * Show queries from current database and expose statistical data as a set of
 * separate columns.
 */
CREATE VIEW pg_track_optimizer AS SELECT
  t.queryid, t.query, t.avg_error, t.rms_error, t.twa_error,
  t.wca_error -> 'min' AS wca_min, t.wca_error -> 'max' AS wca_max,
  t.wca_error -> 'count' AS wca_cnt,
  t.wca_error -> 'mean' AS wca_avg, t.wca_error -> 'stddev' AS wca_dev,
  t.evaluated_nodes, t.plan_nodes, t.exec_time, t.nexecs,
  t.blks_accessed -> 'min' AS blks_min, t.blks_accessed -> 'max' AS blks_max,
  t.blks_accessed -> 'count' AS blks_cnt,
  t.blks_accessed -> 'mean' AS blks_avg, t.blks_accessed -> 'stddev' AS blks_dev
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
