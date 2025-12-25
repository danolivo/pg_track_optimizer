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
    INSERT INTO flush_test SELECT i, i*2 FROM generate_series(1, 100) i;
    ANALYZE flush_test;
});

# Execute some queries to generate tracking data
$node->safe_psql('postgres', 'SELECT * FROM flush_test WHERE x < 10;');
$node->safe_psql('postgres', 'SELECT * FROM flush_test WHERE y > 50;');
$node->safe_psql('postgres', 'SELECT * FROM flush_test WHERE x BETWEEN 20 AND 30;');

# Verify we have tracked data before flush
my $before_flush = $node->safe_psql('postgres',
    'SELECT COUNT(*), SUM(nexecs) FROM pg_track_optimizer();');
note("Before flush: $before_flush");

# Parse the result
my ($count_before, $nexecs_before) = split(/\|/, $before_flush);
$count_before =~ s/^\s+|\s+$//g;
$nexecs_before =~ s/^\s+|\s+$//g;

is($count_before, '3', 'Should have 3 tracked queries before flush');
is($nexecs_before, '3', 'Should have 3 total executions before flush');

# Flush the data to disk
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');

# Verify the flush file was created
my $datadir = $node->data_dir;
my $flush_file = "$datadir/pg_track_optimizer.stat";
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
    'SELECT COUNT(*), SUM(nexecs) FROM pg_track_optimizer();');
note("After restart: $after_restart");

# Parse the result
my ($count_after, $nexecs_after) = split(/\|/, $after_restart);
$count_after =~ s/^\s+|\s+$//g;
$nexecs_after =~ s/^\s+|\s+$//g;

# The critical test: did we load all 3 queries?
# With the EOF bug, this will fail because _load_hash_table errors out
is($count_after, '3', 'Should have 3 queries loaded after restart');
is($nexecs_after, '3', 'Should have 3 total executions after restart');

# Execute one of the original queries again
$node->safe_psql('postgres', 'SELECT * FROM flush_test WHERE x < 10;');

# Verify the execution count increased
my $after_exec = $node->safe_psql('postgres',
    q{SELECT nexecs FROM pg_track_optimizer()
      WHERE query LIKE '%flush_test%WHERE x < $%'
      ORDER BY nexecs DESC LIMIT 1;});
$after_exec =~ s/^\s+|\s+$//g;

is($after_exec, '2', 'Query should have 2 executions (1 before restart + 1 after)');

# Verify all queries are present
my $all_queries = $node->safe_psql('postgres',
    q{SELECT COUNT(*) FROM pg_track_optimizer()
      WHERE query LIKE '%flush_test%';});
$all_queries =~ s/^\s+|\s+$//g;

is($all_queries, '3', 'All 3 queries should be present after restart');

# Check the PostgreSQL log for any errors during load
my $log_contents = $node->log_content();
unlike($log_contents, qr/could not read file.*pg_track_optimizer\.stat/,
    'Should not have file read errors in log');
unlike($log_contents, qr/pg_track_optimizer.*ERROR/i,
    'Should not have any pg_track_optimizer errors in log');

# Additional test: Flush again and restart again to ensure repeatability
note("Testing second flush/restart cycle...");
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');
$node->restart;

my $second_restart = $node->safe_psql('postgres',
    'SELECT COUNT(*), SUM(nexecs) FROM pg_track_optimizer();');
note("After second restart: $second_restart");

my ($count_second, $nexecs_second) = split(/\|/, $second_restart);
$count_second =~ s/^\s+|\s+$//g;
$nexecs_second =~ s/^\s+|\s+$//g;

is($count_second, '3', 'Should still have 3 queries after second restart');

# Clean up
$node->safe_psql('postgres', 'DROP TABLE flush_test;');
$node->stop;

done_testing();
