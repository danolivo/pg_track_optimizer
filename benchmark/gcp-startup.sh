#!/bin/bash
# GCP VM startup script: build PostgreSQL 18 + pg_track_optimizer, run the
# benchmark suite, leave results in /var/tmp/pgto-bench.
# Instance metadata consumed:
#   bench-repo   - git URL of the extension repository
#   bench-branch - branch to benchmark
exec > /var/tmp/pgto-startup.log 2>&1
set -x

META=http://metadata.google.internal/computeMetadata/v1/instance/attributes
REPO=$(curl -s -H 'Metadata-Flavor: Google' $META/bench-repo)
BRANCH=$(curl -s -H 'Metadata-Flavor: Google' $META/bench-branch)

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential flex bison libreadline-dev \
    zlib1g-dev git python3 linux-tools-gcp || \
apt-get install -y -qq build-essential flex bison libreadline-dev \
    zlib1g-dev git python3

mkdir -p /var/tmp/pgto-bench
cd /var/tmp

# Release build of PostgreSQL 18 (no asserts, -O2)
git clone --depth 1 --branch REL_18_STABLE \
    https://github.com/postgres/postgres.git pgsrc
cd pgsrc
./configure --prefix=/opt/pg18 --without-icu
make -j"$(nproc)" -s
make -s install
cd ..

# Extension from the branch under test
git clone --depth 1 --branch "$BRANCH" "$REPO" pgto
cd pgto
make USE_PGXS=1 PG_CONFIG=/opt/pg18/bin/pg_config -s
make USE_PGXS=1 PG_CONFIG=/opt/pg18/bin/pg_config install -s
cd ..

# Record environment facts alongside the results
/opt/pg18/bin/pg_config --configure > /var/tmp/pgto-bench/pg_config.txt
git -C pgto rev-parse HEAD > /var/tmp/pgto-bench/extension_commit.txt
nproc > /var/tmp/pgto-bench/nproc.txt

# The server must not run as root
useradd -m bench || true
chown -R bench /var/tmp/pgto-bench
install -m 0755 pgto/benchmark/bench-suite.sh /var/tmp/bench-suite.sh
install -m 0644 pgto/benchmark/summarize.py /var/tmp/pgto-bench/summarize.py

sudo -u bench PGBIN=/opt/pg18/bin \
    PGDATA=/var/tmp/pgto-bench/data \
    OUTDIR=/var/tmp/pgto-bench \
    /var/tmp/bench-suite.sh

python3 /var/tmp/pgto-bench/summarize.py /var/tmp/pgto-bench/results.csv \
    > /var/tmp/pgto-bench/summary.txt || true

touch /var/tmp/pgto-bench/DONE
