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

Run the regression test `sql/flush_load.sql`:

```bash
make USE_PGXS=1 installcheck TESTS=flush_load
```

### Expected Behavior (after fix)
- 3 queries tracked before flush
- File written successfully
- Extension dropped and recreated
- 3 queries loaded from file
- Test passes

### Actual Behavior (with bug)
In debug builds:
```
ERROR:  [pg_track_optimizer] could not read file "pg_track_optimizer.stat": Success
```

In production builds:
- Same error, but harder to debug
- File appears corrupt even though it's valid

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

## Test Case

The test case `sql/flush_load.sql` demonstrates:
1. Creating tracked queries
2. Flushing to disk
3. Dropping extension (clears memory)
4. Recreating extension (triggers load from disk)
5. Verifying data was loaded correctly

This is the minimal reproducible test case for this bug.
