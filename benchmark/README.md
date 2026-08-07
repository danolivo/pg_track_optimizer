# Benchmark harness

Measures pg_track_optimizer overhead per the plan in
`benchmarking.md`.  Workload matrix (point-lookup client sweep,
TPC-B, tuple-heavy scan, multi-way join, 10k distinct queryIds, connection
churn) across four configurations: not loaded / disabled / normal / forced.

## One-command GCP run

```bash
PROJECT=<your-project> ./benchmark/gcp-run.sh
```

Creates a **c3d-highcpu-60** spot instance (override with `MACHINE=`,
`ZONE=`), builds PostgreSQL 18 (release, -O2) and the current branch of the
extension on it, runs the suite (~2-3 h), downloads `results.csv` +
`summary.txt` into `./bench-results/`, and deletes the instance.  The trap
deletes the instance even if the script is interrupted.

Follow progress:

```bash
gcloud compute ssh pgto-bench -- tail -f /var/tmp/pgto-startup.log
```

## Running the suite on any machine

```bash
PGBIN=/opt/pg18/bin PGDATA=/tmp/bench/data OUTDIR=/tmp/bench \
    ./benchmark/bench-suite.sh
python3 benchmark/summarize.py /tmp/bench/results.csv
```

Knobs: `SCALE` (default 500), `DURATION` (60 s), `REPEATS` (5, first run
discarded as warmup), `CLIENTS` ("1 8 16 30 60 120"), `SHARED_BUFFERS`
(32GB).  The server must run as a non-root user; the suite creates its own
cluster in `PGDATA` on port 55432.

For the slow-clock bound (see the plan), repeat the `scan_c1` cell once on
a scratch VM booted with `clocksource=hpet`.
