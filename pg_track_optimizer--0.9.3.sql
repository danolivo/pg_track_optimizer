
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

-- Binary serialization casts
-- Cast rstats to bytea uses rstats_send directly
CREATE CAST (rstats AS bytea)
WITH FUNCTION rstats_send(rstats)
AS ASSIGNMENT;

-- Wrapper function for bytea to rstats cast
CREATE FUNCTION rstats_from_bytea(bytea)
RETURNS rstats
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

-- Cast bytea to rstats requires wrapper since rstats_recv expects internal
-- Note: This enables round-trip serialization for backups and testing
CREATE CAST (bytea AS rstats)
WITH FUNCTION rstats_from_bytea(bytea)
AS ASSIGNMENT;

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

--
-- Aggregate function for collecting values into rstats
--

CREATE FUNCTION rstats_agg_sfunc(rstats, double precision)
RETURNS rstats
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;
COMMENT ON FUNCTION rstats_agg_sfunc(rstats, double precision)
  IS 'State transition function for rstats_agg aggregate';

CREATE AGGREGATE rstats_agg(double precision) (
  SFUNC = rstats_agg_sfunc,
  STYPE = rstats
);
COMMENT ON AGGREGATE rstats_agg(double precision)
  IS 'Aggregate function that collects values and computes running statistics';

CREATE FUNCTION rstats_distance(rstats, rstats)
RETURNS float8
AS 'MODULE_PATHNAME', 'rstats_distance'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
    LEFTARG = rstats,
    RIGHTARG = rstats,
    FUNCTION = rstats_distance,
    COMMUTATOR = <->
);

COMMENT ON OPERATOR <-> (rstats, rstats) IS
'Mahalanobis distance between two statistical distributions (lower = more similar)';

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
	OUT temp_blks       rstats,
	OUT exec_time       rstats,
	OUT f_join_filter   rstats,
	OUT f_scan_filter   rstats,
	OUT f_worst_splan   rstats,
	OUT njoins          rstats,
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

  /* Temp blocks statistics (work_mem indicator) */
  t.temp_blks -> 'min' AS temp_min, t.temp_blks -> 'max' AS temp_max,
  t.temp_blks -> 'count' AS temp_cnt,
  t.temp_blks -> 'mean' AS temp_avg, t.temp_blks -> 'stddev' AS temp_dev,

  /* Execution time statistics (milliseconds) */
  t.exec_time -> 'min' AS time_min, t.exec_time -> 'max' AS time_max,
  t.exec_time -> 'count' AS time_cnt,
  t.exec_time -> 'mean' AS time_avg, t.exec_time -> 'stddev' AS time_dev,

  /* JOIN filtering factor statistics (time-weighted filtering overhead) */
  t.f_join_filter -> 'min' AS jf_min, t.f_join_filter -> 'max' AS jf_max,
  t.f_join_filter -> 'count' AS jf_cnt,
  t.f_join_filter -> 'mean' AS jf_avg, t.f_join_filter -> 'stddev' AS jf_dev,

  /* Leaf node filtering factor statistics (time-weighted filtering overhead) */
  t.f_scan_filter -> 'min' AS lf_min, t.f_scan_filter -> 'max' AS lf_max,
  t.f_scan_filter -> 'count' AS lf_cnt,
  t.f_scan_filter -> 'mean' AS lf_avg, t.f_scan_filter -> 'stddev' AS lf_dev,

  /* Worst SubPlan cost factor (nloops * cost) statistics */
  t.f_worst_splan -> 'min' AS sp_min, t.f_worst_splan -> 'max' AS sp_max,
  t.f_worst_splan -> 'count' AS sp_cnt,
  t.f_worst_splan -> 'mean' AS sp_avg, t.f_worst_splan -> 'stddev' AS sp_dev,

  /* Number of JOIN nodes statistics */
  t.njoins -> 'min' AS nj_min, t.njoins -> 'max' AS nj_max,
  t.njoins -> 'count' AS nj_cnt,
  t.njoins -> 'mean' AS nj_avg, t.njoins -> 'stddev' AS nj_dev,

  t.evaluated_nodes, t.plan_nodes, t.nexecs
FROM pg_track_optimizer() t, pg_database d
WHERE t.dboid = d.oid AND datname = current_database();
COMMENT ON VIEW pg_track_optimizer IS 'query tracking data for current database';

CREATE FUNCTION pg_track_optimizer_flush()
RETURNS integer
AS 'MODULE_PATHNAME', 'to_flush'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION pg_track_optimizer_reset()
RETURNS integer
AS 'MODULE_PATHNAME', 'to_reset'
LANGUAGE C STRICT VOLATILE;

/*
 * pg_track_optimizer_status - Return current extension status.
 *
 * Returns:
 *   mode          - current tracking mode ('disabled', 'normal', or 'forced')
 *   entries_left  - current number of entries to be filled
 *   is_synced     - whether the hash table is synced with disk
 */
CREATE FUNCTION pg_track_optimizer_status(
	OUT mode          text,
	OUT entries_left  integer,
	OUT is_synced     boolean
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_track_optimizer_status'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_track_optimizer_status AS
  SELECT * FROM pg_track_optimizer_status();

COMMENT ON VIEW pg_track_optimizer_status IS
  'Current status of the pg_track_optimizer extension';

/*
 * Extract and round the total execution time from "actual time=X..Y" format.
 * Keeps only Y (total time), rounded to nearest integer.
 */
CREATE OR REPLACE FUNCTION _normalize_actual_time(line text)
RETURNS text LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
  match text;
  total_time numeric;
  rounded_time integer;
BEGIN
  -- Extract the "actual time=X..Y" portion
  match := (regexp_matches(line, 'actual time=\d+\.\d+\.\.(\d+\.\d+)', 'g'))[1];
  IF match IS NOT NULL THEN
    total_time := match::numeric;
    rounded_time := round(total_time)::integer;
    -- Replace with rounded value
    RETURN regexp_replace(line,
      'actual time=\d+\.\d+\.\.\d+\.\d+',
      'actual time=' || rounded_time::text,
      'g');
  END IF;
  RETURN line;
END; $$;

/*
 * Normalise a single line of EXPLAIN output for regression stability.
 *
 * Platform-dependent masking (when platform_dependent=false, the default):
 *   - Memory sizes (kB/MB/GB) → NN
 *   - Floating-point row counts (N.00) → N
 *   - Volatile counters (Heap Fetches) → N
 *   - Hash allocation (Buckets, Batches) → N
 *   - Worker counts (Workers Planned, Workers Launched) → N
 *   - Wall-clock timings (actual time=X..Y) → actual time=Z (Y rounded to integer)
 *   - Planning/Execution Time → N ms
 * When platform_dependent=true, these values are exposed as-is.
 *
 * Elements shown/hidden (controlled by show_* parameters, all default false):
 *   - Cost estimates (cost=X..Y) → shown when show_cost=true
 *   - Width estimates (width=N) → shown when show_width=true
 *   - Loop counts (loops=N) → shown when show_loops=true
 *   - Detail lines (Buffers, Worker, Workers Planned/Launched, Buckets, Batches, Pre-sorted Groups, Heap Fetches, Sort Method, Cache Mode) → shown when show_details=true
 *
 * Lines matching "Index Searches:" (present only in PG 18+) return NULL to signal
 * they should be filtered out.
 *
 * This is an internal function used by pretty_explain_analyze() and
 * pretty_explain_text(). Exposed for flexibility but not part of the public API.
 */
CREATE OR REPLACE FUNCTION _normalize_explain_line(
  line text,
  platform_dependent boolean DEFAULT false,
  show_cost boolean DEFAULT false,
  show_width boolean DEFAULT false,
  show_loops boolean DEFAULT false,
  show_details boolean DEFAULT false
)
RETURNS text LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
  out_line text;
BEGIN
  out_line := line;
  -- Mask platform-dependent and volatile values unless platform_dependent=true exposes them
  IF NOT platform_dependent THEN
    -- Mask memory sizes: kB, MB, GB. These appear as "Memory Usage: 42kB" or
    -- "Memory: 42kB" (unquoted metric field names). Match only these specific
    -- patterns to avoid masking user data (e.g. quoted identifier "Memory: 42kB").
    out_line := regexp_replace(out_line, '(Memory Usage:\s*)\d+kB', '\1NN', 'g');
    out_line := regexp_replace(out_line, '(Memory Usage:\s*)\d+MB', '\1NN', 'g');
    out_line := regexp_replace(out_line, '(Memory Usage:\s*)\d+GB', '\1NN', 'g');
    out_line := regexp_replace(out_line, '(Memory:\s*)\d+kB', '\1NN', 'g');
    out_line := regexp_replace(out_line, '(Memory:\s*)\d+MB', '\1NN', 'g');
    out_line := regexp_replace(out_line, '(Memory:\s*)\d+GB', '\1NN', 'g');
    -- Mask floating-point row counts emitted by some plan nodes
    out_line := regexp_replace(out_line, 'rows=(\d+)\.00', 'rows=\1', 'g');
    -- Mask volatile counters
    out_line := regexp_replace(out_line, '(Heap Fetches:) \d+', '\1 N', 'g');
    -- Mask hash node allocation (platform-dependent)
    out_line := regexp_replace(out_line, '(Buckets:) \d+', '\1 N', 'g');
    out_line := regexp_replace(out_line, '(Batches:) \d+', '\1 N', 'g');
    -- Mask worker counts (platform-dependent allocation)
    out_line := regexp_replace(out_line, '(Workers Planned:) \d+', '\1 N', 'g');
    out_line := regexp_replace(out_line, '(Workers Launched:) \d+', '\1 N', 'g');
    -- Mask wall-clock timings (present when TIMING is enabled): keep only
    -- the total time (second value after ..), rounded to nearest integer.
    -- Use a helper function to extract and round the value properly.
    out_line := _normalize_actual_time(out_line);
  END IF;
  -- Strip cost estimates if show_cost is false
  IF NOT show_cost THEN
    out_line := regexp_replace(out_line, 'cost=\d+\.\d+\.\.\d+\.\d+\s*', '', 'g');
  END IF;
  -- Strip width estimates if show_width is false
  IF NOT show_width THEN
    out_line := regexp_replace(out_line, '\s+width=\d+', '', 'g');
  END IF;
  -- Strip loop counts if show_loops is false
  IF NOT show_loops THEN
    out_line := regexp_replace(out_line, '\s+loops=\d+', '', 'g');
  END IF;
  -- Signal that Index Searches lines should be filtered out (PG 18+ only)
  IF out_line ~ '^\s*Index Searches:' THEN
    RETURN NULL;
  END IF;
  -- Filter out detail lines if show_details is false
  IF NOT show_details THEN
    IF out_line ~ '^\s*Buffers:' OR
       out_line ~ '^\s*Worker \d+:' OR
       out_line ~ '^\s*Workers Planned:' OR
       out_line ~ '^\s*Workers Launched:' OR
       out_line ~ '^\s*Buckets:' OR
       out_line ~ '^\s*Batches:' OR
       out_line ~ '^\s*Pre-sorted Groups:' OR
       out_line ~ '^\s*Heap Fetches:' OR
       out_line ~ '^\s*Sort Method:' OR
       out_line ~ '^\s*Cache Mode:' THEN
      RETURN NULL;
    END IF;
  END IF;
  RETURN out_line;
END; $$;

/*
 * Execute EXPLAIN on a query and return normalised, regression-stable output.
 *
 * Primary purpose: produce plan text that is identical across PostgreSQL
 * versions and platforms so regression test expected files do not need to be
 * updated for every minor formatting change.
 *
 * The optional `params` argument is passed verbatim inside EXPLAIN (...), so
 * callers can request additional options (e.g. 'ANALYZE, COSTS OFF') without
 * losing the normalisation step.  The default options suppress all
 * execution-time noise (timing, buffers, summary) while still running the
 * query so that actual-rows figures are available.
 */
CREATE OR REPLACE FUNCTION pretty_explain_analyze(
  query text,
  params text DEFAULT 'ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF',
  platform_dependent boolean DEFAULT false,
  show_cost boolean DEFAULT false,
  show_width boolean DEFAULT false,
  show_loops boolean DEFAULT false,
  show_details boolean DEFAULT false
)
RETURNS TABLE (out_line text) LANGUAGE plpgsql AS $$
DECLARE
  line text;
  normalized text;
BEGIN
  IF query IS NULL OR btrim(query) = '' THEN
    RAISE EXCEPTION 'pretty_explain_analyze: query must not be NULL or empty';
  END IF;
  IF params IS NULL OR btrim(params) = '' THEN
    RAISE EXCEPTION 'pretty_explain_analyze: params must not be NULL or empty';
  END IF;

  FOR line IN
    EXECUTE 'EXPLAIN (' || params || ') ' || query
  LOOP
    normalized := _normalize_explain_line(line, platform_dependent, show_cost,
										  show_width, show_loops, show_details);
    IF normalized IS NOT NULL THEN
      out_line := normalized;
      RETURN next;
    END IF;
  END LOOP;
END; $$;

/*
 * Normalise raw EXPLAIN output (copy/pasted text) for regression stability.
 *
 * Takes the full multi-line EXPLAIN output text (as you'd copy from psql),
 * splits by newlines, applies the same normalisations as pretty_explain_analyze(),
 * and returns one line per row.
 *
 * Useful for comparing pre-captured EXPLAIN output without re-executing the
 * query. Simply copy/paste the EXPLAIN output as a multi-line string.
 *
 * Example:
 *   SELECT pretty_explain_text($$Seq Scan on t1 (cost=0.00..1.00 rows=100)
 *     Memory Usage: 42kB$$);
 */
CREATE OR REPLACE FUNCTION pretty_explain_text(
  explain_text text,
  platform_dependent boolean DEFAULT false,
  show_cost boolean DEFAULT false,
  show_width boolean DEFAULT false,
  show_loops boolean DEFAULT false,
  show_details boolean DEFAULT false
)
RETURNS TABLE (out_line text) LANGUAGE plpgsql AS $$
DECLARE
  line text;
  normalized text;
BEGIN
  IF explain_text IS NULL OR btrim(explain_text) = '' THEN
    RAISE EXCEPTION 'pretty_explain_text: explain_text must not be NULL or empty';
  END IF;

  FOR line IN
    SELECT regexp_split_to_table(explain_text, '\n')
  LOOP
    normalized := _normalize_explain_line(line, platform_dependent, show_cost,
										  show_width, show_loops, show_details);
    IF normalized IS NOT NULL THEN
      out_line := normalized;
      RETURN next;
    END IF;
  END LOOP;
END; $$;

COMMENT ON FUNCTION pretty_explain_analyze(text, text, boolean, boolean, boolean, boolean, boolean) IS
  'Run EXPLAIN on query and return plan lines normalised for stable regression output. '
  'When platform_dependent=true, masks memory sizes, floating-point row counts, '
  'hash allocation (Buckets/Batches), worker counts (Workers Planned/Launched), and wall-clock timings. '
  'The optional params argument overrides the default EXPLAIN options. '
  'When show_cost=true, cost estimates are shown. '
  'When show_width=true, width estimates are shown. '
  'When show_loops=true, loop counts are shown. '
  'When show_details=true, detail lines (Buffers, Workers, Buckets, Batches, Pre-sorted Groups, Heap Fetches, Sort Method, Cache Mode) are shown.';

COMMENT ON FUNCTION pretty_explain_text(text, boolean, boolean, boolean, boolean, boolean) IS
  'Normalise copy/pasted raw EXPLAIN output for stable regression comparison. '
  'Takes multi-line EXPLAIN text, applies the same filtering and masking as '
  'pretty_explain_analyze(), and returns one normalised line per row. '
  'When platform_dependent=true, masks memory sizes, floating-point row counts, '
  'hash allocation (Buckets/Batches), worker counts (Workers Planned/Launched), and wall-clock timings. '
  'When show_cost=true, cost estimates are shown. '
  'When show_width=true, width estimates are shown. '
  'When show_loops=true, loop counts are shown. '
  'When show_details=true, detail lines (Buffers, Workers, Buckets, Batches, Pre-sorted Groups, Heap Fetches, Sort Method, Cache Mode) are shown. '
  'Typical workflow: run EXPLAIN in psql, copy the output, then call: '
  'SELECT pretty_explain_text($$[paste EXPLAIN output here]$$);';