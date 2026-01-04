#!/bin/bash
set -e

# Script to generate wiki page with JO-Bench results
# Usage: generate-wiki-page.sh <results_file> <wiki_dir> <ext_commit>

RESULTS_FILE="$1"
WIKI_DIR="$2"
EXT_COMMIT="$3"

if [ -z "$RESULTS_FILE" ] || [ -z "$WIKI_DIR" ] || [ -z "$EXT_COMMIT" ]; then
  echo "Usage: $0 <results_file> <wiki_dir> <ext_commit>"
  exit 1
fi

if [ ! -f "$RESULTS_FILE" ]; then
  echo "Error: Results file not found: $RESULTS_FILE"
  exit 1
fi

# Get metadata
TEST_DATE=$(date -u '+%Y-%m-%d %H:%M:%S UTC')
PG_VERSION=$(psql -d jobench -t -c "SELECT version();" | head -1 | xargs)
PG_COMMIT=$(cd ~/postgresql && git rev-parse HEAD)

# Read benchmark results
BENCHMARK_RESULTS=$(cat "$RESULTS_FILE")

# Generate schema dynamically from pg_track_optimizer view
SCHEMA_SQL=$(psql -d jobench -t -A -c "
  SELECT 'CREATE TABLE job_tracking_data (' || E'\n' ||
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

# Create wiki page filename with date
WIKI_PAGE="JO-Bench-Results-$(date -u '+%Y-%m-%d-%H%M%S').md"
SCHEMA_FILE="JO-Bench-Schema-$(date -u '+%Y-%m-%d-%H%M%S').sql"

# Create schema SQL file
cat > "$SCHEMA_FILE" <<EOF
-- Auto-generated schema for importing pg_track_optimizer JO-Bench results
-- Generated on: ${TEST_DATE}
-- Extension commit: ${EXT_COMMIT:0:7}

${SCHEMA_SQL}

-- Import command:
-- \copy job_tracking_data FROM 'pg_track_optimizer_results.csv' CSV HEADER;
EOF

# Create wiki page content
cat > "$WIKI_PAGE" <<EOF
# JO-Bench Results - ${TEST_DATE}

## Test Environment

- **Test Date**: ${TEST_DATE}
- **PostgreSQL Version**: ${PG_VERSION}
- **PostgreSQL Commit**: [\`${PG_COMMIT:0:7}\`](https://github.com/postgres/postgres/commit/${PG_COMMIT})
- **Extension Commit**: [\`${EXT_COMMIT:0:7}\`](https://github.com/danolivo/pg_track_optimizer/commit/${EXT_COMMIT})

## Top Queries by Error Metrics

This table shows queries that appear in the top 10 for **all** error metrics (avg_avg, rms_avg, twa_avg, and wca_avg). These are the queries with consistently poor estimation across all criteria.

\`\`\`
${BENCHMARK_RESULTS}
\`\`\`

## Column Description

The results above include the following metrics for each query:
- **queryid**: Internal PostgreSQL query identifier
- **query**: The SQL query text (normalised, with literals replaced by placeholders; truncated to first 32 characters)
- **avg_avg, avg_min, avg_max, avg_cnt, avg_dev**: Simple average of log-scale errors across plan nodes (running statistics)
- **rms_avg, rms_min, rms_max, rms_cnt, rms_dev**: Root Mean Square error statistics (emphasises large estimation errors)
- **twa_avg, twa_min, twa_max, twa_cnt, twa_dev**: Time-Weighted Average error statistics (highlights errors in slow nodes)
- **wca_avg, wca_min, wca_max, wca_cnt, wca_dev**: Cost-Weighted Average error statistics (highlights errors in expensive nodes)
- **blks_avg, blks_min, blks_max, blks_cnt, blks_dev**: Block access statistics across all executions
- **local_avg, local_min, local_max, local_cnt, local_dev**: Local block statistics (work_mem indicator - sorts/joins spilling to disk)
- **time_avg, time_min, time_max, time_cnt, time_dev**: Execution time statistics per query (milliseconds)
- **jf_avg, jf_min, jf_max, jf_cnt, jf_dev**: Maximum JOIN filtered rows statistics (detects queries with inefficient join strategies)
- **lf_avg, lf_min, lf_max, lf_cnt, lf_dev**: Maximum leaf node filtered rows statistics (detects scans fetching many rows that get filtered)
- **nexecs**: Number of times the query was executed

Only queries appearing in the top 10 of **every** error metric are shown, representing the most consistently problematic queries.

## How to Use Workflow Artifacts

The workflow produces a CSV artifact (\`pg_track_optimizer_jobench_results\`) containing complete benchmark results for all queries. You can download this artifact from the workflow run and import it into your own PostgreSQL database for analysis.

### 1. Create the tracking table

The schema is auto-generated to match the current \`pg_track_optimizer\` view definition. Download the schema file: [${SCHEMA_FILE}](${SCHEMA_FILE})

### 2. Import the CSV artifact

\`\`\`
\copy job_tracking_data FROM 'pg_track_optimizer_results.csv' CSV HEADER;
\`\`\`

### 3. Example analysis queries

Find queries with highest average error:
\`\`\`sql
SELECT queryid, LEFT(query, 80) as query_preview, avg_avg, rms_avg, twa_avg, wca_avg
FROM job_tracking_data
ORDER BY avg_avg DESC
LIMIT 10;
\`\`\`

Find queries appearing in top 10 of all error metrics (most consistently problematic):
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
git add "$WIKI_PAGE" "$SCHEMA_FILE" Home.md
git commit -m "Add JO-Bench results for ${TEST_DATE}"
git push origin master || {
  echo "Failed to push wiki page. This might be due to permissions."
  exit 1
}

echo "Wiki page created: $WIKI_PAGE"
echo "Schema file created: $SCHEMA_FILE"
echo "Home page updated with link to new results"
