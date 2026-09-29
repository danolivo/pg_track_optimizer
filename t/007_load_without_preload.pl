#!/usr/bin/perl
# The extension does not need shared_preload_libraries or a server restart.
#
# Shared state is created lazily through the DSM registry on first use, so
# the library can be brought in with LOAD or session_preload_libraries on a
# running server.  What the loading method decides is the scope of tracking:
#
#   LOAD                      - the current session only
#   session_preload_libraries - every backend started after a reload
#
# This test never puts the library into shared_preload_libraries and never
# restarts the server, and checks that statistics still accumulate in one
# shared table across sessions loaded either way.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

# Deliberately no shared_preload_libraries here.
$node->append_conf('postgresql.conf', qq(
compute_query_id = on
search_path = 'pgto, "\$user", public'
));

$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_track_optimizer;');
$node->safe_psql('postgres', q{
	CREATE TABLE load_test(x integer);
	INSERT INTO load_test SELECT generate_series(1, 100);
});

my $probe = 'SELECT count(*) FROM load_test WHERE x > 0;';

# Reads the execution counter of the probe query.  Calling
# pg_track_optimizer() loads the library into this session as a side
# effect, but mode defaults to 'disabled', so the reader itself is never
# tracked.
sub probe_nexecs
{
	my ($node) = @_;

	return $node->safe_psql('postgres', q{
		SELECT coalesce(sum(nexecs), 0) FROM pg_track_optimizer()
		WHERE query LIKE '%FROM load_test WHERE%'
		  AND query NOT LIKE '%pg_track_optimizer%';
	});
}

# 1. A session that has not loaded the library is not tracked, even with
#    mode set: without the hooks there is nothing to record.  Setting the
#    GUC before the library is loaded merely creates a placeholder.
$node->safe_psql('postgres', qq{
	SET pg_track_optimizer.mode = 'forced';
	$probe
});
is(probe_nexecs($node), '0',
   'a session without the library loaded is not tracked');

# 2. LOAD in a session is enough: shared memory is created on demand, and
#    queries of that session are tracked from then on.
$node->safe_psql('postgres', qq{
	LOAD 'pg_track_optimizer';
	SET pg_track_optimizer.mode = 'forced';
	$probe
});
is(probe_nexecs($node), '1',
   'LOAD without shared_preload_libraries tracks the current session');

# 3. Scope of LOAD is one session: a fresh connection that does not load
#    the library is still not tracked.
$node->safe_psql('postgres', qq{
	SET pg_track_optimizer.mode = 'forced';
	$probe
});
is(probe_nexecs($node), '1',
   'LOAD does not affect other sessions');

# 4. session_preload_libraries plus a reload - no restart - makes every new
#    backend load the library.  The reload is asynchronous, so wait until a
#    new session reports the setting before relying on it.
$node->append_conf('postgresql.conf',
	"session_preload_libraries = 'pg_track_optimizer'\n");
$node->reload;
$node->poll_query_until('postgres',
	q{SELECT current_setting('session_preload_libraries') = 'pg_track_optimizer';})
  or die 'timed out waiting for session_preload_libraries to take effect';

$node->safe_psql('postgres', qq{
	SET pg_track_optimizer.mode = 'forced';
	$probe
});
is(probe_nexecs($node), '2',
   'session_preload_libraries takes effect on new backends without a restart');

# 5. The statistics collected by a LOADed session and by a preloaded one
#    ended up in the same entry (nexecs went 1 -> 2 rather than a second
#    entry appearing), so shared state is indeed shared regardless of how
#    each backend loaded the library.
my $entries = $node->safe_psql('postgres', q{
	SELECT count(*) FROM pg_track_optimizer()
	WHERE query LIKE '%FROM load_test WHERE%'
	  AND query NOT LIKE '%pg_track_optimizer%';
});
is($entries, '1', 'LOADed and preloaded backends share one hash table');

done_testing();
