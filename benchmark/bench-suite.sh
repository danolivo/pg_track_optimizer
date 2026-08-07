#!/bin/bash
# pg_track_optimizer overhead benchmark suite.
#
# Runs the full workload matrix from benchmarking.md against four
# configurations:
#   A - extension not loaded            (baseline)
#   B - loaded, mode = disabled         (hook overhead only)
#   C - loaded, mode = normal, high log_min_error
#                                       (instrument + walk, no store/log)
#   D - loaded, mode = forced           (instrument + walk + store per exec)
#   E - like C but effort = rows        (row counters only, no clock reads)
#
# Environment (defaults suit a c3d-highcpu-60):
#   PGBIN      - PostgreSQL bin directory                  (/opt/pg18/bin)
#   PGDATA     - data directory to create/use              (/var/tmp/pgto-bench/data)
#   OUTDIR     - results directory                         (/var/tmp/pgto-bench)
#   SCALE      - pgbench scale factor                      (500 = ~7.5 GB)
#   DURATION   - seconds per measured run                  (60)
#   REPEATS    - runs per cell (first is discarded)        (5)
#   CLIENTS    - client counts for the point-lookup sweep  ("1 8 16 30 60 120")
#   SHARED_BUFFERS                                         (32GB)
set -e

PGBIN=${PGBIN:-/opt/pg18/bin}
PGDATA=${PGDATA:-/var/tmp/pgto-bench/data}
OUTDIR=${OUTDIR:-/var/tmp/pgto-bench}
SCALE=${SCALE:-500}
DURATION=${DURATION:-60}
REPEATS=${REPEATS:-5}
CLIENTS=${CLIENTS:-"1 8 16 30 60 120"}
SHARED_BUFFERS=${SHARED_BUFFERS:-32GB}
export PGPORT=${PGPORT:-55432}
export PGHOST=/tmp

RESULTS=$OUTDIR/results.csv
mkdir -p $OUTDIR

njobs=$(nproc)

log() { echo "[bench $(date +%H:%M:%S)] $*"; }

config_server() {
    local preload="$1" mode="$2" lme="$3" effort="${4:-timing}"
    cat > $PGDATA/postgresql.auto.conf <<EOF
shared_preload_libraries = '$preload'
pg_track_optimizer.mode = '$mode'
pg_track_optimizer.log_min_error = $lme
pg_track_optimizer.effort = '$effort'
pg_track_optimizer.hash_mem = 16384
shared_buffers = '$SHARED_BUFFERS'
huge_pages = try
autovacuum = off
max_connections = 300
max_wal_size = '64GB'
checkpoint_timeout = '60min'
max_parallel_workers_per_gather = 4
unix_socket_directories = '/tmp'
port = $PGPORT
EOF
    $PGBIN/pg_ctl -D $PGDATA -l $OUTDIR/server.log -w restart >/dev/null
}

warmup() {
    $PGBIN/psql -q -d bench -c 'SELECT sum(abalance) FROM pgbench_accounts;' >/dev/null
    $PGBIN/pgbench -n -S -M prepared -c 16 -j 16 -T 5 bench >/dev/null 2>&1
}

# run_cell <cfg> <workload-label> <pgbench args...>
run_cell() {
    local cfg="$1" label="$2"; shift 2
    for i in $(seq 1 $REPEATS); do
        tps=$($PGBIN/pgbench -n -M prepared -T $DURATION "$@" bench 2>&1 | \
              awk '/^tps/ {print $3}')
        echo "$cfg,$label,$i,$tps" | tee -a $RESULTS
    done
}

# ---------------------------------------------------------------- one-time init
if [ ! -d $PGDATA ]; then
    log "initdb + pgbench -i -s $SCALE"
    $PGBIN/initdb -D $PGDATA >/dev/null
    cat >> $PGDATA/postgresql.conf <<EOF
unix_socket_directories = '/tmp'
port = $PGPORT
EOF
    $PGBIN/pg_ctl -D $PGDATA -l $OUTDIR/server.log -w start >/dev/null
    $PGBIN/createdb bench
    $PGBIN/pgbench -i -s $SCALE -q bench >/dev/null

    # Distinct-queryId generator: a 100x100 grid of structurally different
    # WHERE expressions => 10,000 distinct queryIds in one script.
    log "generating distinct-query script"
    python3 - > $OUTDIR/distinct.sql <<'PYEOF'
for i in range(1, 101):
    for j in range(1, 101):
        left = '+'.join(['bid'] * i)
        right = '+'.join(['bbalance'] * j)
        print(f"SELECT count(*) FROM pgbench_branches WHERE {left} = {right};")
PYEOF
    $PGBIN/pg_ctl -D $PGDATA -w stop >/dev/null
fi

cat > $OUTDIR/scan.sql <<'EOF'
SELECT sum(abalance) FROM pgbench_accounts;
EOF
cat > $OUTDIR/join.sql <<'EOF'
SELECT count(*), max(t.tbalance)
FROM pgbench_accounts a
JOIN pgbench_branches b USING (bid)
JOIN pgbench_tellers t ON t.bid = b.bid
WHERE a.aid < 100000;
EOF

echo "config,workload,run,tps" > $RESULTS
$PGBIN/pg_test_timing -d 5 > $OUTDIR/pg_test_timing.txt 2>&1 || true

# ---------------------------------------------------------------- main matrix
for CFG in A B C D E; do
    case $CFG in
        A) config_server ''                   'disabled' '-1'             ;;
        B) config_server 'pg_track_optimizer' 'disabled' '-1'             ;;
        C) config_server 'pg_track_optimizer' 'normal'   '100000'         ;;
        D) config_server 'pg_track_optimizer' 'forced'   '-1'             ;;
        E) config_server 'pg_track_optimizer' 'normal'   '100000' 'rows'  ;;
    esac
    log "=== config $CFG ==="
    warmup

    # 1. Point-lookup sweep: fixed per-query overhead and, in forced mode,
    #    store-path contention on a single hot queryId.
    for c in $CLIENTS; do
        j=$(( c < njobs ? c : njobs ))
        run_cell $CFG "point_c$c" -S -c $c -j $j
    done

    # 2. TPC-B read-write, moderate concurrency.
    run_cell $CFG "tpcb_c30" -c 30 -j 30

    # 3. Tuple-heavy parallel aggregate: per-tuple timing worst case.
    run_cell $CFG "scan_c1" -f $OUTDIR/scan.sql -c 1

    # 4. Multi-way join: plan_error() walk vs node count.
    run_cell $CFG "join_c8" -f $OUTDIR/join.sql -c 8 -j 8

    # 5. Distinct queryIds: insert path, dsa allocation, capacity behaviour.
    #    One 'transaction' = 10,000 distinct queries; report seconds/pass.
    for i in $(seq 1 3); do
        t0=$(date +%s.%N)
        $PGBIN/psql -q -d bench -f $OUTDIR/distinct.sql >/dev/null
        t1=$(date +%s.%N)
        echo "$CFG,distinct_10k,$i,$(echo "$t1 $t0" | awk '{print $1-$2}')" | tee -a $RESULTS
    done

    # 6. Connection churn with a populated hash table: auto_flush whole-file
    #    rewrite per backend exit (-C = new connection per transaction).
    run_cell $CFG "churn_c8" -C -S -c 8 -j 8
done

$PGBIN/pg_ctl -D $PGDATA -w stop >/dev/null
log "suite complete: $RESULTS"
