#!/usr/bin/perl
# Test that a failure during hash-entry initialization in store_data()
# cannot expose a half-initialized ("torn") entry to readers.
#
# A torn entry would have a valid key but garbage statistics and, worst of
# all, a garbage query_ptr: every later sequential scan of the hash table
# (the pg_track_optimizer() SRF, flush, reset) would dereference it and
# crash.  store_data() prevents that by using query_ptr itself as the
# validity marker: it is set to InvalidDsaPointer as the first store after
# the entry is inserted and assigned the real allocation as the last store
# of initialization, so any failure in between leaves an entry that readers
# skip and the next execution of the same query rebuilds.
#
# Failures are simulated with injection points at the two interesting
# positions of that window.  The test is skipped when the server was built
# without --enable-injection-points.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

# Tracking starts disabled: the injection points must only fire for the
# queries we arm them for, not for the harness queries around them.
$node->append_conf('postgresql.conf', qq(
shared_preload_libraries = 'pg_track_optimizer'
pg_track_optimizer.mode = 'disabled'
compute_query_id = on
search_path = 'pgto, "\$user", public'
));

$node->start;

# Probe for injection-point support: the module may not be installed, or the
# server may be built without --enable-injection-points.
my ($probe_ret, $probe_out, $probe_err) = $node->psql('postgres', q{
	CREATE EXTENSION injection_points;
	SELECT injection_points_attach('pgto-probe', 'notice');
	SELECT injection_points_detach('pgto-probe');
});
if ($probe_ret != 0)
{
	plan skip_all => 'injection_points extension is not available';
}

$node->safe_psql('postgres', 'CREATE EXTENSION pg_track_optimizer;');
$node->safe_psql('postgres', q{
	CREATE TABLE torn_test(x integer);
	INSERT INTO torn_test SELECT generate_series(1, 100);
});

# Run one query with the named injection point armed to fail, in a session
# of its own: injection_points_set_local() ties the point to that session,
# so it dies with it.  Returns the psql exit status and stderr.
sub run_with_failing_point
{
	my ($node, $point, $query) = @_;
	my ($ret, $stdout, $stderr) = $node->psql('postgres', qq{
		SELECT injection_points_set_local();
		SELECT injection_points_attach('$point', 'error');
		SET pg_track_optimizer.mode = 'forced';
		$query
	});
	return ($ret, $stderr);
}

# Assert the invariants that must hold after any interrupted initialization:
# nothing visible to readers, flush unaffected, the next execution of the same
# query rebuilds the entry, and the memory budget is charged for that rebuild
# exactly once.
sub check_entry_invariants
{
	my ($node, $label, $query, $pattern) = @_;

	my $visible = $node->safe_psql('postgres', qq{
		SELECT count(*) FROM pg_track_optimizer()
		WHERE query LIKE '$pattern'
		  AND query NOT LIKE '%pg_track_optimizer%';
	});
	is($visible, '0', "$label: incomplete entry is invisible to readers");

	my $flushed = $node->safe_psql('postgres',
		'SELECT pg_track_optimizer_flush() >= 0;');
	is($flushed, 't', "$label: flush succeeds with an incomplete entry present");

	# An incomplete entry owns nothing and was never charged, so the rebuild
	# below is what pays for it - and must pay exactly once.
	my ($entries_before, $mem_before) = split /\|/, $node->safe_psql('postgres',
		'SELECT entries, mem_used FROM pg_track_optimizer_status');

	# The injection point died with its session, so this execution succeeds
	# and must adopt the entry left behind by the failed one.
	$node->safe_psql('postgres', qq{
		SET pg_track_optimizer.mode = 'forced';
		$query
	});
	my $nexecs = $node->safe_psql('postgres', qq{
		SELECT nexecs FROM pg_track_optimizer()
		WHERE query LIKE '$pattern'
		  AND query NOT LIKE '%pg_track_optimizer%';
	});
	is($nexecs, '1', "$label: the entry was rebuilt by the next execution");

	my ($entries_after, $mem_after) = split /\|/, $node->safe_psql('postgres',
		'SELECT entries, mem_used FROM pg_track_optimizer_status');
	is($entries_after - $entries_before, 1,
		"$label: the rebuild adds exactly one counted entry");
	cmp_ok($mem_after, '>', $mem_before,
		"$label: the rebuilt entry is charged against the budget");
	cmp_ok($mem_after, '<=', 4 * 1024 * 1024,
		"$label: the charge stays inside the default hash_mem");
}

# Scenario 1: the query-text allocation fails.  This is the realistic case -
# dsa_allocate0() throws on out-of-shared-memory - and nothing was allocated,
# so no memory is lost.
my ($ret1, $stderr1) = run_with_failing_point($node,
	'pg_track_optimizer-query-text-alloc',
	'SELECT count(*) FROM torn_test;');
isnt($ret1, 0, 'query fails when the query-text allocation fails');
like($stderr1, qr/injection point/, 'the injected allocation failure fired');

check_entry_invariants($node, 'alloc failure',
	'SELECT count(*) FROM torn_test;', '%count(*) FROM torn\_test%');

# Scenario 2: a failure after a successful allocation but before the store
# that publishes it.  Today nothing between those two points can fail, so
# this simulates future code drift adding a failure there.  The safety
# properties must still hold; the cost is that the allocation is orphaned
# (documented in the struct comment), which is why the allocation is kept
# adjacent to its publication.
my ($ret2, $stderr2) = run_with_failing_point($node,
	'pg_track_optimizer-entry-publish',
	'SELECT count(x) FROM torn_test;');
isnt($ret2, 0, 'query fails when interrupted before publication');
like($stderr2, qr/injection point/, 'the injected publication failure fired');

check_entry_invariants($node, 'publication failure',
	'SELECT count(x) FROM torn_test;', '%count(x) FROM torn\_test%');

# Reset copes with whatever the exercise left in the table
my $reset_ok = $node->safe_psql('postgres',
	'SELECT pg_track_optimizer_reset() >= 0;');
is($reset_ok, 't', 'reset succeeds after the torn-entry exercise');

done_testing();
