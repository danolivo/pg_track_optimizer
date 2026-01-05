# Wiki JOB Report Structure Documentation

This document describes the standardized structure for Join Order Benchmark (JOB) test reports published to the pg_track_optimizer wiki. The jo-bench.yml workflow must generate reports consistent with this document.

## Report File Naming Convention

**Pattern**: `report-YYYY-MM-DD-HHMMSS.md`

**Example**: `report-2026-01-05-084656.md`

The timestamp corresponds to the test execution date/time in UTC format.

## Artifact Directory Structure

All test artifacts and generated scripts are organized in subdirectories within the wiki repository:

```
wiki/
└── job-pass/
    └── YYYY-MM-DD-HHMMSS/
        ├── pg_track_optimizer_jobench_results_pass1.zip
        ├── pg_track_optimizer_jobench_results_pass2.zip
        ├── postgresql_log.zip
        └── schema.sql
```

**Example**:
```
wiki/job-pass/2026-01-05-084656/
```

### Artifact Files

1. **`pg_track_optimizer_jobench_results_pass1.zip`**
   - Dump the extension's statistical data to analyse later. It is copied to wiki's repository right after the JOB pass has finalised.
   - Contains: CSV file with Pass 1 results (without extra indexes)
   - Generated from: `/tmp/pg_track_optimizer_results_pass1.csv`

2. **`pg_track_optimizer_jobench_results_pass2.zip`**
   - Dump the extension's statistical data to analyse later. It is copied to wiki's repository right after the JOB pass has finalised.
   - Contains: CSV file with Pass 2 results (with extra indexes)
   - Generated from: `/tmp/pg_track_optimizer_results_pass2.csv`

3. **`postgresql_log.zip`**
   - Save EXPLAINs logged by the extension to match execution results. It is copied to wiki's repository right after the JOB pass has finalised.
   - Contains: PostgreSQL server log with EXPLAIN ANALYZE dumps
   - Source: `~/pgdata/logfile.log`
   - Each query execution includes queryId for matching to tracker records (this is provided by the extension's print format itself, no extra actions needed in workflow)

4. **`schema.sql`**
   - Auto-generated CREATE TABLE statement
   - An example of COPY command restoring data from a state dump.
   - Schema matches current `pg_track_optimizer` view definition
   - Used for importing CSV artifacts into analysis database

## Report Markdown Structure

### 1. Title

```markdown
# Join Order Benchmark Test Results
```

**Note**: Use "Join Order Benchmark" (full name), not "JO-Bench"

### 2. Test Environment Section

```markdown
## Test Environment

- **Test Date**: YYYY-MM-DD HH:MM:SS UTC
- **PostgreSQL Version**: [full version string from SELECT version()]
- **PostgreSQL Commit**: [`<short-hash>`](https://github.com/postgres/postgres/commit/<full-hash>)
- **Extension Commit**: [`<short-hash>`](https://github.com/danolivo/pg_track_optimizer/commit/<full-hash>)
- **Test Configuration**: Two-pass testing (Pass 1: no extra indexes, Pass 2: with [extra indexes](extra_indexing))
- **Benchmark Source**: [<short-hash>](https://github.com/danolivo/jo-bench/commit/<full-hash>)
```

**Fields**:
- **Test Date**: UTC timestamp of test execution
- **PostgreSQL Version**: Full output from `SELECT version()`
- **PostgreSQL Commit**: Link to postgres/postgres commit (7-char short hash shown, full hash in URL)
- **Extension Commit**: Link to pg_track_optimizer commit
- **Test Configuration**: Standard two-pass description with link to `extra_indexing` wiki page
- **Benchmark Source**: Link to jo-bench repository commit used for test data/queries

### 3. Results Section - Pass 1

```markdown
## Top Queries by Error Metrics (Pass 1 - basic Join-Order-Benchmark)

This table shows queries that appear in the top 10 for **all** error metrics (avg_avg, rms_avg, twa_avg, and wca_avg). These are the queries with consistently poor estimation across all criteria.

**Note**: These results are from **Pass 1** (without extra indexes).
```
**Example**:
```
       queryid        |        query        |  error
----------------------+---------------------+--------
  8437282107574130241 | /* 30c.sql */       |  4.35
 -8317260399803614106 | /* 28c.sql */       |  4.07
 -1799126654562256384 | /* 29c.sql */       |  3.74
  3901602676749870732 | /* 7c.sql */        |  3.63
(4 rows)
```

### 4. Results Section - Pass 2

```markdown
## Top Queries by Error Metrics (Pass 2 - with extra indexes)

This table shows queries that appear in the top 10 for **all** error metrics (avg_avg, rms_avg, twa_avg, and wca_avg). These are the queries with consistently poor estimation across all criteria.

**Note**: These results are from **Pass 2** (with extra indexes). Pass 1 results (without extra indexes) are available as a separate artifact for comparison.

```

**Example**:
```
       queryid        |        query        |  error
----------------------+---------------------+--------
  8437282107574130241 | /* 30c.sql */       |  4.35
 -8317260399803614106 | /* 28c.sql */       |  4.07
 -1799126654562256384 | /* 29c.sql */       |  3.74
  3901602676749870732 | /* 7c.sql */        |  3.63
(4 rows)
```

**Table Format**:
- **3 columns**: queryid, query, error
- **Query column**: Shows 32 first symbols of the SQL (e.g., `/* 30c.sql */`).
- **Error column**: Single aggregated error metric (represents a mean field of simple average RStats error column)
- Use PostgreSQL psql output format (not markdown table)
- Include row count footer: `(N rows)`

**SQL Query** to generate this table (approximate):
```sql
WITH
  top_avg AS (SELECT queryid FROM pg_track_optimizer ORDER BY avg_avg DESC LIMIT 10),
  top_rms AS (SELECT queryid FROM pg_track_optimizer ORDER BY rms_avg DESC LIMIT 10),
  top_twa AS (SELECT queryid FROM pg_track_optimizer ORDER BY twa_avg DESC LIMIT 10),
  top_wca AS (SELECT queryid FROM pg_track_optimizer ORDER BY wca_avg DESC LIMIT 10),
  intersection AS (
    SELECT queryid FROM top_avg
    INTERSECT SELECT queryid FROM top_rms
    INTERSECT SELECT queryid FROM top_twa
    INTERSECT SELECT queryid FROM top_wca
  )
SELECT
  v.queryid, LEFT(v.query, 32), ROUND(v.avg_avg, 2) as error
FROM pg_track_optimizer v
WHERE v.queryid IN (SELECT queryid FROM intersection)
ORDER BY error DESC;
```

### 5. Column Description Section

```markdown
## Column Description

The results above include the following metrics for each query:
- **queryid**: Internal PostgreSQL query identifier
- **query**: The SQL query text (normalised, with literals replaced by placeholders; truncated to first 32 characters)
- **error**: Simple average error across plan nodes

Only queries appearing in the top 10 of **every** error metric are shown, representing the most consistently problematic queries.
```

**Note**: Simplified from the full pg_track_optimizer view schema. Only documents the 3 columns shown in the results table.

### 6. Workflow Artifacts Section

```markdown
## How to Use Workflow Artifacts

The workflow performs **two passes** and produces two CSV artifacts for comparison:

- **Pass 1** (`pg_track_optimizer_jobench_results_pass1`): [results](job-pass/YYYY-MM-DD-HHMMSS/pg_track_optimizer_jobench_results_pass1.zip) **without** extra indexes.
- **Pass 2** (`pg_track_optimizer_jobench_results_pass2`): [results](job-pass/YYYY-MM-DD-HHMMSS/pg_track_optimizer_jobench_results_pass2.zip) **with** extra indexes from [Extra-Indexing](extra_indexing) wiki page.

Also, explore the [log file](job-pass/YYYY-MM-DD-HHMMSS/postgresql_log.zip), which contains EXPLAIN ANALYZE dumps for each executed query. You can match them to a 'tracker' record by queryId, attached to each explain.

Artifacts can also be downloaded from the workflow run and imported into your own PostgreSQL database for comparative analysis.

### 1. Create the tracking table

The schema is auto-generated to match the current `pg_track_optimizer` view definition. Download the schema file: [schema](job-pass/YYYY-MM-DD-HHMMSS/schema.sql)

### 2. Import the CSV artifact

```
\copy job_tracking_data FROM 'pg_track_optimizer_results.csv' CSV HEADER;
```

### 3. Example analysis queries

Find queries with the highest average error:
```sql
SELECT queryid, LEFT(query, 80) as query_preview, avg_avg, rms_avg, twa_avg, wca_avg
FROM job_tracking_data
ORDER BY avg_avg DESC
LIMIT 10;
```

Find queries appearing in the top 10 of all error metrics (most consistently problematic):
```sql
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
```
```

**Artifact Links Format**:
- Use relative paths from wiki root
- Pattern: `job-pass/YYYY-MM-DD-HHMMSS/filename.ext`
- All links must be enclosed in markdown link format: `[text](path)`

### 7. Footer

```markdown
---

_Automatically generated by [JO-Bench workflow](https://github.com/danolivo/pg_track_optimizer/actions/workflows/jo-bench.yml)_
```

## Workflow Implementation Requirements

The `jo-bench.yml` workflow and `generate-wiki-page.sh` script must:

### A. Generate Timestamp Variable

```bash
TIMESTAMP=$(date -u '+%Y-%m-%d-%H%M%S')  # e.g., 2026-01-05-084656
```

### B. Create Artifact Directory

```bash
ARTIFACT_DIR="${WIKI_DIR}/job-pass/${TIMESTAMP}"
mkdir -p "${ARTIFACT_DIR}"
```

### C. Copy and Compress Artifacts

```bash
# Copy CSV results
zip "${ARTIFACT_DIR}/pg_track_optimizer_jobench_results_pass1.zip" \
    /tmp/pg_track_optimizer_results_pass1.csv

zip "${ARTIFACT_DIR}/pg_track_optimizer_jobench_results_pass2.zip" \
    /tmp/pg_track_optimizer_results_pass2.csv

# Copy PostgreSQL log
zip "${ARTIFACT_DIR}/postgresql_log.zip" ~/pgdata/logfile

# Copy schema SQL
cp "${SCHEMA_FILE}" "${ARTIFACT_DIR}/schema.sql"
```

### D. Generate Report with Correct Links

All artifact links in the markdown must use the format:
```markdown
[text](job-pass/YYYY-MM-DD-HHMMSS/filename.ext)
```

### E. Extract Benchmark Commit

```bash
# Get jo-bench repository commit
cd ~/jo-bench
BENCH_COMMIT=$(git rev-parse HEAD)
```

Include in report metadata:
```markdown
- **Benchmark Source**: [`${BENCH_COMMIT:0:7}`](https://github.com/danolivo/jo-bench/commit/${BENCH_COMMIT})
```

### F. Simplified Results Query

The workflow should execute a simplified query that produces 3 columns only:
- `queryid`
- `query` (truncated/formatted with query filename comment)
- `error` (simple average error)

Format as psql output (not markdown table) and wrap in triple backticks.

### G. Wiki Page Naming

Create page with name pattern:
```bash
WIKI_PAGE="report-${TIMESTAMP}.md"
```

### H. Update Home Page Link

Add link to Home.md in the JO-Bench results section:
```bash
sed -i "/## JO-Bench Performance Test Results/a\\- [${TEST_DATE}](report-${TIMESTAMP})" Home.md
```

### I. Git Operations

```bash
cd "${WIKI_DIR}"
git add "report-${TIMESTAMP}.md" \
        "job-pass/${TIMESTAMP}/" \
        "Home.md"
git commit -m "Add JO-Bench results for ${TEST_DATE}"
git push origin master
```

## Validation Checklist

Before publishing a report, verify:

- [ ] Report filename matches `report-YYYY-MM-DD-HHMMSS.md` pattern
- [ ] Artifact directory exists: `job-pass/YYYY-MM-DD-HHMMSS/`
- [ ] All 4 artifact files present and properly compressed/formatted
- [ ] Report title is "Join Order Benchmark Test Results" (not "JO-Bench")
- [ ] Test Environment has all 6 metadata fields
- [ ] PostgreSQL Version shows full version string
- [ ] All commit links use 7-char short hash display, full hash in URL
- [ ] Both Pass 1 and Pass 2 result sections present
- [ ] Results table uses PostgreSQL psql format (not markdown table)
- [ ] Results table has exactly 3 columns
- [ ] Column Description documents only the 3 shown columns
- [ ] Artifact links use relative paths: `job-pass/YYYY-MM-DD-HHMMSS/...`
- [ ] All artifact links enclosed in markdown format: `[text](path)`
- [ ] Footer contains workflow attribution link
- [ ] Home.md updated with link to new report
- [ ] Benchmark Source commit included in metadata

## Future Improvements

- [ ] Implement Pass 1 results generation (currently shows "TODO")
- [ ] Add comparative analysis between Pass 1 and Pass 2
- [ ] Consider adding summary statistics section
- [ ] Explore visualization options for error metrics comparison

# Extra Requirements for Claude

- Follow this document as strictly as possible.
- Always ask questions if you're not sure about your decision.
- Keep report page as laconic as possible. In case of wordy material prefer to fold it into an attachement.
- Check workflows on consistency with this document.
- Re-check consistency of this document before using it.
- Never change this document automatically.
