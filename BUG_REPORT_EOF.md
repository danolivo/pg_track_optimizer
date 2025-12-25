# Bug Report: EOF Handling in _load_hash_table()

## Summary
The `_load_hash_table()` function in `pg_track_optimizer.c` (lines 786-867) contains a critical bug in EOF detection that will cause data load failures in production builds and assertion failures in debug builds.

## Location
File: `pg_track_optimizer.c`
Function: `_load_hash_table()`
Lines: 786-867 (specifically line 786)

## The Bug

```c
while (!feof(file))
{
    if (fread(&disk_entry, sizeof(DSMOptimizerTrackerEntry), 1, file) != 1)
        goto read_error;
    // ... process entry ...
}

Assert(counter == nrecs);  // Line 837
```

## Problem Explanation

The `feof()` function only returns `true` **AFTER** a read operation has already failed due to EOF. This creates the following sequence:

1. Last valid record is read successfully
2. Loop continues (because `feof()` is still `false`)
3. `fread()` attempts to read beyond EOF
4. `fread()` returns 0 (failure)
5. Code jumps to `read_error` instead of cleanly exiting
6. Assertion `Assert(counter == nrecs)` at line 837 is never reached

## Impact

### Debug Builds (with assertions enabled)
- The loop will process all records but then attempt one more read
- The extra read fails, goes to `read_error`
- File never completes loading
- ERROR reported: "could not read file"
- Data is lost

### Production Builds (assertions disabled)
- Same behavior as debug, but no assertion check
- File loading always fails with read error
- Extension appears to work but persistence is broken
- Users get error: `[pg_track_optimizer] could not read file "pg_track_optimizer.stat": %m`

## Correct Implementation

The standard pattern for reading a known number of records is:

```c
for (uint32 i = 0; i < nrecs; i++)
{
    if (fread(&disk_entry, sizeof(DSMOptimizerTrackerEntry), 1, file) != 1)
        goto read_error;

    // ... process entry ...
    counter++;
}

Assert(counter == nrecs);
```

Alternatively, if using while loop:

```c
while (counter < nrecs)
{
    if (fread(&disk_entry, sizeof(DSMOptimizerTrackerEntry), 1, file) != 1)
        goto read_error;

    // ... process entry ...
    counter++;
}
```

## How to Reproduce

### TAP Test (Recommended)

Run the TAP test that actually restarts the server:

```bash
make USE_PGXS=1 prove_check PROVE_TESTS='t/001_flush_load.pl'
```

This test:
1. Creates tracking data
2. Flushes to disk
3. **Restarts the PostgreSQL server** (triggers `_load_hash_table()`)
4. Verifies data was loaded correctly

### SQL Regression Test (Limited)

Note: The SQL test `sql/flush_load.sql` doesn't actually restart the server, so it only partially tests the scenario:

```bash
make USE_PGXS=1 installcheck TESTS=flush_load
```

### Expected Behavior (after fix)
- 3 queries tracked before flush
- File written successfully
- Server restarted
- 3 queries loaded from file successfully
- TAP test passes with all assertions OK

### Actual Behavior (with bug)

TAP test output:
```
ok 1 - Should have 3 tracked queries before flush
ok 2 - Should have 3 total executions before flush
ok 3 - Flush file should exist after flush
ok 4 - Flush file should not be empty
not ok 5 - Should have 3 queries loaded after restart
#   Failed test 'Should have 3 queries loaded after restart'
#          got: '0'
#     expected: '3'
```

PostgreSQL log shows:
```
ERROR:  [pg_track_optimizer] could not read file "pg_track_optimizer.stat": Success
```

In production builds:
- Same error, but harder to debug
- File appears corrupt even though it's valid
- No assertions to catch the problem

## Proof of Bug

The code reads the header which contains `nrecs` (number of records):

```c
fread(&nrecs, sizeof(uint32), 1, file)  // Line 777
```

Then uses `while (!feof(file))` instead of `while (counter < nrecs)`.

Since `feof()` only becomes true **after** a failed read, the code will:
- Read all `nrecs` records successfully (counter == nrecs)
- Continue looping (feof still false)
- Attempt to read record `nrecs+1`
- Fail and goto read_error
- Never reach the assertion

## Additional Evidence

From the code at lines 783-784:
```c
Assert(nrecs >= 0);

while (!feof(file))
```

The `nrecs` value is available but **not used** for loop control. This is a clear coding error.

## Related Issues

This bug is related to Issue #16 in the security audit: **DATA_FORMAT_VERSION not incremented** when `local_blks` field was added. Combined, these bugs mean:

1. Old files (without `local_blks`) will fail to load due to EOF bug
2. Even if EOF bug is fixed, old files will corrupt memory due to structure size mismatch
3. VERSION should be incremented to 2 and load code should handle version 1 files

## Severity

**CRITICAL** - This bug makes the flush/load feature completely non-functional:
- Data cannot be persisted across restarts
- Defeats the purpose of `pg_track_optimizer_flush()`
- Silent failure in production (logs show "could not read file")
- Users lose all accumulated statistics

## Recommendation

**Immediate action required:**

1. Fix the EOF detection loop (use `for` loop with `nrecs`)
2. Increment `DATA_FORMAT_VERSION` to 2
3. Add version migration code for version 1 files (if needed)
4. Add regression test `flush_load.sql` to CI/CD pipeline
5. Document in release notes that old persisted files must be flushed before upgrade

## Test Cases

### TAP Test: `t/001_flush_load.pl`
The primary test that fully reproduces the bug:
1. Creates tracked queries
2. Flushes to disk
3. **Restarts PostgreSQL server** (triggers `_load_hash_table()`)
4. Verifies data was loaded correctly
5. Tests repeatability with second flush/restart cycle
6. Checks server logs for errors

This is the definitive test case that proves the bug exists.

### SQL Test: `sql/flush_load.sql`
A limited test that demonstrates the flush operation but doesn't restart the server:
1. Creates tracked queries
2. Flushes to disk
3. Drops and recreates extension (not the same as server restart)
4. Verifies data persistence

Note: This SQL test is useful for basic flush testing but doesn't fully trigger the load path.
