# pg_track_optimizer Framework Architecture

## Executive Summary

This document proposes an extensible framework architecture that allows:
- **Users** to configure which metrics to track (reducing overhead and memory)
- **Developers** to add custom metrics with minimal code changes
- **Backward compatibility** with existing deployments

---

## Current Architecture Analysis

### 1. Current Hardcoded Metrics

**Location: `plan_error.h:28-51`**
```c
typedef struct PlanEstimatorContext {
    double  totaltime;
    double  totalcost;
    int     nnodes;
    int     counter;

    /* HARDCODED metrics */
    double  avg_error;
    double  rms_error;
    double  twa_error;
    double  wca_error;
    int64   blks_accessed;
    int64   local_blks;
} PlanEstimatorContext;
```

**Storage: `pg_track_optimizer.c:92-112`**
```c
typedef struct DSMOptimizerTrackerEntry {
    DSMOptimizerTrackerKey  key;
    int32                   evaluated_nodes;
    int32                   plan_nodes;

    /* HARDCODED RStats fields */
    RStats  avg_error;
    RStats  rms_error;
    RStats  twa_error;
    RStats  wca_error;
    RStats  blks_accessed;
    RStats  local_blks;
    RStats  exec_time;
    int64   nexecs;

    dsa_pointer query_ptr;
} DSMOptimizerTrackerEntry;
```

**Computation: `plan_error.c:184-198`**
```c
/* HARDCODED computation logic */
node_error = fabs(log(real_rows / plan_rows));
ctx->avg_error += node_error;
ctx->rms_error += node_error * node_error;
ctx->twa_error += node_error * relative_time;
ctx->wca_error += node_error * relative_cost;
```

### 2. Current Data Flow

```
┌─────────────────┐
│  ExecutorEnd    │
│    Hook         │
└────────┬────────┘
         │
         ▼
┌─────────────────────────┐
│  plan_error()           │
│  ├─ prediction_walker() │──┐
│  │   ├─ Compute errors  │  │ HARDCODED
│  │   └─ Update context  │  │
│  └─ Finalize averages   │──┘
└────────┬────────────────┘
         │
         ▼
┌─────────────────────────┐
│  store_data()           │
│  ├─ Find/create entry   │
│  └─ rstats_add_value()  │──┐ HARDCODED
└────────┬────────────────┘  │ for each metric
         │                    │
         ▼                    │
┌─────────────────────────┐  │
│  DSM Hash Table         │  │
│  (shared memory)        │◄─┘
└─────────────────────────┘
```

### 3. Critical Pain Points

1. **Adding new metric requires:**
   - Modify `PlanEstimatorContext` struct
   - Modify `DSMOptimizerTrackerEntry` struct
   - Add computation in `prediction_walker()`
   - Add finalization in `plan_error()`
   - Add storage in `store_data()`
   - Update SQL function `pg_track_optimizer()`
   - Update SQL view
   - Update disk format (version bump)
   - Update all serialization code

2. **User cannot disable expensive metrics:**
   - All 7 RStats fields always allocated (7 × 32 bytes = 224 bytes/query)
   - All metrics always computed (CPU overhead)
   - Cannot trade features for performance

---

## Proposed Framework Architecture

### Design Principles

1. **Plugin-based metrics** - Each metric is a self-contained module
2. **Configuration-driven** - Users select which metrics to track via GUC
3. **Registration API** - Developers register metrics at extension load
4. **Type-safe** - Strong typing for metric computation
5. **Backward compatible** - Default configuration matches current behavior

---

## Core Framework Components

### 1. Metric Descriptor System

**New file: `metric_framework.h`**

```c
#ifndef METRIC_FRAMEWORK_H
#define METRIC_FRAMEWORK_H

#include "executor/executor.h"
#include "nodes/plannodes.h"

/* Forward declarations */
typedef struct MetricDescriptor MetricDescriptor;
typedef struct MetricContext MetricContext;
typedef struct MetricValue MetricValue;

/*
 * Type of metric computation:
 * - PER_NODE: Computed for each plan node (avg_error, rms_error, etc.)
 * - PER_EXECUTION: Computed once per query execution (exec_time, blks_accessed)
 */
typedef enum MetricType
{
    METRIC_TYPE_PER_NODE,
    METRIC_TYPE_PER_EXECUTION
} MetricType;

/*
 * Storage type for metric:
 * - RSTATS: Running statistics (min/max/avg/stddev)
 * - COUNTER: Simple accumulator (int64)
 * - GAUGE: Latest value only (double)
 */
typedef enum MetricStorage
{
    METRIC_STORAGE_RSTATS,
    METRIC_STORAGE_COUNTER,
    METRIC_STORAGE_GAUGE
} MetricStorage;

/*
 * Metric computation context - passed to metric compute functions.
 * Contains all information about the current plan node and execution.
 */
typedef struct MetricContext
{
    /* Plan node information */
    PlanState      *pstate;
    Plan           *plan;
    Instrumentation *instrument;

    /* Execution context */
    QueryDesc      *queryDesc;
    double          totaltime;
    double          totalcost;
    int             nnodes;         /* Total nodes evaluated so far */
    int             node_index;     /* Current node index */
    bool            is_leaf_node;   /* True if this is a leaf node */

    /* Computed node metrics (for per-node metrics) */
    double          plan_rows;
    double          actual_rows;
    double          nloops;
    double          relative_time;  /* node_time / totaltime */
    double          relative_cost;  /* node_cost / totalcost */

    /* User-extensible context (for complex metrics) */
    void           *user_data;
} MetricContext;

/*
 * Generic metric value container.
 * Metrics return this, framework stores it appropriately.
 */
typedef struct MetricValue
{
    MetricStorage   storage_type;
    union {
        double      double_val;     /* For RSTATS input, GAUGE */
        int64       int64_val;      /* For COUNTER */
    } value;
    bool            is_valid;       /* False if metric couldn't be computed */
} MetricValue;

/*
 * Metric computation callback.
 *
 * Called for each node (PER_NODE) or once per execution (PER_EXECUTION).
 * Returns MetricValue to be stored.
 *
 * Return .is_valid = false to skip storing this metric for this execution.
 */
typedef MetricValue (*MetricComputeFunc)(const MetricContext *ctx);

/*
 * Metric finalization callback (optional).
 *
 * Called after all nodes have been processed, before storing to DSM.
 * Allows metrics to perform aggregation (e.g., averaging across nodes).
 *
 * accumulated: sum of all MetricValues returned by compute function
 * nnodes: number of nodes that returned valid values
 *
 * Return finalized value to store. Return .is_valid = false to skip storage.
 */
typedef MetricValue (*MetricFinalizeFunc)(double accumulated, int nnodes);

/*
 * Metric initialization callback (optional).
 *
 * Called when a new DSMOptimizerTrackerEntry is created.
 * Allows metric to initialize its storage (e.g., rstats_set_empty()).
 *
 * storage_ptr: pointer to the metric's storage slot in DSMOptimizerTrackerEntry
 * storage_type: type of storage for this metric
 */
typedef void (*MetricInitFunc)(void *storage_ptr, MetricStorage storage_type);

/*
 * Metric Descriptor - Defines a trackable metric.
 *
 * Developers register these at extension load time.
 */
struct MetricDescriptor
{
    /* Identity */
    const char         *name;           /* Metric name (SQL column name) */
    const char         *description;    /* Human-readable description */

    /* Behavior */
    MetricType          type;           /* Per-node or per-execution */
    MetricStorage       storage;        /* How to store values */

    /* Callbacks */
    MetricComputeFunc   compute;        /* Required: compute metric value */
    MetricFinalizeFunc  finalize;       /* Optional: finalize before storage */
    MetricInitFunc      initialize;     /* Optional: initialize storage */

    /* Configuration */
    bool                enabled_default; /* Default enabled state */
    int                 priority;        /* Display order (lower = first) */

    /* Runtime state (managed by framework) */
    bool                enabled;         /* Current enabled state */
    int                 storage_offset;  /* Offset in DSMOptimizerTrackerEntry */
    Size                storage_size;    /* Size of storage */
};

/*
 * Framework API - Metric Registration
 */

/* Register a metric descriptor. Call from _PG_init(). */
extern void RegisterMetric(MetricDescriptor *desc);

/* Get array of all registered metrics. */
extern MetricDescriptor **GetRegisteredMetrics(int *nmetrics);

/* Enable/disable metric by name (via GUC). */
extern void SetMetricEnabled(const char *name, bool enabled);

/* Get metric descriptor by name. */
extern MetricDescriptor *GetMetricByName(const char *name);

/*
 * Framework API - Metric Execution
 */

/* Compute all enabled metrics for current context. */
extern void ComputeMetrics(MetricContext *ctx, MetricValue *values_out);

/* Finalize all per-node metrics after tree walk. */
extern void FinalizeMetrics(MetricValue *accumulated, int nnodes,
                           MetricValue *finalized_out);

/* Initialize metric storage in new DSM entry. */
extern void InitializeMetricStorage(void *entry_base);

/* Store computed metrics into DSM entry. */
extern void StoreMetrics(void *entry_base, const MetricValue *values);

#endif /* METRIC_FRAMEWORK_H */
```

---

### 2. Dynamic Storage Allocation

**Problem:** Current design has fixed-size `DSMOptimizerTrackerEntry`.
**Solution:** Variable-size entries with dynamic offset calculation.

**New approach: `pg_track_optimizer.c`**

```c
/*
 * Header for DSM entry - fixed size.
 * Followed by variable-size metric storage.
 */
typedef struct DSMOptimizerTrackerEntryHeader
{
    DSMOptimizerTrackerKey  key;
    int32                   evaluated_nodes;
    int32                   plan_nodes;
    int64                   nexecs;
    dsa_pointer             query_ptr;

    /* NEW: Metric storage follows this header */
    uint32                  metrics_offset;  /* Offset to metric data */
    uint32                  metrics_size;    /* Total size of metric data */
} DSMOptimizerTrackerEntryHeader;

/*
 * Compute total entry size based on enabled metrics.
 */
static Size
compute_entry_size(void)
{
    Size size = sizeof(DSMOptimizerTrackerEntryHeader);
    MetricDescriptor **metrics;
    int nmetrics, i;

    metrics = GetRegisteredMetrics(&nmetrics);
    for (i = 0; i < nmetrics; i++)
    {
        if (metrics[i]->enabled)
            size += metrics[i]->storage_size;
    }

    return size;
}

/*
 * Get pointer to metric storage within entry.
 */
static void *
get_metric_storage(DSMOptimizerTrackerEntryHeader *entry,
                   MetricDescriptor *metric)
{
    char *base = (char *) entry;
    return base + entry->metrics_offset + metric->storage_offset;
}
```

---

### 3. Built-in Metrics as Plugins

**Migrate existing metrics to new framework:**

**New file: `metrics/avg_error.c`**

```c
#include "metric_framework.h"
#include <math.h>

/*
 * Average Error Metric
 *
 * Computes |log(actual_rows / estimated_rows)| per node,
 * then averages across all nodes.
 */

static MetricValue
avg_error_compute(const MetricContext *ctx)
{
    MetricValue result = {0};
    double node_error;

    result.storage_type = METRIC_STORAGE_RSTATS;
    result.is_valid = true;

    /* Skip if we don't have valid data */
    if (ctx->actual_rows <= 0 || ctx->plan_rows <= 0)
    {
        result.is_valid = false;
        return result;
    }

    /* Compute relative error on log scale */
    node_error = fabs(log(ctx->actual_rows / ctx->plan_rows));
    result.value.double_val = node_error;

    return result;
}

static MetricValue
avg_error_finalize(double accumulated, int nnodes)
{
    MetricValue result = {0};

    result.storage_type = METRIC_STORAGE_RSTATS;
    result.is_valid = (nnodes > 0);

    if (nnodes > 0)
        result.value.double_val = accumulated / nnodes;
    else
        result.value.double_val = -1.0;  /* No data */

    return result;
}

static void
avg_error_init(void *storage_ptr, MetricStorage storage_type)
{
    RStats *stats = (RStats *) storage_ptr;
    rstats_set_empty(stats);
}

/* Metric descriptor - registered in _PG_init() */
MetricDescriptor avg_error_metric = {
    .name = "avg_error",
    .description = "Average estimation error across all plan nodes",
    .type = METRIC_TYPE_PER_NODE,
    .storage = METRIC_STORAGE_RSTATS,
    .compute = avg_error_compute,
    .finalize = avg_error_finalize,
    .initialize = avg_error_init,
    .enabled_default = true,
    .priority = 10,
    .storage_size = sizeof(RStats)
};
```

**New file: `metrics/twa_error.c`**

```c
/*
 * Time-Weighted Average Error
 *
 * Weights errors by relative execution time of each node.
 * Emphasizes errors in time-consuming operations.
 */

static MetricValue
twa_error_compute(const MetricContext *ctx)
{
    MetricValue result = {0};
    double node_error;

    result.storage_type = METRIC_STORAGE_RSTATS;
    result.is_valid = true;

    if (ctx->actual_rows <= 0 || ctx->plan_rows <= 0)
    {
        result.is_valid = false;
        return result;
    }

    /* Compute error weighted by time */
    node_error = fabs(log(ctx->actual_rows / ctx->plan_rows));
    result.value.double_val = node_error * ctx->relative_time;

    return result;
}

/* Similar finalize and init as avg_error */

MetricDescriptor twa_error_metric = {
    .name = "twa_error",
    .description = "Time-weighted average error (emphasizes slow nodes)",
    .type = METRIC_TYPE_PER_NODE,
    .storage = METRIC_STORAGE_RSTATS,
    .compute = twa_error_compute,
    .finalize = avg_error_finalize,  /* Reuse same logic */
    .initialize = avg_error_init,    /* Reuse same logic */
    .enabled_default = true,
    .priority = 30,
    .storage_size = sizeof(RStats)
};
```

**New file: `metrics/blks_accessed.c`**

```c
/*
 * Blocks Accessed Metric
 *
 * Tracks total I/O (hits + reads + writes).
 * Per-execution metric (not per-node).
 */

static MetricValue
blks_accessed_compute(const MetricContext *ctx)
{
    MetricValue result = {0};
    BufferUsage *bufusage = &ctx->queryDesc->totaltime->bufusage;

    result.storage_type = METRIC_STORAGE_RSTATS;
    result.is_valid = true;

    /* Sum all block accesses */
    result.value.double_val = (double)(
        bufusage->shared_blks_hit +
        bufusage->shared_blks_read +
        bufusage->temp_blks_read +
        bufusage->temp_blks_written
    );

    return result;
}

MetricDescriptor blks_accessed_metric = {
    .name = "blks_accessed",
    .description = "Total blocks accessed (hits + reads + writes)",
    .type = METRIC_TYPE_PER_EXECUTION,  /* Once per query */
    .storage = METRIC_STORAGE_RSTATS,
    .compute = blks_accessed_compute,
    .finalize = NULL,  /* No finalization needed */
    .initialize = avg_error_init,
    .enabled_default = true,
    .priority = 50,
    .storage_size = sizeof(RStats)
};
```

---

### 4. Example: Custom User Metric

**Users can add custom metrics by creating a separate extension.**

**File: `pg_track_custom_metrics.c`**

```c
/*
 * Custom metric: Track queries with hash aggregations
 *
 * This metric counts how many HashAggregate nodes appear in each query.
 * Useful for identifying queries that might benefit from increased work_mem.
 */

#include "postgres.h"
#include "metric_framework.h"

PG_MODULE_MAGIC;

static MetricValue
hash_agg_count_compute(const MetricContext *ctx)
{
    MetricValue result = {0};

    result.storage_type = METRIC_STORAGE_COUNTER;
    result.is_valid = true;

    /* Check if this node is a HashAggregate */
    if (IsA(ctx->plan, Agg) && ((Agg *)ctx->plan)->aggstrategy == AGG_HASHED)
        result.value.int64_val = 1;
    else
        result.value.int64_val = 0;

    return result;
}

static void
hash_agg_count_init(void *storage_ptr, MetricStorage storage_type)
{
    int64 *counter = (int64 *) storage_ptr;
    *counter = 0;
}

static MetricDescriptor hash_agg_metric = {
    .name = "hash_agg_count",
    .description = "Number of hash aggregate nodes in query plan",
    .type = METRIC_TYPE_PER_NODE,
    .storage = METRIC_STORAGE_COUNTER,
    .compute = hash_agg_count_compute,
    .finalize = NULL,
    .initialize = hash_agg_count_init,
    .enabled_default = false,  /* Opt-in */
    .priority = 100,
    .storage_size = sizeof(int64)
};

void
_PG_init(void)
{
    /* Register our custom metric with pg_track_optimizer framework */
    RegisterMetric(&hash_agg_metric);
}
```

**To use:**
```sql
-- Load custom metric extension (after pg_track_optimizer)
CREATE EXTENSION pg_track_custom_metrics;

-- Enable the custom metric
SET pg_track_optimizer.metrics = 'avg_error,rms_error,hash_agg_count';

-- Now hash_agg_count appears in pg_track_optimizer view
SELECT queryid, query, hash_agg_count
FROM pg_track_optimizer
WHERE hash_agg_count > 5;
```

---

### 5. Configuration System

**New GUC: `pg_track_optimizer.metrics`**

```c
/*
 * GUC: Comma-separated list of enabled metrics.
 *
 * Default: "all" (enable all registered metrics with enabled_default=true)
 *
 * Examples:
 *   SET pg_track_optimizer.metrics = 'all';
 *   SET pg_track_optimizer.metrics = 'avg_error,rms_error,exec_time';
 *   SET pg_track_optimizer.metrics = 'none';  -- Disable tracking
 */

static char *metrics_config = NULL;

static bool
metrics_check_hook(char **newval, void **extra, GucSource source)
{
    char       *rawstring;
    List       *elemlist;
    ListCell   *l;

    /* Parse comma-separated list */
    rawstring = pstrdup(*newval);
    if (!SplitIdentifierString(rawstring, ',', &elemlist))
    {
        GUC_check_errdetail("Invalid metric name list");
        pfree(rawstring);
        list_free(elemlist);
        return false;
    }

    /* Special values */
    if (list_length(elemlist) == 1)
    {
        char *val = (char *) linitial(elemlist);
        if (pg_strcasecmp(val, "all") == 0 ||
            pg_strcasecmp(val, "none") == 0)
        {
            pfree(rawstring);
            list_free(elemlist);
            return true;
        }
    }

    /* Validate each metric name exists */
    foreach(l, elemlist)
    {
        char *metric_name = (char *) lfirst(l);
        if (GetMetricByName(metric_name) == NULL)
        {
            GUC_check_errdetail("Unknown metric: \"%s\"", metric_name);
            pfree(rawstring);
            list_free(elemlist);
            return false;
        }
    }

    pfree(rawstring);
    list_free(elemlist);
    return true;
}

static void
metrics_assign_hook(const char *newval, void *extra)
{
    char       *rawstring;
    List       *elemlist;
    ListCell   *l;
    MetricDescriptor **metrics;
    int         nmetrics, i;

    /* Get all registered metrics */
    metrics = GetRegisteredMetrics(&nmetrics);

    /* Handle special values */
    if (pg_strcasecmp(newval, "all") == 0)
    {
        /* Enable all metrics with enabled_default=true */
        for (i = 0; i < nmetrics; i++)
            metrics[i]->enabled = metrics[i]->enabled_default;
        return;
    }

    if (pg_strcasecmp(newval, "none") == 0)
    {
        /* Disable all metrics */
        for (i = 0; i < nmetrics; i++)
            metrics[i]->enabled = false;
        return;
    }

    /* Disable all, then enable specified ones */
    for (i = 0; i < nmetrics; i++)
        metrics[i]->enabled = false;

    rawstring = pstrdup(newval);
    SplitIdentifierString(rawstring, ',', &elemlist);

    foreach(l, elemlist)
    {
        char *metric_name = (char *) lfirst(l);
        MetricDescriptor *desc = GetMetricByName(metric_name);
        if (desc != NULL)
            desc->enabled = true;
    }

    pfree(rawstring);
    list_free(elemlist);
}

/* In _PG_init() */
DefineCustomStringVariable("pg_track_optimizer.metrics",
    "Comma-separated list of metrics to track",
    "Use 'all' for default set, or specify individual metrics",
    &metrics_config,
    "all",
    PGC_SUSET,
    0,
    metrics_check_hook,
    metrics_assign_hook,
    NULL);
```

---

### 6. SQL Interface Generation

**Auto-generate SQL functions/views based on registered metrics.**

**New function: `generate_sql_interface()`**

```c
/*
 * Generate CREATE FUNCTION pg_track_optimizer() dynamically.
 * Called during CREATE EXTENSION.
 */
static void
generate_sql_interface(void)
{
    MetricDescriptor **metrics;
    int nmetrics, i;
    StringInfoData buf;

    initStringInfo(&buf);

    /* Function header */
    appendStringInfo(&buf,
        "CREATE FUNCTION pg_track_optimizer(\n"
        "    OUT dboid Oid,\n"
        "    OUT queryid bigint,\n"
        "    OUT query text,\n"
        "    OUT evaluated_nodes int,\n"
        "    OUT plan_nodes int,\n"
        "    OUT nexecs bigint");

    /* Add output parameters for each enabled metric */
    metrics = GetRegisteredMetrics(&nmetrics);
    for (i = 0; i < nmetrics; i++)
    {
        if (!metrics[i]->enabled_default)
            continue;  /* Only include default metrics in base function */

        appendStringInfo(&buf, ",\n    OUT %s ", metrics[i]->name);

        switch (metrics[i]->storage)
        {
            case METRIC_STORAGE_RSTATS:
                appendStringInfo(&buf, "rstats");
                break;
            case METRIC_STORAGE_COUNTER:
                appendStringInfo(&buf, "bigint");
                break;
            case METRIC_STORAGE_GAUGE:
                appendStringInfo(&buf, "double precision");
                break;
        }
    }

    appendStringInfo(&buf,
        "\n)\n"
        "RETURNS SETOF record\n"
        "AS 'MODULE_PATHNAME', 'pg_track_optimizer'\n"
        "LANGUAGE C STRICT VOLATILE;");

    /* Execute SQL */
    SPI_connect();
    SPI_execute(buf.data, false, 0);
    SPI_finish();

    /* Generate view similarly */
    generate_sql_view();
}
```

---

## Directory Structure

```
pg_track_optimizer/
├── metric_framework.h          # Framework API
├── metric_framework.c          # Framework implementation
├── metrics/                    # Built-in metrics
│   ├── avg_error.c
│   ├── rms_error.c
│   ├── twa_error.c
│   ├── wca_error.c
│   ├── blks_accessed.c
│   ├── local_blks.c
│   └── exec_time.c
├── plan_error.h                # Modified for framework
├── plan_error.c                # Modified to use framework
├── pg_track_optimizer.c        # Modified for dynamic storage
├── pg_track_optimizer--0.2.sql # New version with framework
└── Makefile                    # Updated for new files
```

---

## Migration Path

### Phase 1: Framework Core (v0.2)

1. Implement `metric_framework.h/c`
2. Add `pg_track_optimizer.metrics` GUC
3. Keep existing hardcoded structure as "compatibility mode"
4. Add `pg_track_optimizer.use_framework` GUC (default: false)

### Phase 2: Migrate Built-in Metrics (v0.3)

1. Convert existing metrics to plugin format
2. Test extensively for identical behavior
3. Switch `use_framework` default to true
4. Deprecate compatibility mode

### Phase 3: Dynamic Storage (v0.4)

1. Implement variable-size DSM entries
2. Break file format compatibility (bump version)
3. Remove old compatibility code

### Phase 4: Documentation & Examples (v0.5)

1. Write developer guide for custom metrics
2. Provide example custom metrics
3. Update regression tests
4. Add metric library repository

---

## Benefits

### For Users

| Feature | Before | After |
|---------|--------|-------|
| **Memory per query** | 224 bytes (7 × RStats) | Configurable (e.g., 64 bytes for 2 metrics) |
| **CPU overhead** | Always compute all 7 metrics | Only compute enabled metrics |
| **Customization** | None | Can add custom metrics via extensions |
| **Configuration** | Binary (all or nothing) | Granular per-metric control |

### For Developers

| Task | Before | After |
|------|--------|-------|
| **Add new metric** | Modify 8+ files | Create 1 plugin file |
| **Test metric** | Rebuild entire extension | Independent plugin testing |
| **Distribute metric** | Fork extension | Publish as separate extension |
| **Maintain metrics** | All metrics coupled | Independent maintenance |

---

## Example Configurations

### Minimal Overhead (Production)
```sql
-- Track only critical metrics
SET pg_track_optimizer.metrics = 'avg_error,exec_time';
-- Memory: ~64 bytes/query (2 RStats)
-- CPU: ~60% reduction
```

### Comprehensive Analysis (Development)
```sql
-- Track everything including custom metrics
SET pg_track_optimizer.metrics = 'all';
LOAD 'pg_track_custom_metrics';
-- Memory: ~350 bytes/query (10+ metrics)
-- CPU: Full analysis
```

### Specific Problem Investigation
```sql
-- Track only I/O related metrics
SET pg_track_optimizer.metrics = 'blks_accessed,local_blks';
-- Memory: ~64 bytes/query
-- CPU: Minimal overhead
```

---

## Implementation Estimate

| Phase | Effort | Risk |
|-------|--------|------|
| Framework core | 3-4 days | Low - well-defined scope |
| Migrate 7 metrics | 2-3 days | Medium - regression testing critical |
| Dynamic storage | 2-3 days | Medium - file format changes |
| Testing & docs | 2-3 days | Low - good test coverage exists |
| **Total** | **9-13 days** | **Medium** |

---

## Alternative Approaches Considered

### 1. Compile-Time Configuration
**Pros:** Zero runtime overhead
**Cons:** Can't change metrics without recompile, poor user experience

### 2. Callback-Based (No Descriptors)
**Pros:** Simpler implementation
**Cons:** Can't introspect available metrics, no auto-SQL generation

### 3. JSON Configuration
**Pros:** Very flexible
**Cons:** Slower parsing, more complex validation

### 4. Separate Extension Per Metric
**Pros:** Ultimate modularity
**Cons:** 7+ extensions to install, dependency hell

**Chosen approach balances flexibility, performance, and usability.**

---

## Conclusion

The proposed framework transforms `pg_track_optimizer` from a monolithic extension into an extensible platform. Users gain performance and memory control, while developers gain a clean API for custom metrics.

**Key innovation:** Metric-as-a-plugin architecture with configuration-driven activation.

**Backward compatibility:** Maintained via default configuration matching current behavior.

**Path forward:** Incremental migration allows testing at each phase.
