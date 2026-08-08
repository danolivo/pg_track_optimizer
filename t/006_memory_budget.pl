#!/usr/bin/perl
# Test that pg_track_optimizer.hash_mem bounds the memory the hash table
# really uses - query texts included - rather than an entry count derived from
# it.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

# A deliberately tiny budget: a handful of entries fit, no more.
$node->append_conf('postgresql.conf', qq(
shared_preload_libraries = 'pg_track_optimizer'
pg_track_optimizer.mode = 'forced'
pg_track_optimizer.hash_mem = 8kB
compute_query_id = on
));

$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_track_optimizer;');
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_reset();');

# Flood the table with distinct query IDs.  Constants are normalized away by
# the query jumble, so vary the shape - and give every query a long text, so
# that the budget is spent on texts rather than on the fixed-size entries.
# That is exactly what the old entry-count limit ignored.
my $padding = 'x' x 4000;
my @flood = map { 'SELECT ' . join(',', ('1') x $_) . " /* $padding */;" } (1 .. 100);
$node->safe_psql('postgres', join(' ', @flood));

my ($entries, $mem_used, $dsa_size) = split /\|/,
	$node->safe_psql('postgres',
		'SELECT entries, mem_used, dsa_size FROM pg_track_optimizer_status');
note("entries=$entries mem_used=$mem_used dsa_size=$dsa_size");

cmp_ok($mem_used, '<=', 8 * 1024,
	'charged memory stays inside pg_track_optimizer.hash_mem');
cmp_ok($entries, '<', 100,
	'the flood is cut short by the budget, not admitted wholesale');
# The charge is not the footprint: hash_mem bounds what is requested, and the
# DSA's initial segment alone exceeds a budget this small.
cmp_ok($dsa_size, '>=', $mem_used,
	'the real DSA footprint is at least what is charged');

# Releasing the entries gives the budget back.
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_reset();');
my $after_reset = $node->safe_psql('postgres',
	'SELECT mem_used FROM pg_track_optimizer_status');
cmp_ok($after_reset, '<', $mem_used, 'reset releases the charged memory');

# A statistics file that does not fit the configured budget must be refused at
# load time rather than blowing past it.
$node->safe_psql('postgres', "ALTER SYSTEM SET pg_track_optimizer.hash_mem = '1MB'");
$node->restart;
$node->safe_psql('postgres', join(' ', @flood));
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');

my $flushed = $node->safe_psql('postgres',
	'SELECT entries FROM pg_track_optimizer_status');
cmp_ok($flushed, '>', 50, 'entries accumulated under the larger budget');

my $logstart = -s $node->logfile;
$node->safe_psql('postgres', "ALTER SYSTEM SET pg_track_optimizer.hash_mem = '8kB'");
$node->restart;

# Touch the extension so that the shared state - and the load - is initialized.
$node->safe_psql('postgres', 'SELECT 1;');
my $loaded = $node->safe_psql('postgres',
	'SELECT entries FROM pg_track_optimizer_status');
cmp_ok($loaded, '<=', 2, 'a file exceeding the budget is not loaded');
ok($node->log_contains(qr/holds more data than "pg_track_optimizer\.hash_mem" allows/,
					   $logstart),
   'the refusal is reported');

$node->stop;
done_testing();
