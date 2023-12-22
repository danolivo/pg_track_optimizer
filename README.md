# pg_track_optimizer
Explore query plan and execution statistics to find signs of non-optimal optimization

Occasionally, blunders in the optimizer predictions will cause a non-optimal query plan.
So, in an extensive database with a lot of queries, it is challenging to find poorly designed plans. Honestly, so far, I don't know any tools that can help detect such problems.
Here, we introduce trivial criteria based on the difference between estimated and actual (after the execution) cardinalities.
Of course, it is not proof of the problem, but we can filter candidates from a vast set of queries using this value.
To avoid creating a brand new extension (and introducing many new errors), we just add one field into the pg_stat_statements and one mode into auto_explain to log such candidates.

Current patch can be applied to the current PostgreSQL master branch.