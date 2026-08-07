# Measured overhead

Throughput of pg_track_optimizer in each combination of
`pg_track_optimizer.mode` and `pg_track_optimizer.effort`, relative to a
server running without the extension.

## Test conditions

| | |
|---|---|
| Machine | GCP c3d-highcpu-60: 60 vCPU AMD EPYC 9B14, 118 GB RAM |
| PostgreSQL | 18.4, release build (`-O2`, no `--enable-cassert`) |
| Extension | commit `d6966cd` |
| Timer cost | 28.2 ns per loop (`pg_test_timing`) |
| Dataset | `pgbench` scale 500 (~7.5 GB), fully cached |
| Server settings | `shared_buffers = 32GB`, `autovacuum = off`, `max_parallel_workers_per_gather = 4`, checkpoints outside the measurement window |
| Client | `pgbench -M prepared` |
| Per cell | 4 runs of 30 s, first discarded, median reported |

`normal` mode runs with `log_min_error` set above any error the workloads
produce, so no query is stored or logged; `forced` stores every execution.

## Workloads

| Name | Statement |
|---|---|
| Point lookups | `pgbench -S`, 1 / 16 / 60 / 120 clients |
| Aggregate | `SELECT sum(abalance) FROM pgbench_accounts` (50 M rows) |
| Join | 3-table join over `pgbench_accounts/branches/tellers`, 8 clients |
| Distinct queryIds | 10 000 structurally different statements per pass |
| Connection churn | `pgbench -C -S`, 8 clients |

## Results

Absolute value and change against the unloaded server.

| Workload (unit) | not loaded | disabled | normal<br>rows | normal<br>timing | normal<br>full | forced<br>rows | forced<br>timing | forced<br>full |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Point lookups, 1 client (tps) | 33 726 | 33 185<br>(−2%) | 32 673<br>(−3%) | 32 287<br>(−4%) | 33 062<br>(−2%) | 32 287<br>(−4%) | 32 285<br>(−4%) | 32 433<br>(−4%) |
| Point lookups, 16 clients (tps) | 469 823 | 463 728<br>(−1%) | 454 348<br>(−3%) | 451 546<br>(−4%) | 459 220<br>(−2%) | 441 792<br>(−6%) | 436 443<br>(−7%) | 438 465<br>(−7%) |
| Point lookups, 60 clients (tps) | 1 616 482 | 1 628 051<br>(+1%) | 1 541 708<br>(−5%) | 1 572 010<br>(−3%) | 1 545 574<br>(−4%) | 728 121<br>(−55%) | 648 568<br>(−60%) | 609 569<br>(−62%) |
| Point lookups, 120 clients (tps) | 1 634 550 | 1 631 054<br>(−0%) | 1 595 629<br>(−2%) | 1 567 602<br>(−4%) | 1 591 399<br>(−3%) | 739 278<br>(−55%) | 659 453<br>(−60%) | 648 761<br>(−60%) |
| Aggregate, 50 M rows (queries/s) | 1.29 | 1.39<br>(+8%) | 1.30<br>(+1%) | 0.84<br>(−35%) | 0.75<br>(−42%) | 1.28<br>(−1%) | 0.83<br>(−36%) | 0.75<br>(−42%) |
| Join, 8 clients (tps) | 149 | 144<br>(−3%) | 137<br>(−8%) | 98<br>(−34%) | 91<br>(−39%) | 139<br>(−7%) | 98<br>(−34%) | 92<br>(−38%) |
| 10 000 distinct queryIds (s/pass) | 5.01 | 5.12<br>(−2%) | 5.03<br>(−0%) | 5.14<br>(−3%) | 5.45<br>(−9%) | 5.13<br>(−3%) | 5.19<br>(−4%) | 5.13<br>(−2%) |
| Connection churn, 8 clients (tps) | 2 749 | 2 630<br>(−4%) | 2 595<br>(−6%) | 2 592<br>(−6%) | 2 562<br>(−7%) | 2 606<br>(−5%) | 2 576<br>(−6%) | 2 584<br>(−6%) |

For the distinct-queryId row the unit is seconds per pass, so lower is
better; its percentage is normalised, like every other row, so that a
negative number means slower than the unloaded server.
