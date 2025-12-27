# Current vs Framework Architecture - Visual Comparison

## Current Monolithic Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    ExecutorEnd Hook                         │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│               plan_error.c (HARDCODED)                      │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ prediction_walker()                                   │ │
│  │  ├─ Compute avg_error = |log(actual/plan)|          │ │
│  │  ├─ Compute rms_error = error²                       │ │
│  │  ├─ Compute twa_error = error × time_weight          │ │
│  │  ├─ Compute wca_error = error × cost_weight          │ │
│  │  └─ Update PlanEstimatorContext (FIXED STRUCT)       │ │
│  └───────────────────────────────────────────────────────┘ │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ plan_error()                                          │ │
│  │  ├─ Compute blks_accessed from BufferUsage           │ │
│  │  ├─ Compute local_blks from BufferUsage              │ │
│  │  ├─ Average all errors over nnodes                   │ │
│  │  └─ Return PlanEstimatorContext (FIXED 7 METRICS)    │ │
│  └───────────────────────────────────────────────────────┘ │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│            store_data() (HARDCODED)                         │
│  ├─ Find/create DSMOptimizerTrackerEntry                   │
│  │   (FIXED SIZE: 224 bytes for 7 RStats)                  │
│  ├─ rstats_add_value(&entry->avg_error, ctx->avg_error)    │
│  ├─ rstats_add_value(&entry->rms_error, ctx->rms_error)    │
│  ├─ rstats_add_value(&entry->twa_error, ctx->twa_error)    │
│  ├─ rstats_add_value(&entry->wca_error, ctx->wca_error)    │
│  ├─ rstats_add_value(&entry->blks_accessed, ...)           │
│  ├─ rstats_add_value(&entry->local_blks, ...)              │
│  └─ rstats_add_value(&entry->exec_time, ...)               │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                  DSM Hash Table                             │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ DSMOptimizerTrackerEntry (FIXED STRUCT)              │  │
│  │  key: {dbOid, queryId}                               │  │
│  │  evaluated_nodes: int32                              │  │
│  │  plan_nodes: int32                                   │  │
│  │  avg_error: RStats      ← 32 bytes                   │  │
│  │  rms_error: RStats      ← 32 bytes                   │  │
│  │  twa_error: RStats      ← 32 bytes                   │  │
│  │  wca_error: RStats      ← 32 bytes                   │  │
│  │  blks_accessed: RStats  ← 32 bytes                   │  │
│  │  local_blks: RStats     ← 32 bytes                   │  │
│  │  exec_time: RStats      ← 32 bytes                   │  │
│  │  nexecs: int64                                        │  │
│  │  query_ptr: dsa_pointer                              │  │
│  │  TOTAL: ~280 bytes/query (ALL METRICS ALWAYS)        │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘

PAIN POINTS:
✗ Adding metric requires modifying 8+ files
✗ All 7 metrics always computed (CPU overhead)
✗ All 7 metrics always stored (280 bytes/query)
✗ No user configuration
✗ No third-party extensibility
```

---

## Proposed Framework Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    ExecutorEnd Hook                         │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              Metric Framework (DYNAMIC)                     │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ GetRegisteredMetrics() → Array of MetricDescriptors   │ │
│  │                                                        │ │
│  │  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐ │ │
│  │  │ avg_error    │  │ twa_error    │  │ custom_     │ │ │
│  │  │ .enabled=true│  │ .enabled=true│  │ .enabled=   │ │ │
│  │  │ .compute()   │  │ .compute()   │  │  false      │ │ │
│  │  └──────────────┘  └──────────────┘  └─────────────┘ │ │
│  │                                                        │ │
│  │  Only enabled metrics are computed ✓                  │ │
│  └───────────────────────────────────────────────────────┘ │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ ComputeMetrics(context) → MetricValue[]               │ │
│  │  FOR EACH enabled metric:                             │ │
│  │    value = metric->compute(context)                   │ │
│  │    if (value.is_valid)                                │ │
│  │      values[i] = value                                │ │
│  └───────────────────────────────────────────────────────┘ │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│         Metric Plugins (INDEPENDENT MODULES)                │
│                                                             │
│  ┌────────────────────┐  ┌────────────────────┐            │
│  │ metrics/           │  │ custom extensions/ │            │
│  │  avg_error.c       │  │  hash_agg_count.c  │            │
│  │  rms_error.c       │  │  cache_hit_ratio.c │            │
│  │  twa_error.c       │  │  spill_detector.c  │            │
│  │  wca_error.c       │  │  ...               │            │
│  │  blks_accessed.c   │  └────────────────────┘            │
│  │  local_blks.c      │                                    │
│  │  exec_time.c       │   Each metric is:                  │
│  └────────────────────┘   ✓ Self-contained                 │
│                           ✓ Independently testable          │
│                           ✓ Optional                        │
│                           ✓ Configurable                    │
└─────────────────────────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│            StoreMetrics() (DYNAMIC)                         │
│  ├─ Allocate entry with VARIABLE SIZE:                     │
│  │   size = header + Σ(enabled_metrics[i].storage_size)    │
│  │                                                          │
│  │   Example configurations:                               │
│  │   • "avg_error,exec_time"     → 96 bytes  (66% less!)   │
│  │   • "all"                     → 280 bytes (same)        │
│  │   • "avg_error,rms_error,twa" → 128 bytes (54% less!)   │
│  │                                                          │
│  ├─ FOR EACH enabled metric:                               │
│  │   storage_ptr = get_metric_storage(entry, metric)       │
│  │   store_value(storage_ptr, values[i])                   │
│  └─ Only store enabled metrics ✓                           │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              DSM Hash Table (VARIABLE SIZE)                 │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ DSMOptimizerTrackerEntryHeader (FIXED)               │  │
│  │  key: {dbOid, queryId}                               │  │
│  │  evaluated_nodes: int32                              │  │
│  │  plan_nodes: int32                                   │  │
│  │  nexecs: int64                                        │  │
│  │  metrics_offset: uint32                              │  │
│  │  metrics_size: uint32                                │  │
│  │  query_ptr: dsa_pointer                              │  │
│  ├──────────────────────────────────────────────────────┤  │
│  │ VARIABLE METRIC STORAGE (configured at runtime)      │  │
│  │                                                       │  │
│  │  Configuration: "avg_error,exec_time"                │  │
│  │    avg_error: RStats   ← 32 bytes                    │  │
│  │    exec_time: RStats   ← 32 bytes                    │  │
│  │    TOTAL: 96 bytes (66% reduction!)                  │  │
│  │                                                       │  │
│  │  Configuration: "all"                                │  │
│  │    avg_error: RStats      ← 32 bytes                 │  │
│  │    rms_error: RStats      ← 32 bytes                 │  │
│  │    twa_error: RStats      ← 32 bytes                 │  │
│  │    wca_error: RStats      ← 32 bytes                 │  │
│  │    blks_accessed: RStats  ← 32 bytes                 │  │
│  │    local_blks: RStats     ← 32 bytes                 │  │
│  │    exec_time: RStats      ← 32 bytes                 │  │
│  │    TOTAL: 280 bytes (backward compatible)            │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘

BENEFITS:
✓ Add metric = create 1 plugin file (no core changes)
✓ Only enabled metrics computed (CPU savings)
✓ Only enabled metrics stored (memory savings)
✓ User-configurable via GUC
✓ Third-party extensions can add metrics
✓ Backward compatible (default config = current behavior)
```

---

## Adding a New Metric: Before vs After

### Before (Monolithic)

```
1. Edit plan_error.h
   └─ Add field to PlanEstimatorContext

2. Edit plan_error.c
   └─ Add computation logic in prediction_walker()
   └─ Add finalization in plan_error()

3. Edit pg_track_optimizer.c
   └─ Add field to DSMOptimizerTrackerEntry
   └─ Add storage logic in store_data()
   └─ Update pg_track_optimizer() function signature
   └─ Update flush/load serialization

4. Edit pg_track_optimizer--0.1.sql
   └─ Update CREATE FUNCTION pg_track_optimizer()
   └─ Update CREATE VIEW pg_track_optimizer

5. Update expected/ test outputs

6. Bump DATA_FORMAT_VERSION

7. Test all 7 existing metrics for regressions

8. Hope nothing breaks

TOTAL: 8+ file modifications, high regression risk
```

### After (Framework)

```
1. Create metrics/my_metric.c:

   #include "metric_framework.h"

   static MetricValue
   my_metric_compute(const MetricContext *ctx)
   {
       MetricValue result = {0};
       result.storage_type = METRIC_STORAGE_RSTATS;
       result.is_valid = true;

       /* Your computation here */
       result.value.double_val = compute_something(ctx);

       return result;
   }

   MetricDescriptor my_metric = {
       .name = "my_metric",
       .description = "Does something useful",
       .type = METRIC_TYPE_PER_NODE,
       .storage = METRIC_STORAGE_RSTATS,
       .compute = my_metric_compute,
       .enabled_default = false,
       .priority = 90,
       .storage_size = sizeof(RStats)
   };

2. Register in _PG_init():
   RegisterMetric(&my_metric);

3. Enable via GUC:
   SET pg_track_optimizer.metrics = 'avg_error,my_metric';

4. Done!

TOTAL: 1 file creation, zero core changes, isolated testing
```

---

## Configuration Examples

### Production: Minimal Overhead

```sql
-- Track only essential metrics
SET pg_track_optimizer.metrics = 'avg_error,exec_time';

Memory impact:
  Before: 280 bytes × 10,000 queries = 2.8 MB
  After:  96 bytes × 10,000 queries  = 0.96 MB
  Savings: 66% reduction (1.84 MB saved)

CPU impact:
  Before: Compute 7 metrics × 20 nodes = 140 computations/query
  After:  Compute 2 metrics × 20 nodes = 40 computations/query
  Savings: 71% reduction
```

### Development: Full Analysis

```sql
-- Track everything + custom metrics
LOAD 'pg_track_custom_metrics';
SET pg_track_optimizer.metrics = 'all,hash_agg_count,spill_detector';

Memory impact:
  Before: 280 bytes × 1,000 queries = 280 KB
  After:  344 bytes × 1,000 queries = 344 KB
  Cost:   23% increase (64 KB more for 2 custom metrics)

Value: Custom insights into hash aggregations and work_mem spills
```

### Problem-Specific: I/O Investigation

```sql
-- Focus on I/O metrics only
SET pg_track_optimizer.metrics = 'blks_accessed,local_blks';

Memory impact:
  Before: 280 bytes × 50,000 queries = 14 MB
  After:  96 bytes × 50,000 queries  = 4.8 MB
  Savings: 66% reduction (9.2 MB saved)

Benefit: Identify work_mem issues without estimation error overhead
```

---

## Implementation Phases

```
Phase 1: Framework Core (2 weeks)
┌────────────────────────────────────┐
│ • metric_framework.h/c             │
│ • MetricDescriptor system          │
│ • Registration API                 │
│ • Configuration GUC                │
│ • Backward compatibility mode      │
└────────────────────────────────────┘
         │
         ▼
Phase 2: Migrate Built-ins (1 week)
┌────────────────────────────────────┐
│ • Convert 7 existing metrics       │
│ • Regression testing               │
│ • Performance benchmarking         │
│ • Documentation updates            │
└────────────────────────────────────┘
         │
         ▼
Phase 3: Dynamic Storage (1 week)
┌────────────────────────────────────┐
│ • Variable-size DSM entries        │
│ • Updated serialization            │
│ • File format version bump         │
│ • Migration path testing           │
└────────────────────────────────────┘
         │
         ▼
Phase 4: Release & Ecosystem (ongoing)
┌────────────────────────────────────┐
│ • Developer guide                  │
│ • Example custom metrics           │
│ • Community metric repository      │
│ • Third-party integration          │
└────────────────────────────────────┘
```

---

## Code Size Comparison

### Current: Hardcoded Approach
```
plan_error.c:         262 lines (includes all 7 metrics)
pg_track_optimizer.c: 1100 lines (includes storage for all metrics)
pg_track_optimizer.sql: 200 lines (hardcoded view)
TOTAL: 1562 lines, highly coupled
```

### Framework: Modular Approach
```
metric_framework.c:    450 lines (reusable core)
metrics/avg_error.c:    45 lines (independent)
metrics/rms_error.c:    42 lines (independent)
metrics/twa_error.c:    48 lines (independent)
metrics/wca_error.c:    48 lines (independent)
metrics/blks_accessed.c: 35 lines (independent)
metrics/local_blks.c:   38 lines (independent)
metrics/exec_time.c:    32 lines (independent)
plan_error.c:          180 lines (simplified)
pg_track_optimizer.c:  950 lines (simplified)
TOTAL: 1868 lines, loosely coupled

Net increase: 306 lines (19%)
Value: Full extensibility, modularity, user control
```

---

## Compatibility Matrix

| Scenario | Current | Framework | Notes |
|----------|---------|-----------|-------|
| Default behavior | 7 metrics | 7 metrics | `metrics='all'` |
| Memory usage | 280 bytes | 96-344 bytes | User configurable |
| Add new metric | 8+ file changes | 1 new file | Zero core impact |
| Third-party metrics | Impossible | Supported | Via extensions |
| Disable metric | Impossible | `SET metrics='...'` | Runtime config |
| Test isolation | All or nothing | Per-metric | Independent |
| Backward compat | N/A | 100% | Default matches current |

---

## Success Metrics

### Technical Goals
- ✓ 50-70% memory reduction with minimal config
- ✓ 40-60% CPU reduction with minimal config
- ✓ Zero-change addition of new metrics
- ✓ 100% backward compatibility
- ✓ < 5% overhead for framework itself

### User Experience Goals
- ✓ Simple GUC configuration (`SET metrics='...'`)
- ✓ Auto-generated SQL interface (no manual updates)
- ✓ Clear error messages for invalid configs
- ✓ Examples for common use cases
- ✓ Migration path documentation

### Developer Experience Goals
- ✓ < 50 lines of code for new metric
- ✓ Independent testing per metric
- ✓ Reusable framework functions
- ✓ Clear API documentation
- ✓ Example metrics as templates

---

## Conclusion

The framework architecture transforms `pg_track_optimizer` from a **fixed-function extension** into a **customizable platform**.

**For users:** Control overhead, focus on relevant metrics
**For developers:** Add metrics without forking, independent testing
**For maintainers:** Modular codebase, easier to extend and debug

**Investment:** 2-4 weeks development
**Payoff:** Infinite extensibility, happier users, vibrant ecosystem
