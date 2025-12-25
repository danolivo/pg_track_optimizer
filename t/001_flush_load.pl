#!/usr/bin/perl
# Test flush and load functionality with server restart
# This test reproduces the EOF handling bug in _load_hash_table()

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Create a new PostgreSQL cluster
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

# Configure postgresql.conf to load pg_track_optimizer
$node->append_conf('postgresql.conf', qq(
shared_preload_libraries = 'pg_track_optimizer'
pg_track_optimizer.mode = 'forced'
compute_query_id = on
));

# Start the cluster
$node->start;

# Create the extension
$node->safe_psql('postgres', 'CREATE EXTENSION pg_track_optimizer;');

# Create a test table and populate it
$node->safe_psql('postgres', q{
    CREATE TABLE flush_test(x integer, y integer);
});

# Execute some queries to generate tracking data
$node->safe_psql('postgres', 'SELECT * FROM flush_test WHERE x < 10;');
$node->safe_psql('postgres', 'SELECT * FROM flush_test WHERE y > 50;');
$node->safe_psql('postgres', 'SELECT * FROM flush_test WHERE x BETWEEN 20 AND 30;');

# Fix number of records in the HTAB
$node->safe_psql('postgres',
				 qq(ALTER SYSTEM SET pg_track_optimizer.mode = 'disabled'));
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');

# Save number of records before the flush. Use nexecs as a checksum
my $before_flush = $node->safe_psql('postgres', 'SELECT COUNT(*), SUM(nexecs) FROM pg_track_optimizer;');
note("Before flush: $before_flush");

# Flush the data to disk
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');

# Verify the flush file was created
my $datadir = $node->data_dir;
my $flush_file = "$datadir/pg_stat_tmp/pg_track_optimizer.stat";
ok(-f $flush_file, 'Flush file should exist after flush');

my $file_size = -s $flush_file;
note("Flush file size: $file_size bytes");
ok($file_size > 0, 'Flush file should not be empty');

# Now restart the server - this triggers _load_hash_table()
note("Restarting server to trigger load from disk...");
$node->restart;

# After restart, verify the data was loaded correctly
# This is where the EOF bug would manifest
my $after_restart = $node->safe_psql('postgres',
    'SELECT COUNT(*), SUM(nexecs) FROM pg_track_optimizer;');
note("After restart: $after_restart");

print STDERR "DEBUGGING: $before_flush, $after_restart\n";
# Check number of record read
is($before_flush, $after_restart, 'Check loaded statistics consistency after restart');

# Clean up
$node->safe_psql('postgres', 'DROP TABLE flush_test;');
$node->stop;

done_testing();
