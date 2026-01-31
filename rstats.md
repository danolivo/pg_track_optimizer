# RStats Type Documentation

## Overview

The `RStats` type implements numerically stable running statistics using Welford's algorithm. This document describes the design decisions, implementation details, and rationale for the sentinel value approach used to represent empty state.

## Sentinel Value Design

### Design Choice: Zero Sentinels

The `RStats` type uses **count=0 with all other fields set to 0.0** as the canonical empty state representation.

### Rationale

#### Why Count=0 is Primary
- **Unambiguous**: count=0 is the natural definition of "no data accumulated"
- **Simple**: Single boolean check `(count == 0)` for emptiness
- **No conflicts**: Cannot collide with legitimate statistics (any real data has count >= 1)

#### Why All Fields Must Be Zero When Empty
While count=0 alone is sufficient to identify empty state, we enforce that all other fields (mean, m2, min, max) must also be 0.0 when count=0. This provides:

1. **Canonical Representation**
   - Empty state has exactly one valid serialization: `(count:0,mean:0,min:0,max:0,stddev:0)`
   - Enables byte-for-byte comparison of empty values
   - Simplifies debugging and testing

2. **Clean Display**
   - Text output shows intuitive zeros: `(count:0,mean:0,min:0,max:0,stddev:0)`
   - No confusing sentinel values like -1 that might suggest data

3. **Corruption Detection**
   - If count=0 but other fields are non-zero, we know data is corrupted
   - Catches bugs in serialization/deserialization
   - Detects memory corruption or improper initialization

4. **Consistency Across Formats**
   - Binary and text formats produce identical empty representations
   - No format-specific edge cases

### Implementation: Defense in Depth

The canonical empty state (count=0, all zeros) is enforced at four validation points:

#### 1. Text Input Validation (`rstats_in`)

**Purpose**: Reject malformed text input from users or external systems
**When**: Parsing SQL input like `SELECT '(count:0,mean:1,...)'::rstats`
**Error Code**: `ERRCODE_INVALID_TEXT_REPRESENTATION`

#### 2. Binary Input Validation (`rstats_recv`)

**Purpose**: Catch corruption in network protocol or disk storage
**When**: Receiving binary data from client or reading from disk
**Error Code**: `ERRCODE_DATA_CORRUPTED`

#### 3. Binary Output Validation (`rstats_send`)

**Purpose**: Catch bugs in C code that corrupts RStats in memory
**When**: Before sending binary data to client
**Error Code**: `ERRCODE_DATA_CORRUPTED`
**Note**: Includes hint that this is an internal bug

#### 4. Runtime Emptiness Check (`rstats_is_empty`)

**Purpose**: Guard any code path checking if statistics are empty
**When**: Called by `rstats_add_value()` to detect lazy initialization
**Error Code**: `ERRCODE_DATA_CORRUPTED`

### Trade-offs and Limitations

#### Cannot Distinguish Empty vs. Init(0)

The current design cannot distinguish between:
- Truly empty statistics: `rstats_set_empty()`
- Statistics initialized with single zero value: `rstats_init_internal(&stats, 0.0)`

Both result in: count=1 for init(0), but count=0 for empty.

**This is acceptable because:**
1. The count field correctly reflects the difference (0 vs. 1)
2. Both are valid states with clear semantics
3. The distinction is not operationally important
4. User can check count if they need to distinguish

### Memory Layout

```c
typedef struct RStats {
    int64   count;  /* 8 bytes - Primary empty indicator */
    double  mean;   /* 8 bytes - Must be 0.0 when count=0 */
    double  m2;     /* 8 bytes - Must be 0.0 when count=0 */
    double  min;    /* 8 bytes - Must be 0.0 when count=0 */
    double  max;    /* 8 bytes - Must be 0.0 when count=0 */
} RStats;
/* Total: 40 bytes, fixed-size, no varlena header */
```

### Performance Considerations

The sentinel validation has minimal performance impact:

- **Validation Cost**: 4 floating-point equality comparisons
- **Compiler Optimization**: Often optimized to SIMD comparison
- **Zero Overhead in Happy Path**: If count > 0, skip validation entirely
- **Only on Boundaries**: Validation only at I/O and emptiness checks, not during computation

### Compatibility and Migration

#### Future Changes

If sentinel approach needs modification in the future:

1. Update this documentation with rationale
2. Increment type version (create `rstats--0.1--0.2.sql` migration script)
3. Add compatibility layer if needed
4. Update all four validation points consistently

## RStats Operators and Functions

This section documents the available operators and their semantics.

### Type Casts

#### Numeric to RStats

Implicit casts from numeric types initialize RStats with a single value:

```sql
-- From double precision
SELECT 42.5::rstats;
-- Result: (count:1,mean:42.5,min:42.5,max:42.5,stddev:0)

-- From integer
SELECT 100::rstats;
-- Result: (count:1,mean:100,min:100,max:100,stddev:0)

-- From numeric
SELECT 3.14159::numeric::rstats;
-- Result: (count:1,mean:3.14159,min:3.14159,max:3.14159,stddev:0)
```

**Semantics**: Creates RStats initialized with a single data point.

#### Binary Serialization Casts

Assignment casts for binary serialization (backup/restore):

```sql
-- RStats to bytea (serialization)
SELECT rstats()::bytea;
-- Result: \x0000000000000000...

-- Bytea to RStats (deserialization)
SELECT '\x0000000000000000...'::bytea::rstats;
-- Result: (count:0,mean:0,min:0,max:0,stddev:0)

-- Round-trip preserves state
SELECT (10::rstats + 20 + 30)::bytea::rstats;
-- Result: (count:3,mean:20,min:10,max:30,stddev:10)
```

**Semantics**: Enables binary backup/restore and external storage.

### Addition Operator (+)

Accumulates a new value into running statistics:

```sql
-- Add double precision
SELECT 10.0::rstats + 20.0 + 30.0;
-- Result: (count:3,mean:20,min:10,max:30,stddev:10)

-- Add integer
SELECT 5::rstats + 10 + 15;
-- Result: (count:3,mean:10,min:5,max:15,stddev:5)

-- Chain multiple additions
SELECT ((10.0::rstats + 20.0) + 30.0) + 40.0;
-- Result: (count:4,mean:25,min:10,max:40,stddev:12.91)
```

**Semantics**:
- Updates mean, stddev (via m2), min, max using Welford's algorithm
- Order-dependent: accumulates values sequentially
- Deterministic: same sequence → same result
- Handles NULL gracefully: `stats + NULL` returns NULL

### Equality Operator (=)

Tests for exact identity (bit-identical state):

```sql
-- Exact match: same sequence
SELECT (10::rstats + 20 + 30) = (10::rstats + 20 + 30);
-- Result: true

-- Different sequence, same statistics
SELECT (10::rstats + 20) = (15::rstats + 15);
-- Result: false (mean is same, but internal state differs)

-- Empty state comparison
SELECT rstats() = rstats();
-- Result: true
```

**Semantics**:
- **Identity, not numerical equivalence**: Tests if two RStats accumulated the exact same sequence of values
- **Deterministic algorithm**: Welford's algorithm guarantees same inputs → bit-identical state
- **Exact float comparison**: No epsilon tolerance (by design)
- **Use cases**: Caching, deduplication, identity checks in hash tables

**For numerical similarity**, compare extracted statistics:
```sql
-- Check if means are approximately equal
SELECT ABS((stats1 -> 'mean') - (stats2 -> 'mean')) < 0.001;
```

### Distance Operator (<->)

Calculates the Mahalanobis distance between two statistical distributions:

```sql
-- Compare two distributions
SELECT (10::rstats + 20 + 30) <-> (15::rstats + 25 + 35);
-- Result: small value (similar distributions)

SELECT (10::rstats + 20 + 30) <-> (100::rstats + 200 + 300);
-- Result: larger value (dissimilar distributions)

-- Find queries with statistics most different from a reference
SELECT queryid, stats <-> reference_stats AS distance
FROM pg_track_optimizer, (SELECT 0::rstats + 1 + 2 AS reference_stats) ref
ORDER BY distance DESC;
```

**Semantics**:
- Returns `double precision` representing statistical distance
- Lower values indicate more similar distributions
- Commutative: `a <-> b` equals `b <-> a`
- Useful for clustering queries by statistical similarity or detecting outliers

### Field Accessor Operator (->)

Extracts statistical properties as double precision:

```sql
SELECT
    stats -> 'count' AS count,
    stats -> 'mean' AS mean,
    stats -> 'stddev' AS stddev,
    stats -> 'min' AS min,
    stats -> 'max' AS max
FROM (SELECT (10::rstats + 20 + 30) AS stats) t;
```

**Available Fields**:
- `count`: Number of accumulated values (int64 → float8)
- `mean`: Arithmetic mean
- `stddev`: Sample standard deviation (n-1 denominator)
- `min`: Minimum value observed
- `max`: Maximum value observed

**Semantics**:
- Returns `double precision` (cast as needed)
- Stddev uses sample formula: `sqrt(m2 / (count - 1))`
- Returns 0 stddev for count ≤ 1
- Works with expression indexes: `CREATE INDEX ON table ((stats -> 'mean'))`

### Constructor Functions

#### Empty Constructor

```sql
SELECT rstats();
-- Result: (count:0,mean:0,min:0,max:0,stddev:0)
```

**Semantics**: Creates canonical empty state.

#### Polymorphic Constructor

```sql
-- Accepts any numeric-convertible type
SELECT rstats(42::int2);
SELECT rstats(3.14::float4);
SELECT rstats(100::bigint);
```

**Semantics**: Initializes with a single value (delegates to type-specific casts).

### Aggregate Function

#### rstats_agg(double precision)

Aggregates multiple values into a single RStats object:

```sql
-- Aggregate values from a table column
SELECT rstats_agg(value) FROM measurements;
-- Result: (count:N,mean:...,min:...,max:...,stddev:...)

-- Aggregate with grouping
SELECT category, rstats_agg(price) AS price_stats
FROM products
GROUP BY category;

-- Combine with field accessor
SELECT
    category,
    rstats_agg(price) -> 'mean' AS avg_price,
    rstats_agg(price) -> 'stddev' AS price_stddev
FROM products
GROUP BY category;
```

**Semantics**:
- Collects all non-NULL values in the group into running statistics
- Returns canonical empty state if no values are aggregated
- Uses Welford's algorithm internally for numerical stability
- Order of aggregation may affect floating-point rounding (typically negligible)

### References

- **Welford's Algorithm**: https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
- **PostgreSQL Type System**: https://www.postgresql.org/docs/current/xtypes.html
- **Error Codes**: https://www.postgresql.org/docs/current/errcodes-appendix.html

---

*Last Updated: 2026-01-10*
*Author: Andrei Lepikhov*
*Reviewers: Claude Code Review*
