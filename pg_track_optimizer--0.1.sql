
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_track_optimizer" to load this file. \quit

/* *****************************************************************************
 *
 * The RStats' base type definition is here
 *
 * ****************************************************************************/

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
  INPUT          = rstats_in,
  OUTPUT         = rstats_out,
  RECEIVE        = rstats_recv,
  SEND           = rstats_send,
  ALIGNMENT      = double
);
COMMENT ON TYPE rstats IS
  'Incremental statistics type using Welford''s algorithm';

--
-- Initialization operator (double precision -> rstats)
--

CREATE FUNCTION rstats() RETURNS rstats
AS 'MODULE_PATHNAME', 'rstats_empty_constructor'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION rstats(ANYELEMENT) RETURNS rstats
AS 'MODULE_PATHNAME', 'rstats_constructor'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION rstats_init_double(double precision)
RETURNS rstats
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION rstats_init_int4(integer)
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
CREATE CAST (integer AS rstats)
WITH FUNCTION rstats_init_int4(integer)
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
COMMENT ON FUNCTION rstats_add(rstats, double precision)
  IS 'Add a new double value to running statistics';
CREATE FUNCTION rstats_add(rstats, integer)
RETURNS rstats
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;
COMMENT ON FUNCTION rstats_add(rstats, integer)
  IS 'Add a new integer value to running statistics';

CREATE OPERATOR + (
  LEFTARG  = rstats,
  RIGHTARG = double precision,
  FUNCTION = rstats_add,
  COMMUTATOR = +
);
COMMENT ON OPERATOR + (rstats, double precision)
  IS 'Add a double value to running statistics';

CREATE OPERATOR + (
  LEFTARG  = rstats,
  RIGHTARG = integer,
  FUNCTION = rstats_add,
  COMMUTATOR = +
);
COMMENT ON OPERATOR + (rstats, integer)
  IS 'Add an integer value to running statistics';

-- Equality comparison operator
CREATE FUNCTION rstats_eq(rstats, rstats)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'rstats_eq'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION rstats_eq(rstats, rstats)
  IS 'Check equality of two rstats objects';

CREATE OPERATOR = (
  LEFTARG  = rstats,
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

COMMENT ON FUNCTION rstats_get_field(rstats, text)
  IS 'Access rstats field by name using -> operator';

CREATE OPERATOR -> (
  LEFTARG  = rstats,
  RIGHTARG = text,
  FUNCTION = rstats_get_field
);

COMMENT ON OPERATOR -> (rstats, text)
  IS 'Field accessor operator for rstats type (e.g., stats -> ''mean'')';

/* *****************************************************************************
 *
 * The pg_track_optimizer's UI objects is defined here
 *
 * ****************************************************************************/

CREATE FUNCTION pg_track_optimizer(
	OUT dboid			Oid,
	OUT queryid			bigint,
	OUT query           text,
	OUT avg_error		rstats,
	OUT rms_error		rstats,
	OUT twa_error		rstats,
	OUT wca_error		rstats,
	OUT blks_accessed   rstats,
	OUT local_blks      rstats,
	OUT exec_time       rstats,
	OUT max_join_filtered rstats,
	OUT max_leaf_filtered rstats,
	OUT evaluated_nodes integer,
	OUT plan_nodes      integer,
	OUT nexecs          bigint
)
RETURNS setof record
AS 'MODULE_PATHNAME', 'pg_track_optimizer'
LANGUAGE C STRICT VOLATILE;

/*
 * Show queries from current database and expose statistical data as a set of
 * separate columns.
 */
CREATE VIEW pg_track_optimizer AS SELECT
  t.queryid, t.query,

  /* Average error statistics */
  t.avg_error -> 'min' AS avg_min, t.avg_error -> 'max' AS avg_max,
  t.avg_error -> 'count' AS avg_cnt,
  t.avg_error -> 'mean' AS avg_avg, t.avg_error -> 'stddev' AS avg_dev,

  /* RMS error statistics */
  t.rms_error -> 'min' AS rms_min, t.rms_error -> 'max' AS rms_max,
  t.rms_error -> 'count' AS rms_cnt,
  t.rms_error -> 'mean' AS rms_avg, t.rms_error -> 'stddev' AS rms_dev,

  /* Time-weighted average error statistics */
  t.twa_error -> 'min' AS twa_min, t.twa_error -> 'max' AS twa_max,
  t.twa_error -> 'count' AS twa_cnt,
  t.twa_error -> 'mean' AS twa_avg, t.twa_error -> 'stddev' AS twa_dev,

  /* Planning error, weighted by cost factor */
  t.wca_error -> 'min' AS wca_min, t.wca_error -> 'max' AS wca_max,
  t.wca_error -> 'count' AS wca_cnt,
  t.wca_error -> 'mean' AS wca_avg, t.wca_error -> 'stddev' AS wca_dev,

  /* Blocks statistics */
  t.blks_accessed -> 'min' AS blks_min, t.blks_accessed -> 'max' AS blks_max,
  t.blks_accessed -> 'count' AS blks_cnt,
  t.blks_accessed -> 'mean' AS blks_avg, t.blks_accessed -> 'stddev' AS blks_dev,

  /* Local blocks statistics (work_mem indicator) */
  t.local_blks -> 'min' AS local_min, t.local_blks -> 'max' AS local_max,
  t.local_blks -> 'count' AS local_cnt,
  t.local_blks -> 'mean' AS local_avg, t.local_blks -> 'stddev' AS local_dev,

  /* Execution time statistics (milliseconds) */
  t.exec_time -> 'min' AS time_min, t.exec_time -> 'max' AS time_max,
  t.exec_time -> 'count' AS time_cnt,
  t.exec_time -> 'mean' AS time_avg, t.exec_time -> 'stddev' AS time_dev,

  /* Maximum JOIN filtered rows statistics */
  t.max_join_filtered -> 'min' AS jf_min, t.max_join_filtered -> 'max' AS jf_max,
  t.max_join_filtered -> 'count' AS jf_cnt,
  t.max_join_filtered -> 'mean' AS jf_avg, t.max_join_filtered -> 'stddev' AS jf_dev,

  /* Maximum leaf node filtered rows statistics */
  t.max_leaf_filtered -> 'min' AS lf_min, t.max_leaf_filtered -> 'max' AS lf_max,
  t.max_leaf_filtered -> 'count' AS lf_cnt,
  t.max_leaf_filtered -> 'mean' AS lf_avg, t.max_leaf_filtered -> 'stddev' AS lf_dev,

  t.evaluated_nodes, t.plan_nodes, t.nexecs
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
