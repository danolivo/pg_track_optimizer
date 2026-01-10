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
   - Empty state has exactly one valid serialization: `(count:0,mean:0,min:0,max:0,variance:0)`
   - Enables byte-for-byte comparison of empty values
   - Simplifies debugging and testing

2. **Clean Display**
   - Text output shows intuitive zeros: `(count:0,mean:0,min:0,max:0,variance:0)`
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

### References

- **Welford's Algorithm**: https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
- **PostgreSQL Type System**: https://www.postgresql.org/docs/current/xtypes.html
- **Error Codes**: https://www.postgresql.org/docs/current/errcodes-appendix.html

---

*Last Updated: 2026-01-10*
*Author: Andrei Lepikhov*
*Reviewers: Claude Code Review*
