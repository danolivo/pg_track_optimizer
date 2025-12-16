# pg_track_optimizer
Lightweight (dynamically loaded) extension to explore query plan and execution statistics to find signs of non-optimal optimisation.
**Designed for PostgreSQL v.17 and above.**  

## Abstract
Occasionally, blunders in the optimizer predictions cause a non-optimal query plan.
In an extensive database with a lot of queries, it is challenging to find poorly designed plans. So far, I don't know any tools that can help detect such problems.
Here, we introduce trivial criteria based on the difference between estimated and actual (after the execution) cardinalities.
Of course, it is not proof of the problem, but we can filter candidates from a vast set of queries using this value.
This extension can gather statistics in shared memory and show it on-demand. Also, by setting an error threshold, you can obtain explains of queries with high value of the error in the instance log.

## Interface
### GUCs
- *pg_track_optimizer.mode* = {normal | forced | disabled (default)}. *disabled* mode switches off all activity of the library; *normal* mode gathers statistics only when the value of log_min_error is exceeded; *forced* mode gathers data on each incoming query.
- *pg_track_optimizer.log_min_error* - logging threshold. Criteria for pushing the query explain into the log.
- pg_track_optimizer.hash_mem - memory limit for the hash table size. Right now it doesn't include query texts - looks like a very soft limit.

### Routines
- *pg_track_optimizer()* - show all data gathered.
- *pg_track_optimizer_flush()* - save statistical data to disk. We don't have any automatization yet to avoid overheads.
- *pg_track_optimizer_reset()* - cleanup statistics data.
