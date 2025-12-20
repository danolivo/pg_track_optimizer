
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_track_optimizer" to load this file. \quit


CREATE TYPE statistics;

-- Input/Output functions
CREATE FUNCTION statistics_in(cstring)
    RETURNS statistics
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION statistics_out(statistics)
    RETURNS cstring
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

-- Binary I/O functions
CREATE FUNCTION statistics_recv(internal)
    RETURNS statistics
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION statistics_send(statistics)
    RETURNS bytea
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

-- Create the type
CREATE TYPE statistics (
    INTERNALLENGTH = 40,
    INPUT = statistics_in,
    OUTPUT = statistics_out,
    RECEIVE = statistics_recv,
    SEND = statistics_send,
    ALIGNMENT = double
);

COMMENT ON TYPE statistics IS 'Incremental statistics type using Welford''s algorithm';

--
-- Initialization operator (double precision -> statistics)
--

CREATE FUNCTION statistics_init(double precision)
    RETURNS statistics
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;
COMMENT ON FUNCTION statistics_init(double precision) IS 'Initialize statistics from a single value';

-- Cast from double precision to statistics
CREATE CAST (double precision AS statistics)
    WITH FUNCTION statistics_init(double precision)
    AS IMPLICIT;

--
-- Addition operator (statistics + double precision)
--

CREATE FUNCTION statistics_add(statistics, double precision)
    RETURNS statistics
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;
COMMENT ON FUNCTION statistics_add(statistics, double precision) IS 'Add a new value to statistics using Welford''s algorithm';

CREATE OPERATOR + (
    LEFTARG = statistics,
    RIGHTARG = double precision,
    FUNCTION = statistics_add,
    COMMUTATOR = +
);
COMMENT ON OPERATOR + (statistics, double precision) IS 'Add a value to statistics';

--
-- Accessor functions
--

CREATE FUNCTION stats_count(statistics)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'statistics_get_count'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION stats_count(statistics) IS 'Get the count from statistics';

CREATE FUNCTION stats_mean(statistics)
    RETURNS double precision
    AS 'MODULE_PATHNAME', 'statistics_get_mean'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION stats_mean(statistics) IS 'Get the mean from statistics';

CREATE FUNCTION stats_variance(statistics)
    RETURNS double precision
    AS 'MODULE_PATHNAME', 'statistics_get_variance'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION stats_variance(statistics) IS 'Get the variance from statistics';

CREATE FUNCTION stats_stddev(statistics)
    RETURNS double precision
    AS 'MODULE_PATHNAME', 'statistics_get_stddev'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION stats_stddev(statistics) IS 'Get the standard deviation from statistics';

CREATE FUNCTION stats_min(statistics)
    RETURNS double precision
    AS 'MODULE_PATHNAME', 'statistics_get_min'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION stats_min(statistics) IS 'Get the minimum from statistics';

CREATE FUNCTION stats_max(statistics)
    RETURNS double precision
    AS 'MODULE_PATHNAME', 'statistics_get_max'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION stats_max(statistics) IS 'Get the maximum from statistics';

-- Equality comparison operator
CREATE FUNCTION statistics_eq(statistics, statistics)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'statistics_eq'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION statistics_eq(statistics, statistics) IS 'Check equality of two statistics objects';

CREATE OPERATOR = (
    LEFTARG = statistics,
    RIGHTARG = statistics,
    FUNCTION = statistics_eq,
    COMMUTATOR = =
);

COMMENT ON OPERATOR = (statistics, statistics) IS 'Equality operator for statistics type';

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
