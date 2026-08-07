-- Instrumentation effort levels (pg_track_optimizer.effort).
--
-- 'rows' collects per-node row counters only: the estimation-error metrics
-- (avg/rms/wca) and the query-level statistics (exec_time, blocks) are
-- still gathered, while the time-weighted metrics (twa_error, filter and
-- SubPlan factors) are skipped.  'timing' collects everything.
CREATE EXTENSION pg_track_optimizer;
SELECT pg_track_optimizer_reset() >= 0 AS reset_ok;

CREATE TABLE effort_test AS SELECT gs AS x FROM generate_series(1, 1000) gs;
ANALYZE effort_test;

SET pg_track_optimizer.mode = 'forced';

SET pg_track_optimizer.effort = 'rows';
SELECT count(*) FROM effort_test WHERE x > 10;

SET pg_track_optimizer.effort = 'timing';
SELECT sum(x) FROM effort_test WHERE x > 20;

SET pg_track_optimizer.mode = 'disabled';

-- rows effort: estimation errors and query-level stats have one sample,
-- time-weighted metrics have none
SELECT avg_cnt, rms_cnt, wca_cnt, twa_cnt, jf_cnt, lf_cnt, sp_cnt,
       time_cnt, blks_cnt, nexecs
FROM pg_track_optimizer
WHERE query LIKE '%count(*) FROM effort_test%'
  AND query NOT LIKE '%pg_track_optimizer%';

-- timing effort: every metric has a sample
SELECT avg_cnt, rms_cnt, wca_cnt, twa_cnt, jf_cnt, lf_cnt, sp_cnt,
       time_cnt, blks_cnt, nexecs
FROM pg_track_optimizer
WHERE query LIKE '%sum(x) FROM effort_test%'
  AND query NOT LIKE '%pg_track_optimizer%';

-- Clean up
DROP TABLE effort_test;
RESET pg_track_optimizer.effort;
SELECT pg_track_optimizer_reset() >= 0 AS reset_ok;
DROP EXTENSION pg_track_optimizer;
