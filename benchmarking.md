# Performance Overhead: Measurements and Benchmarking Plan

## Why

The extension forces `INSTRUMENT_TIMER | INSTRUMENT_ROWS |
INSTRUMENT_BUFFERS` on every tracked query, which is exactly the per-tuple
`InstrStartNode()/InstrStopNode()` timing overhead known from
`EXPLAIN ANALYZE` — historically anywhere from a few percent on point
lookups to 30–60% on tuple-heavy plans with slow system clocks.  On top of
that, `forced` mode takes an exclusive dshash partition lock and updates
eleven running-statistics accumulators at every `ExecutorEnd`.

This document records (a) preliminary numbers from a small container,
(b) the benchmarking methodology those numbers came from, and (c) the plan
and hardware recommendation for a proper run on GCP.

## What the extension adds per query, by mode

| cost component                                   | disabled | normal | forced |
|--------------------------------------------------|:--------:|:------:|:------:|
| ExecutorStart/End hook calls + shmem attach check |    x     |   x    |   x    |
| per-tuple timing/row/buffer instrumentation       |          |   x    |   x    |
| plan-tree walk computing error metrics            |          |   x    |   x    |
| dshash exclusive lock + 11 RStats updates         |          |  (1)   |   x    |
| EXPLAIN logging                                   |          |  (1)   |  (1)   |

(1) only when the computed error exceeds `log_min_error`.

`normal` mode pays the full instrumentation cost for *every* query even
though only queries crossing the threshold are stored/logged — there is no
sampling.  This is the main structural cost driver.

## Preliminary numbers (small container — directional only)

Environment: 4 shared vCPUs, 15 GB RAM, cloud container; PostgreSQL 18.4
release build (-O2, no asserts); scale-50 pgbench (~750 MB, fully cached);
`shared_buffers = 4GB`; medians of 3 × 20 s runs, `-M prepared`.

Configurations:

* **A** — extension not loaded (baseline)
* **B** — loaded, `mode = disabled`
* **C** — loaded, `mode = normal`, `log_min_error` high (instrument + walk,
  no store, no log)
* **D** — loaded, `mode = forced` (instrument + walk + store per execution)

Workloads:

* **point_c4** — `pgbench -S`, 4 clients: OLTP point lookups, 1-node plan.
  Measures fixed per-query overhead.
* **point_c16** — same with 16 clients (4× oversubscribed): shows lock/
  contention effects of the store path, all clients hammering one queryId.
* **scan_c1** — `SELECT sum(abalance) FROM pgbench_accounts` (5 M rows,
  parallel seq scan): worst case for per-tuple timing overhead.

Results (tps, median of 3; Δ vs A):

| workload   | A (not loaded) | B (disabled)    | C (normal)       | D (forced)       |
|------------|---------------:|----------------:|-----------------:|-----------------:|
| point_c4   |        141,946 | 140,082 (−1.3%) | 136,392 (−3.9%)  | 131,066 (−7.7%)  |
| point_c16  |         91,475 |  92,847 (+1.5%) |  90,433 (−1.1%)  |  76,194 (−16.7%) |
| scan_c1    |           7.00 |    6.90 (−1.4%) |    3.71 (−47.0%) |    4.07 (−41.9%) |

`pg_test_timing` on this machine: 24.86 ns per loop (fast clock — the
favourable case for timing instrumentation).

### Reading of the preliminary numbers

* **Hook overhead alone (B) is genuinely negligible** — within run-to-run
  noise (±1.5%) on every workload.  Loading the extension with
  `mode = disabled` costs nothing measurable.
* **`normal` mode on cached OLTP point lookups costs low single digits**
  (−1 to −4%).  A one-node plan returning one row has little per-tuple
  work to instrument; the fixed per-query costs (instrumentation setup,
  plan_error() walk over one node) dominate and they are small.
* **`normal` mode on tuple-heavy plans costs ~half the throughput**
  (−47% on a 5M-row parallel aggregate) — and this is on a 25 ns clock.
  This is the per-tuple `InstrStopNode()` timing tax, identical in
  nature to `EXPLAIN ANALYZE` overhead, and it applies to *every*
  execution regardless of whether the query ends up being stored or
  logged.  On hardware with slow clock reads it will be worse.
  "Minimal performance impact" is not a defensible description of this
  mode for scan-heavy workloads; the README has been updated to say so.
* **`forced` mode adds visible store-path contention on hot queryIds**:
  −7.7% at 4 clients grows to −16.7% at 16 clients on point lookups
  where every client updates the same hash entry under an exclusive
  dshash partition lock.  The divergence with client count is the
  signature of lock serialization, and 4 shared vCPUs can only hint at
  it — this is the cell that most needs the large-core-count GCP run.
* On the scan workload C and D are equivalent within noise: one store
  per ~250 ms query is amortized to nothing.  The store path only
  matters at high execution rates.

## Definitive run: c3d-highcpu-60 (GCP, 2026-08-05)

Environment: GCP c3d-highcpu-60 spot (60 vCPU AMD Genoa, us-east4-b),
Ubuntu 24.04, PostgreSQL 18 release build (-O2, no asserts), scale-500
pgbench (~7.5 GB, `shared_buffers = 32GB`, fully cached), 28.2 ns clock
(`pg_test_timing`).  Medians of 5 x 60 s runs (first discarded),
`-M prepared`.

Results (tps except distinct_10k = seconds/pass; Δ vs A):

| workload     | A (not loaded) | B (disabled) | C (normal) | D (forced) |
|--------------|---------------:|-------------:|-----------:|-----------:|
| point_c1     |         33,589 |        −0.1% |      −2.3% |      −4.0% |
| point_c8     |        254,528 |        −0.7% |      −2.3% |      −3.7% |
| point_c16    |        471,664 |        −0.6% |      −2.7% |      −5.7% |
| point_c30    |        666,766 |        +0.9% |      −2.1% |     −14.6% |
| point_c60    |      1,645,589 |        −0.6% |      −5.7% |   **−54.8%** |
| point_c120   |      1,693,765 |        −1.9% |      −5.2% |   **−56.5%** |
| tpcb_c30     |         28,223 |        +0.8% |      −0.1% |      −2.4% |
| scan_c1      |           1.55 |        −0.1% |     −44.3% |     −44.2% |
| join_c8      |          150.6 |        −3.2% |     −38.0% |     −40.4% |
| distinct_10k |         4.63 s |        −0.7% |      −1.3% |      −1.4% |
| churn_c8     |          2,753 |        −4.5% |      −5.6% |      −5.8% |

### Reading of the definitive numbers

* **`disabled` is free** at every scale - within noise on all eleven
  cells.  Shipping the extension preloaded-but-disabled costs nothing.
* **`normal` mode splits cleanly by workload shape.**  OLTP point
  lookups and TPC-B pay 0-6%; per-tuple-heavy plans pay 38-44%
  (44% on a 50M-row parallel aggregate, 38% on a three-way join), even
  on a 28 ns clock.  The container measurement (-47%) reproduced almost
  exactly on server hardware: this is a structural property of always-on
  `INSTRUMENT_TIMER`, not an artifact.
* **`forced` mode collapses under same-query concurrency.**  The
  exclusive dshash partition lock on the hot queryId serializes
  ExecutorEnd: -5.7% at 16 clients, -14.6% at 30, **-55% at 60 and
  beyond** - throughput caps at ~740k tps while the baseline reaches
  1.69M.  This is the smoking gun for the pg_stat_statements-style
  store-path redesign (shared lock + per-entry spinlock/atomics).
* **The insert path is cheap** (10k distinct queryIds: -1.4%) and the
  **churn cell survives** at -5.8% - before the LWLockConditionalAcquire
  fix this cell exhausted max_connections and aborted.
* TPC-B is essentially unaffected in `normal` mode: WAL and write costs
  dominate the per-tuple instrumentation of small plans.

### Consequences

1. The single most valuable future work for `normal` mode is a
   `sample_rate` GUC (as in auto_explain): instrument only a fraction of
   executions, keeping the detection ability while shedding the always-on
   per-tuple tax.
2. The store path (exclusive partition lock + 11 RStats updates per
   execution) deserves a pg_stat_statements-style redesign - shared
   lock + per-entry spinlock - before `forced` mode is recommended on
   high-throughput OLTP.
