#!/bin/bash
set -e

# Script to generate wiki page with JO-Bench results
# Usage: generate-wiki-page.sh <pass1_results> <pass2_results> <wiki_dir> <ext_commit> <bench_commit> <pass1_csv> <pass2_csv> <logfile>

PASS1_RESULTS="$1"
PASS2_RESULTS="$2"
WIKI_DIR="$3"
EXT_COMMIT="$4"
BENCH_COMMIT="$5"
PASS1_CSV="$6"
PASS2_CSV="$7"
LOGFILE="$8"

if [ -z "$PASS1_RESULTS" ] || [ -z "$PASS2_RESULTS" ] || [ -z "$WIKI_DIR" ] || [ -z "$EXT_COMMIT" ] || [ -z "$BENCH_COMMIT" ] || [ -z "$PASS1_CSV" ] || [ -z "$PASS2_CSV" ] || [ -z "$LOGFILE" ]; then
  echo "Usage: $0 <pass1_results> <pass2_results> <wiki_dir> <ext_commit> <bench_commit> <pass1_csv> <pass2_csv> <logfile>"
  exit 1
fi

if [ ! -f "$PASS1_RESULTS" ]; then
  echo "Error: Pass 1 results file not found: $PASS1_RESULTS"
  exit 1
fi

if [ ! -f "$PASS2_RESULTS" ]; then
  echo "Error: Pass 2 results file not found: $PASS2_RESULTS"
  exit 1
fi

# Get metadata
TEST_DATE=$(date -u '+%Y-%m-%d %H:%M:%S UTC')
TIMESTAMP=$(date -u '+%Y-%m-%d-%H%M%S')
PG_VERSION=$(psql -d jobench -t -c "SELECT version();" | head -1 | xargs)
PG_COMMIT=$(cd ~/postgresql && git rev-parse HEAD)

# Read benchmark results
PASS1_BENCHMARK_RESULTS=$(cat "$PASS1_RESULTS")
PASS2_BENCHMARK_RESULTS=$(cat "$PASS2_RESULTS")

# Generate schema dynamically from pg_track_optimizer view
SCHEMA_SQL1=$(psql -d jobench -t -A -c "
  SELECT 'CREATE TABLE job_tracking_data_pass1 (' || E'\n' ||
         string_agg('  ' || column_name || ' ' ||
                    CASE
                      WHEN data_type = 'USER-DEFINED' THEN 'double precision'
                      WHEN data_type = 'character varying' THEN 'text'
                      ELSE data_type
                    END ||
                    CASE WHEN column_name != last_col THEN ',' ELSE '' END,
                    E'\n' ORDER BY ordinal_position) ||
         E'\n' || ');'
  FROM (
    SELECT
      column_name,
      data_type,
      ordinal_position,
      LAST_VALUE(column_name) OVER (ORDER BY ordinal_position ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) as last_col
    FROM information_schema.columns
    WHERE table_schema = 'public'
      AND table_name = 'pg_track_optimizer'
  ) cols;
")

SCHEMA_SQL2=$(psql -d jobench -t -A -c "
  SELECT 'CREATE TABLE job_tracking_data_pass2 (' || E'\n' ||
         string_agg('  ' || column_name || ' ' ||
                    CASE
                      WHEN data_type = 'USER-DEFINED' THEN 'double precision'
                      WHEN data_type = 'character varying' THEN 'text'
                      ELSE data_type
                    END ||
                    CASE WHEN column_name != last_col THEN ',' ELSE '' END,
                    E'\n' ORDER BY ordinal_position) ||
         E'\n' || ');'
  FROM (
    SELECT
      column_name,
      data_type,
      ordinal_position,
      LAST_VALUE(column_name) OVER (ORDER BY ordinal_position ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) as last_col
    FROM information_schema.columns
    WHERE table_schema = 'public'
      AND table_name = 'pg_track_optimizer'
  ) cols;
")

# Navigate to wiki directory
cd "$WIKI_DIR"

# Configure git
git config user.name "github-actions[bot]"
git config user.email "github-actions[bot]@users.noreply.github.com"

# Create artifact directory
ARTIFACT_DIR="job-pass/${TIMESTAMP}"
mkdir -p "${ARTIFACT_DIR}"

# Create schema SQL file in artifact directory
cat > "${ARTIFACT_DIR}/schema.sql" <<EOF
-- Auto-generated schema for importing pg_track_optimizer JO-Bench results
-- Generated on: ${TEST_DATE}
-- Extension commit: ${EXT_COMMIT:0:7}
-- Benchmark commit: ${BENCH_COMMIT:0:7}

${SCHEMA_SQL1}
${SCHEMA_SQL2}

-- Import command:
\copy job_tracking_data_pass1 FROM 'pg_track_optimizer_results_pass1.csv' CSV HEADER;
\copy job_tracking_data_pass2 FROM 'pg_track_optimizer_results_pass2.csv' CSV HEADER;
EOF

# Check if zip is available, if not install it
if ! command -v zip &> /dev/null; then
  echo "zip command not found, installing..."
  sudo apt-get update && sudo apt-get install -y zip
fi

# Compress and copy artifacts
if [ -f "$PASS1_CSV" ]; then
  zip -j "${ARTIFACT_DIR}/pg_track_optimizer_jobench_results_pass1.zip" "$PASS1_CSV"
else
  echo "Warning: Pass 1 CSV not found: $PASS1_CSV"
fi

if [ -f "$PASS2_CSV" ]; then
  zip -j "${ARTIFACT_DIR}/pg_track_optimizer_jobench_results_pass2.zip" "$PASS2_CSV"
else
  echo "Warning: Pass 2 CSV not found: $PASS2_CSV"
fi

if [ -f "$LOGFILE" ]; then
  zip -j "${ARTIFACT_DIR}/postgresql_log.zip" "$LOGFILE"
else
  echo "Error: PostgreSQL log not found: $LOGFILE"
  exit 1
fi

# Create wiki page with new naming convention
WIKI_PAGE="report-${TIMESTAMP}.md"

# Create wiki page content
cat > "$WIKI_PAGE" <<EOF
# Join Order Benchmark Test Results

## Test Environment

- **Test Date**: ${TEST_DATE}
- **PostgreSQL Version**: ${PG_VERSION}
- **PostgreSQL Commit**: [\`${PG_COMMIT:0:7}\`](https://github.com/postgres/postgres/commit/${PG_COMMIT})
- **Extension Commit**: [\`${EXT_COMMIT:0:7}\`](https://github.com/danolivo/pg_track_optimizer/commit/${EXT_COMMIT})
- **Test Configuration**: Two-pass testing (Pass 1: no extra indexes, Pass 2: with [extra indexes](extra_indexing))
- **Benchmark Source**: [\`${BENCH_COMMIT:0:7}\`](https://github.com/danolivo/jo-bench/commit/${BENCH_COMMIT})

## Top Queries by Error Metrics (Pass 1 - basic Join-Order-Benchmark)

This table shows queries that appear in the top 10 for **all** error metrics (avg_avg, rms_avg, twa_avg, and wca_avg). These are the queries with consistently poor estimation across all criteria.

**Note**: These results are from **Pass 1** (without extra indexes).

\`\`\`
${PASS1_BENCHMARK_RESULTS}
\`\`\`

## Top Queries by Error Metrics (Pass 2 - with extra indexes)

This table shows queries that appear in the top 10 for **all** error metrics (avg_avg, rms_avg, twa_avg, and wca_avg). These are the queries with consistently poor estimation across all criteria.

**Note**: These results are from **Pass 2** (with extra indexes). Pass 1 results (without extra indexes) are available as a separate artifact for comparison.

\`\`\`
${PASS2_BENCHMARK_RESULTS}
\`\`\`

## Column Description

The results above include the following metrics for each query:
- **queryid**: Internal PostgreSQL query identifier
- **query**: The SQL query text (normalised, with literals replaced by placeholders; truncated to first 32 characters)
- **error**: Simple average error across plan nodes

Only queries appearing in the top 10 of **every** error metric are shown, representing the most consistently problematic queries.

## How to Use Workflow Artifacts

The workflow performs **two passes** and produces two CSV artifacts for comparison:

- **Pass 1** (\`pg_track_optimizer_jobench_results_pass1\`): [results](${ARTIFACT_DIR}/pg_track_optimizer_jobench_results_pass1.zip) **without** extra indexes.
- **Pass 2** (\`pg_track_optimizer_jobench_results_pass2\`): [results](${ARTIFACT_DIR}/pg_track_optimizer_jobench_results_pass2.zip) **with** extra indexes from [Extra-Indexing](extra_indexing) wiki page.

Also, explore the [log file](${ARTIFACT_DIR}/postgresql_log.zip), which contains EXPLAIN ANALYZE dumps for each executed query. You can match them to a 'tracker' record by queryId, attached to each explain.

Artifacts can also be downloaded from the workflow run and imported into your own PostgreSQL database for comparative analysis.

### 1. Create the tracking table

The schema is auto-generated to match the current \`pg_track_optimizer\` view definition. Download the schema file: [schema.sql](${ARTIFACT_DIR}/schema.sql)

### 2. Import the CSV artifact

\`\`\`
\copy job_tracking_data FROM 'pg_track_optimizer_results.csv' CSV HEADER;
\`\`\`

### 3. Example analysis queries

Find queries with the highest average error:
\`\`\`sql
SELECT queryid, LEFT(query, 80) as query_preview, avg_avg, rms_avg, twa_avg, wca_avg
FROM job_tracking_data
ORDER BY avg_avg DESC
LIMIT 10;
\`\`\`

Find queries appearing in the top 10 of all error metrics (most consistently problematic):
\`\`\`sql
WITH
  top_avg AS (SELECT queryid FROM job_tracking_data ORDER BY avg_avg DESC LIMIT 10),
  top_rms AS (SELECT queryid FROM job_tracking_data ORDER BY rms_avg DESC LIMIT 10),
  top_twa AS (SELECT queryid FROM job_tracking_data ORDER BY twa_avg DESC LIMIT 10),
  top_wca AS (SELECT queryid FROM job_tracking_data ORDER BY wca_avg DESC LIMIT 10),
  intersection AS (
    SELECT queryid FROM top_avg
    INTERSECT SELECT queryid FROM top_rms
    INTERSECT SELECT queryid FROM top_twa
    INTERSECT SELECT queryid FROM top_wca
  )
SELECT t.*
FROM job_tracking_data t
WHERE t.queryid IN (SELECT queryid FROM intersection)
ORDER BY t.avg_avg DESC;
\`\`\`

---

_Automatically generated by [JO-Bench workflow](https://github.com/danolivo/pg_track_optimizer/actions/workflows/jo-bench.yml)_
EOF

# Update Home page with link to new results
WIKI_PAGE_NAME=$(basename "$WIKI_PAGE" .md)

if [ ! -f "Home.md" ]; then
  # Create Home page if it doesn't exist
  cat > Home.md <<'HOMEEOF'
# pg_track_optimizer Wiki

Welcome to the pg_track_optimizer wiki!

## JO-Bench Performance Test Results

HOMEEOF
fi

# Add link to the new report at the beginning of the results section
if grep -q "## JO-Bench Performance Test Results" Home.md; then
  # Insert link after the header
  sed -i "/## JO-Bench Performance Test Results/a\\- [${TEST_DATE}](${WIKI_PAGE_NAME})" Home.md
else
  # Append section if it doesn't exist
  cat >> Home.md <<HOMEEOF

## JO-Bench Performance Test Results

- [${TEST_DATE}](${WIKI_PAGE_NAME})
HOMEEOF
fi

# Commit and push to wiki repository
git add "$WIKI_PAGE" "${ARTIFACT_DIR}/" Home.md
git commit -m "Add Join Order Benchmark results for ${TEST_DATE}"
git push origin master || {
  echo "Failed to push wiki page. This might be due to permissions."
  exit 1
}

echo "Wiki page created: $WIKI_PAGE"
echo "Artifacts directory created: ${ARTIFACT_DIR}/"
echo "Home page updated with link to new results"
