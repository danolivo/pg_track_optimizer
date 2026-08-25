#!/bin/bash
#
# Run every jo-bench query once against the jobench database.
# Usage: run-jobench-queries.sh <query_dir> <pass_label>
#
# Each query is prefixed with a comment naming its file, so that the
# pg_track_optimizer entry it produces can be traced back to the query.
#
# Result rows are discarded - what this pass collects is plans, not data - but
# errors are not.  A query that fails leaves a hole in the corpus, and two
# passes that ran different sets of queries cannot be compared against each
# other, which is the whole point of the benchmark.  So every failure is
# reported with its psql output, and the script exits non-zero once the pass is
# over.  Reporting all of them beats stopping at the first: one run then tells
# you everything that is broken.

set -uo pipefail

QUERY_DIR=${1:?usage: run-jobench-queries.sh <query_dir> <pass_label>}
PASS=${2:?usage: run-jobench-queries.sh <query_dir> <pass_label>}

shopt -s nullglob
queries=("$QUERY_DIR"/*.sql)
shopt -u nullglob

# An empty directory used to slip through unnoticed: the loop simply had
# nothing to iterate over and the step reported that all queries had run.
if [ ${#queries[@]} -eq 0 ]; then
	echo "::error::no .sql files found in $QUERY_DIR"
	exit 1
fi

failed=()

for query in "${queries[@]}"; do
	name=$(basename "$query")
	echo "Running: $name"

	# -o /dev/null drops the rows while leaving stderr alone; the old
	# '>/dev/null 2>&1' hid the error message along with them.
	if ! { echo "/* $name */"; cat "$query"; } |
			psql -d jobench -q -o /dev/null \
			     -v ON_ERROR_STOP=1 -v datadir="'$HOME/jo-bench'"; then
		failed+=("$name")
	fi
done

echo "Pass $PASS: ${#queries[@]} queries executed, ${#failed[@]} failed."

if [ ${#failed[@]} -gt 0 ]; then
	for name in "${failed[@]}"; do
		echo "::error::pass $PASS: query $name failed"
	done
	exit 1
fi
