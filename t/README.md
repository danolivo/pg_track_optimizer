# TAP Tests for pg_track_optimizer

This directory contains TAP (Test Anything Protocol) tests for pg_track_optimizer.

## Why TAP Tests?

TAP tests allow us to test scenarios that require:
- Server restarts
- Configuration changes between server starts
- File system operations
- Multi-session testing

These capabilities are not available in standard SQL regression tests.

## Available Tests

### 001_flush_load.pl

**Purpose:** Test the flush-to-disk and load-from-disk functionality by actually restarting the PostgreSQL server.

**What it tests:**
1. Creates tracking data by executing queries
2. Flushes statistics to disk using `pg_track_optimizer_flush()`
3. **Restarts the PostgreSQL server** (triggers `_load_hash_table()`)
4. Verifies all data was loaded correctly after restart
5. Executes additional queries to verify statistics accumulate correctly
6. Tests a second flush/restart cycle for repeatability

**Why this test is critical:**

The SQL regression test `sql/flush_load.sql` cannot actually restart the server, so it doesn't truly test the disk load path. This TAP test exposes the EOF handling bug in `_load_hash_table()` by actually triggering a server restart.

**Bug reproduced:**

Without the fix in `FIX_EOF_BUG.patch`, this test will fail because:
- `_load_hash_table()` uses `while (!feof(file))` which is incorrect
- After reading the last valid record, `feof()` is still false
- The code attempts one more read, which fails
- Goes to `read_error` instead of completing successfully
- Statistics are not loaded after restart

**Expected behavior (after fix):**
```
ok 1 - Should have 3 tracked queries before flush
ok 2 - Should have 3 total executions before flush
ok 3 - Flush file should exist after flush
ok 4 - Flush file should not be empty
ok 5 - Should have 3 queries loaded after restart
ok 6 - Should have 3 total executions after restart
ok 7 - Query should have 2 executions (1 before restart + 1 after)
ok 8 - All 3 queries should be present after restart
ok 9 - Should not have file read errors in log
ok 10 - Should not have any pg_track_optimizer errors in log
ok 11 - Should still have 3 queries after second restart
```

**Current behavior (with bug):**
```
ok 1 - Should have 3 tracked queries before flush
ok 2 - Should have 3 total executions before flush
ok 3 - Flush file should exist after flush
ok 4 - Flush file should not be empty
not ok 5 - Should have 3 queries loaded after restart
# got: '0'
# expected: '3'
```

The server log will show:
```
ERROR:  [pg_track_optimizer] could not read file "pg_track_optimizer.stat": Success
```

## Running TAP Tests

### Run all TAP tests:
```bash
make USE_PGXS=1 check
```

### Run only TAP tests (skip SQL regression):
```bash
make USE_PGXS=1 prove_check
```

### Run a specific TAP test:
```bash
make USE_PGXS=1 prove_check PROVE_TESTS='t/001_flush_load.pl'
```

### Run with verbose output:
```bash
PROVE_FLAGS='--verbose' make USE_PGXS=1 prove_check
```

## Requirements

TAP tests require:
- PostgreSQL compiled with `--enable-tap-tests` (default in most distributions)
- Perl modules: `PostgreSQL::Test::Cluster`, `PostgreSQL::Test::Utils`, `Test::More`
- IPC::Run Perl module

On Ubuntu/Debian:
```bash
sudo apt-get install libtest-simple-perl libipc-run-perl
```

## Debugging Failed Tests

If a TAP test fails:

1. Check the test output for which assertion failed
2. Look in `tmp_check/log/` for PostgreSQL logs
3. The test creates a temporary cluster that's cleaned up after the test
4. Add `note()` statements to the test for additional debugging output

## Writing New TAP Tests

New TAP tests should:
- Be numbered sequentially (002_xxx.pl, 003_xxx.pl, etc.)
- Use `PostgreSQL::Test::Cluster` for cluster management
- Clean up resources (tables, extensions) after testing
- Include descriptive `note()` statements for debugging
- Test one specific feature or bug fix

Example skeleton:
```perl
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('testnode');
$node->init;
$node->start;

# Your tests here

$node->stop;
done_testing();
```
