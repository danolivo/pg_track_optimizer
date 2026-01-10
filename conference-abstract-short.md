# PGConf.dev 2026 - Talk Submission

## Title
**Finding the Invisible: Detecting Query Planning Issues That Don't Show Up in Performance Metrics**

## Format
Talk (45 minutes)

## Track
Performance & Optimization / Extensions & Tools

## Level
Intermediate

---

## Abstract (300 words)

PostgreSQL's query optimizer makes sophisticated predictions about row counts and execution costs, but even the best planner can be wrong. The problem? Traditional monitoring tools like pg_stat_statements focus on execution metrics—CPU time, I/O, and duration—but completely miss the critical dimension: **how accurate were the planner's predictions?**

A query executing in 10ms might seem perfectly healthy, but if the planner estimated 10 rows and got 100,000, you're one data distribution change away from a production catastrophe. These "invisible problems" lurk in your database: queries that execute fine today but carry 5×, 10×, or even 100× cardinality estimation errors. When conditions shift—a new customer, a viral post, a seasonal spike—these ticking time bombs explode.

This talk introduces **pg_track_optimizer**, a PostgreSQL extension architected specifically to detect cardinality estimation errors in real-world production workloads. Unlike pg_stat_statements which tracks *what happened during execution*, pg_track_optimizer tracks *what the planner predicted would happen* versus *what actually happened at every plan node*.

**You'll learn:**

- **Architecture**: How executor hooks, dynamic shared memory (DSM), and the custom RStats type enable efficient tracking with <2% overhead
- **Why pg_stat_statements isn't enough**: The fundamental difference between execution metrics and planning accuracy
- **Detection techniques**: Four error metrics (avg, RMS, time-weighted, cost-weighted) and advanced indicators:
  - JOIN filtering factors (detecting post-join filtering overhead)
  - Scan filtering factors (finding index inefficiencies)
  - SubPlan cost analysis (correlated subquery detection)
- **Real-world impact**: Finding problematic queries *before* they become slow, even when current execution times look fine

Whether you're a DBA hunting phantom performance issues or a PostgreSQL developer curious about optimizer internals, you'll learn how to surface planning problems that traditional monitoring tools fundamentally cannot see.

---

## Target Audience

- Database administrators managing production PostgreSQL systems
- Performance engineers investigating query optimization
- Developers working with complex queries and large datasets
- Anyone interested in PostgreSQL internals and extension development

**Prerequisites:** Familiarity with EXPLAIN/EXPLAIN ANALYZE output. Basic understanding of query planning concepts. No extension development experience required.

---

## Key Takeaways

1. Execution metrics miss planning problems—fast queries can harbor dangerous estimation errors
2. Node-level cardinality tracking reveals issues invisible to query-level monitoring
3. Four complementary error metrics detect different types of planning failures
4. Advanced indicators (filtering factors, SubPlan analysis) pinpoint specific optimization opportunities
5. Proactive detection enables fixing problems before they impact production

---

## Outline

1. **The Problem** (5 min): Why execution metrics miss planning problems
2. **Architecture** (15 min): Executor hooks, DSM design, RStats type implementation
3. **vs. pg_stat_statements** (8 min): Complementary tools, not competing
4. **Detection Techniques** (10 min): Four error metrics + advanced indicators
5. **Real-World Impact** (5 min): Production deployment, case studies
6. **Q&A** (2 min)

---

## Why This Talk Matters

Query optimization is typically reactive: "This query is slow, fix it." But by the time a query is slow, you're in crisis mode. pg_track_optimizer enables **proactive optimization**: identifying queries with poor cardinality estimates while they're still fast, giving you time to fix them before they cause outages.

This represents a fundamental shift in how we approach PostgreSQL performance: from symptom treatment to root cause prevention.

---

## About the Speaker

Andrei Lepikhov is a PostgreSQL contributor and database performance engineer specializing in query optimization. [Add your background, affiliations, previous talks, contributions, etc.]

---

## Additional Information

- **GitHub**: https://github.com/danolivo/pg_track_optimizer
- **Will include**: Live demo, reproducible examples, slide deck
- **First presented**: [If applicable]
- **Related work**: [Any papers, blog posts, previous talks]

---

## Alternative Titles (if needed)

1. "Beyond pg_stat_statements: Tracking PostgreSQL Query Planner Accuracy"
2. "The Hidden Cost of Bad Plans: Architecture of pg_track_optimizer"
3. "Cardinality Catastrophes: How to Find Query Planning Problems Before They Strike"
4. "Plan Prediction vs. Reality: Building a PostgreSQL Optimizer Tracking Extension"
