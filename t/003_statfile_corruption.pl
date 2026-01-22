#!/usr/bin/perl
# Test different statfile corruption patterns
# This test verifies that corrupted statistics files are detected and rejected
# by corrupting a byte in the file and checking that the data is not loaded

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

# Create a test table and populate it with some data
$node->safe_psql('postgres', q{
    CREATE TABLE checksum_test(x integer, y integer);
    INSERT INTO checksum_test SELECT i, i*2 FROM generate_series(1, 100) i;
});

# Execute some queries to generate tracking data
$node->safe_psql('postgres', 'SELECT * FROM checksum_test WHERE x < 10;');
$node->safe_psql('postgres', 'SELECT * FROM checksum_test WHERE y > 50;');
$node->safe_psql('postgres', 'SELECT * FROM checksum_test WHERE x BETWEEN 20 AND 30;');
$node->safe_psql('postgres', 'SELECT COUNT(*) FROM checksum_test;');

# Disable tracking to fix the HTAB contents
$node->safe_psql('postgres',
	qq(ALTER SYSTEM SET pg_track_optimizer.mode = 'disabled'));
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');

# Save number of records before the flush
my $before_flush = $node->safe_psql('postgres',
	'SELECT COUNT(*), SUM(nexecs) FROM pg_track_optimizer;');
note("Before flush: $before_flush");

my ($record_count, $nexecs_sum) = split(/\|/, $before_flush);
ok($record_count > 0, 'Should have some tracking records');

# Flush the data to disk
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');

# Verify the flush file was created
my $datadir = $node->data_dir;
my $flush_file = "$datadir/pg_stat_tmp/pg_track_optimizer.stat";
ok(-f $flush_file, 'Flush file should exist after flush');

my $file_size = -s $flush_file;
note("Flush file size: $file_size bytes");
ok($file_size > 0, 'Flush file should not be empty');

# Stop the server before corrupting the file
$node->stop;

# Corrupt a byte in the middle of the file (not in header/checksum)
# File format:
#   uint32 header (4 bytes)
#   uint32 version (4 bytes)
#   uint32 verstr_len (4 bytes)
#   char[] version string (variable)
#   For each entry:
#     DSMOptimizerTrackerEntry structure
#     uint32 query_len
#     char[] query_text (query_len bytes)
#   EOF marker (DSMOptimizerTrackerEntry with zeros)
#   uint32 counter (record count)
#   pg_crc32c checksum (4 bytes at end)
#
# We corrupt a byte in the middle of the file (50% offset from start)
# to ensure it's in the data section, not header or checksum

my $corrupt_offset = int($file_size / 2);
note("Corrupting byte at offset $corrupt_offset (50% into file) ...");
open(my $fh, '+<:raw', $flush_file) or die "Cannot open $flush_file: $!";
seek($fh, $corrupt_offset, 0) or die "Cannot seek to offset $corrupt_offset: $!";
my $original_byte;
read($fh, $original_byte, 1) or die "Cannot read byte: $!";
my $corrupted_byte = chr(ord($original_byte) ^ 0xFF);  # Invert all bits
seek($fh, $corrupt_offset, 0) or die "Cannot seek back to offset $corrupt_offset: $!";
print $fh $corrupted_byte or die "Cannot write corrupted byte: $!";
close($fh);

note("Byte at offset $corrupt_offset changed from " . sprintf("0x%02X", ord($original_byte)) .
     " to " . sprintf("0x%02X", ord($corrupted_byte)));

# Start the server - it should detect the checksum error
note("Restarting server with corrupted statistics file...");
$node->start;

# Trigger the file load by querying the view
# This will cause the checksum check to occur
my $ret = $node->safe_psql('postgres',
    'SELECT COUNT(*) FROM pg_track_optimizer;');
is($ret, '0', 'File with failed checksum can not contribute entries to HTAB');

# Now check the server log for checksum error message
# The log should show the checksum validation failure
my $log_content = slurp_file($node->logfile);
like($log_content, qr/has incorrect CRC32C checksum/,
     'Server log should contain checksum error message');

# The corrupted file should have been rejected
pass('Checksum validation correctly detected file corruption');

# Note: The server handles corruption gracefully with a WARNING and continues running.
# The test framework will shut it down automatically when done_testing() is called.

done_testing();
