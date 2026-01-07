# SubPlan Factor Formula Selection

## Objective

The goal is to identify potentially suboptimal subplans where optimization could yield significant performance improvements. This requires distinguishing between inherently expensive but necessary subplans and those that could benefit from query rewriting.

### Use Cases

**Acceptable SubPlan:** A subplan performing an index scan on a primary key that quickly returns a single value may be optimal, especially when executed with few loop iterations and minimal parameter value duplication.

**Problematic SubPlan:** A subplan, multiple times triggering sequential scans of entire tables should be rewritten as a JOIN to eliminate redundant scans.

### Challenge

Given both the actual and planned query states, we must identify potential performance issues. The critical question is: among numerous queries, how do we identify those most negatively impacted by their subplans?

An additional complexity arises from nested subplans, where some may be optimal while others are not.

## Formula Analysis

### Option 1: Time-Based Ratio

```c
sp_factor += instrument->total / cts->total_time;
```

This approach highlights subplans consuming a large portion of query execution time. However, it may produce false positives for simple queries where spending most time in a subplan is unavoidable and appropriate.

### Option 2: Block Access Ratio

```c
sp_factor = subplan_nblocks / subplan_nloops / (query_nblocks / query_nloops);
```

Block access provides a more concrete performance metric. However, this approach has limitations:
- It cannot distinguish between intentional aggregations over large datasets and optimization opportunities
- The extension already provides block-based metrics, offering no additional diagnostic value

### Option 3: Loop Count with Execution Time Weight (Selected)

```c
sp_factor = subplan_nloops * (subplan_exectime / query_exectime);
```

**Rationale:**

The number of loops is a critical factor. Converting a subplan to a JOIN eliminates iterative execution. Single-iteration subplans offer limited optimization potential, though flattening them could enable parallel execution in some cases.

The execution time ratio ensures we focus on subplans with meaningful performance impact, filtering out those consuming less than 1% of query time.

**Advantage for Nested SubPlans:**

This formula effectively identifies problematic nested subplans. While an outer subplan may dominate execution time, if it executes only once, an inner subplan with 1M iterations may represent a more promising optimization target despite its lower absolute time contribution.

---

## Alternative Perspective (Claude's Proposal)

While Option 3 provides a balanced approach, I propose considering a **hybrid metric** that combines multiple signals to reduce false positives and better prioritize optimization efforts:

### Option 4: Multi-Signal Cost Factor

```c
// Primary factor: loops × planned cost (scaled to prevent overflow)
base_factor = (nloops / SCALE) * (planned_cost / SCALE);

// Adjustment: actual execution time ratio (0-1 range)
time_weight = subplan_actual_time / query_total_time;

// Adjustment: estimation error magnitude
error_penalty = abs(log(actual_rows / planned_rows));

// Combined metric
sp_factor = base_factor * time_weight * (1 + error_penalty);
```

**Rationale:**

1. **Loops × Cost as Foundation**: This captures the theoretical amplification effect of repeated subplan execution. Unlike execution time alone, it identifies problems even when the current dataset is small but the query pattern would scale poorly.

2. **Time Weight for Relevance**: Multiplying by the time ratio ensures we focus on subplans that actually matter. A subplan with 1M loops but microsecond execution time per loop is less critical than one consuming 50% of query time.

3. **Estimation Error as Signal**: Large discrepancies between planned and actual rows often indicate missing statistics or suboptimal join order choices. This penalty term elevates subplans where the planner made poor predictions, as these are prime candidates for rewriting.

**Advantages:**

- **Handles edge cases**: Won't flag 1M-loop subplans that are actually fast (low time_weight)
- **Prioritizes actionable issues**: High estimation error suggests the planner lacks information to optimize properly
- **Scalability prediction**: Uses planned cost to identify queries that will degrade as data grows, even if currently fast
- **Balanced scoring**: Multiplication ensures all three factors must be present for a high score

**Trade-offs:**

- More complex to interpret than a single metric
- Requires tuning to determine what constitutes a "high" score
- Estimation error might not always indicate optimization opportunity (could be outdated statistics)

**When to Use Each Metric:**

- **Option 3** (nloops × time_ratio): Best for immediate problem identification in production
- **Option 4** (hybrid): Best for comprehensive analysis and predictive optimization

**Andrei's opinion**: cost value in the factor looks inconvenient because makes this parameter dimensional. We want to compare potential inefficiency of very different queries: one query may be executed in an hour and optimised to be executed in 30 minutes. On the other side, a query, executed in 100ms may be optimised to be executed in 1ms. What is more profitable?

---

## Option 9: Logarithmically Dampened Loop Factor

```c
sp_factor = (subplan_nloops / log(subplan_nloops + 1)) * (subplan_exectime / query_exectime);
```

**Rationale:**

This formula addresses a key insight: the optimization value doesn't grow linearly with loop count. Converting a 10,000-loop subplan to 1 loop isn't necessarily 10× more valuable than converting a 1,000-loop subplan to 1 loop - much of the benefit is captured in the first 90% reduction.

**Growth Characteristics:**

| Loops | Linear Growth (Option 3) | Logarithmic Growth (Option 9) |
|-------|-------------------------|-------------------------------|
| 10    | 10                      | ~4.2                          |
| 100   | 100                     | ~21.7                         |
| 1,000 | 1,000                   | ~145                          |
| 10,000| 10,000                  | ~1,087                        |

The `nloops / log(nloops + 1)` term creates super-linear but sub-quadratic growth:
- **Small loop counts (10)**: Minimal penalty (~4×) - probably not worth complex rewrites
- **Moderate loop counts (100)**: Noticeable factor (~22×) - starting to be interesting
- **High loop counts (1,000)**: Strong signal (~145×) - clear optimization candidate
- **Extreme loop counts (10,000)**: Critical issue (~1,087×) but not explosive

**Advantages:**

1. **Dimensionless**: Like Option 3, this remains a pure ratio comparable across all queries regardless of absolute execution time
2. **Natural thresholding**: Automatically de-prioritizes trivial cases (few loops) while emphasizing truly problematic ones
3. **Non-explosive growth**: Unlike quadratic penalties, this won't produce misleadingly huge numbers for extreme cases
4. **Reflects optimization reality**: The marginal value of eliminating additional loops decreases as you eliminate more

**Disadvantages:**

1. **Less intuitive**: The logarithmic dampening makes the factor harder to interpret directly
2. **More complex computation**: Requires logarithm calculation vs simple multiplication
3. **May under-emphasize extreme cases**: A 10,000-loop subplan might deserve more attention than the ~1,087 factor suggests

**When to Use:**

- Use Option 9 when you want to naturally balance between trivial and extreme cases
- Use Option 3 when you prefer simplicity and direct proportionality to loop count