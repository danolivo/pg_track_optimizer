#!/usr/bin/perl
# Test concurrent flush and reset operations under load
# Uses pgbench with multiple clients to stress-test the locking

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Temp qw(tempdir);

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

# Create the extension and pgbench tables
$node->safe_psql('postgres', 'CREATE EXTENSION pg_track_optimizer;');
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_reset();');

# Initialize pgbench
$node->command_ok(
	[ 'pgbench', '-i', '-s', '1', '-p', $node->port, 'postgres' ],
	'pgbench initialization'
);

# Create temporary directory for pgbench scripts
my $tmpdir = tempdir(CLEANUP => 1);

# Script 1: flush (10% weight)
my $flush_script = "$tmpdir/flush.sql";
open(my $fh1, '>', $flush_script) or die "Cannot create $flush_script: $!";
print $fh1 "SELECT pg_track_optimizer_flush();\n";
close($fh1);

# Script 2: reset (10% weight)
my $reset_script = "$tmpdir/reset.sql";
open(my $fh2, '>', $reset_script) or die "Cannot create $reset_script: $!";
print $fh2 "SELECT pg_track_optimizer_reset();\n";
close($fh2);

# Run pgbench with weighted scripts:
# -b tpcb-like@8 = 80% built-in TPC-B transaction
# -f flush.sql@1 = 10% flush
# -f reset.sql@1 = 10% reset
# Total weight = 10, so 8/10=80%, 1/10=10%, 1/10=10%
my @pgbench_cmd = (
	'pgbench',
	'-c', '10',           # 10 clients
	'-j', '10',           # 10 threads
	'-T', '10',           # Run for 10 seconds
	'-b', 'tpcb-like@8',  # 80% built-in TPC-B
	'-f', "$flush_script\@1",  # 10% flush
	'-f', "$reset_script\@1",  # 10% reset
	'-p', $node->port,
	'postgres'
);

$node->command_ok(\@pgbench_cmd, 'pgbench concurrent flush/reset completed without errors');

# Verify the extension is still functional after stress test
my $result = $node->safe_psql('postgres', 'SELECT COUNT(*) >= 0 FROM pg_track_optimizer;');
is($result, 't', 'pg_track_optimizer view is accessible after stress test');

# Try one more flush and reset to ensure everything still works
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');
pass('flush works after stress test');

$node->safe_psql('postgres', 'SELECT pg_track_optimizer_reset();');
pass('reset works after stress test');

# ============================================================================
# Second pass: test with auto_flush disabled
# ============================================================================
note("Starting second pass with auto_flush disabled");

$node->safe_psql('postgres',
	q{ALTER SYSTEM SET pg_track_optimizer.auto_flush = off;});
$node->safe_psql('postgres', 'SELECT pg_reload_conf();');

# Verify the GUC is set correctly
my $auto_flush_value = $node->safe_psql('postgres',
	'SHOW pg_track_optimizer.auto_flush;');
is($auto_flush_value, 'off', 'auto_flush GUC is disabled');

# Run the same pgbench workload with auto_flush disabled
$node->command_ok(\@pgbench_cmd,
	'pgbench concurrent flush/reset completed without errors (auto_flush=off)');

# Verify the extension is still functional
$result = $node->safe_psql('postgres',
	'SELECT COUNT(*) >= 0 FROM pg_track_optimizer;');
is($result, 't',
	'pg_track_optimizer view is accessible after stress test (auto_flush=off)');

# Manual flush and reset should still work
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');
pass('manual flush works with auto_flush=off');

$node->safe_psql('postgres', 'SELECT pg_track_optimizer_reset();');
pass('manual reset works with auto_flush=off');

# Clean up
$node->stop;

done_testing();
