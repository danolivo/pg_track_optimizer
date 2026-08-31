#!/usr/bin/perl
# Test the on-exit flushing machinery (auto_flush / need_syncing).
#
# Two defects are covered here:
#
# 1. Under shared_preload_libraries, _PG_init() runs in the postmaster only.
#    An exit callback registered there is cleared in forked children, and the
#    postmaster itself fails the IsUnderPostmaster check - so auto_flush
#    never fired at all: nothing was ever written to disk unless the user
#    called pg_track_optimizer_flush() explicitly.
#
# 2. need_syncing was only set when a NEW queryId was inserted.  A steady
#    workload of already-known queries accumulated statistics that were
#    never flushed on exit, and is_synced claimed everything was on disk
#    while it was not.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

$node->append_conf('postgresql.conf', qq(
shared_preload_libraries = 'pg_track_optimizer'
pg_track_optimizer.mode = 'forced'
compute_query_id = on
search_path = 'pgto, "\$user", public'
));

$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_track_optimizer;');
$node->safe_psql('postgres', q{
	CREATE TABLE need_sync_test(x integer);
	INSERT INTO need_sync_test SELECT generate_series(1, 100);
});

# Exit-time flushes run asynchronously after psql disconnects.  Wait until
# the table is flushed to disk before proceeding, so later observations are
# deterministic.  The poll query disables tracking for its own session first,
# so polling does not itself dirty the table.
sub wait_synced
{
	my ($node) = @_;

	# set_config() disables tracking for the polling session from within the
	# query itself, so its output is the bare flag value.
	$node->poll_query_until('postgres',
		q{SELECT is_synced FROM pg_track_optimizer_status,
		  set_config('pg_track_optimizer.mode', 'disabled', false);}, 't')
	  or die 'timed out waiting for the hash table to be flushed';
}

# Execution 1: a fresh connection runs the probe query for the first time,
# inserting a new hash table entry.  The exit of that backend must flush the
# table to disk - this is the auto_flush contract, and it must hold in the
# shared_preload_libraries configuration.
$node->safe_psql('postgres',
	'SELECT count(*) FROM need_sync_test WHERE x > 0;');
wait_synced($node);

# Execution 2: another fresh connection runs the same query.  This is an
# update of an existing entry - no new queryId appears - and it must be
# flushed on exit just the same.
$node->safe_psql('postgres',
	'SELECT count(*) FROM need_sync_test WHERE x > 0;');

# Restart drops shared memory; shutdown waits for backend exits, so all
# pending flushes are on disk.  The first attach reloads the file.
$node->restart;

# Read back the persisted statistics.  This session is made fully inert
# (no tracking, no exit flush) so it cannot disturb the next scenario.
my $nexecs = $node->safe_psql('postgres', q{
	SET pg_track_optimizer.mode = 'disabled';
	SET pg_track_optimizer.auto_flush = off;
	SELECT nexecs FROM pg_track_optimizer()
	WHERE query LIKE '%FROM need_sync_test WHERE%'
	  AND query NOT LIKE '%pg_track_optimizer%';
});
is($nexecs, '2',
   'statistics updated after the last flush survive a restart');

# Now the is_synced side of the same flag, observed from a single session
# so no concurrent exit-time flush can interfere: sync explicitly, verify
# is_synced = true, update a known entry, verify is_synced = false.
my $out = $node->safe_psql('postgres', q{
	SET pg_track_optimizer.mode = 'disabled';
	SET pg_track_optimizer.auto_flush = off;
	SELECT pg_track_optimizer_flush() >= 0;
	SELECT is_synced FROM pg_track_optimizer_status;
	SET pg_track_optimizer.mode = 'forced';
	SELECT count(*) FROM need_sync_test WHERE x > 0;
	SET pg_track_optimizer.mode = 'disabled';
	SELECT is_synced FROM pg_track_optimizer_status;
});
my @lines = split(/\n/, $out);
is($lines[1], 't', 'is_synced reports true right after an explicit flush');
is($lines[3], 'f', 'is_synced reports false after an update-only execution');

done_testing();
