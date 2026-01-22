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

# Flush the data to the statistics file
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

# Verify the system can recover after corruption
# This tests that the atomic counter was properly reset to 0
note("Testing recovery: flush new data and verify it can be loaded");

# Re-enable tracking
$node->safe_psql('postgres',
	qq(ALTER SYSTEM SET pg_track_optimizer.mode = 'forced'));
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');

# Execute a new query to generate fresh tracking data
$node->safe_psql('postgres', 'SELECT * FROM checksum_test WHERE x = 42;');

# Disable tracking again
$node->safe_psql('postgres',
	qq(ALTER SYSTEM SET pg_track_optimizer.mode = 'disabled'));
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');

# Verify we have data in the hash table before flush
my $count_before_recovery_flush = $node->safe_psql('postgres',
	'SELECT COUNT(*) FROM pg_track_optimizer;');
ok($count_before_recovery_flush > 0, 'Hash table contains data after corruption recovery');

# Flush and restart to verify the new data can be loaded
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');
note("Restarting to verify new data loads successfully");
$node->restart;

# Load should succeed now with the new uncorrupted file
my $count_after_reload = $node->safe_psql('postgres',
	'SELECT COUNT(*) FROM pg_track_optimizer;');
is($count_after_reload, $count_before_recovery_flush,
   'New data loads successfully after corruption recovery - atomic counter was properly reset');

# Verify no checksum error in logs this time
my $final_log = slurp_file($node->logfile);
my $error_count = () = $final_log =~ /has incorrect CRC32C checksum/g;
is($error_count, 1, 'Only the original corruption was logged, not the recovery');

# Note: The server handles corruption gracefully with a WARNING and continues running.
# After recovery, normal operation resumes and new data can be flushed and loaded.

# Test header corruption - corrupt a byte in the header section
note("Testing header corruption detection");

# Re-enable tracking to generate fresh data
$node->safe_psql('postgres',
	qq(ALTER SYSTEM SET pg_track_optimizer.mode = 'forced'));
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');

# Execute queries to generate tracking data
$node->safe_psql('postgres', 'SELECT * FROM checksum_test WHERE x = 99;');

# Disable tracking
$node->safe_psql('postgres',
	qq(ALTER SYSTEM SET pg_track_optimizer.mode = 'disabled'));
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');

# Flush to create a fresh uncorrupted file
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');

# Stop the server to corrupt the file
$node->stop;

# Corrupt a byte in the header (offset 2 - inside the 4-byte header)
# Header is at offset 0-3, we'll corrupt byte 2
my $header_corrupt_offset = 2;
note("Corrupting header byte at offset $header_corrupt_offset");

open(my $fh2, '+<:raw', $flush_file) or die "Cannot open $flush_file: $!";
seek($fh2, $header_corrupt_offset, 0) or die "Cannot seek to offset $header_corrupt_offset: $!";
my $orig_byte;
read($fh2, $orig_byte, 1) or die "Cannot read byte: $!";
my $corrupt_byte = chr(ord($orig_byte) ^ 0xFF);  # Invert all bits
seek($fh2, $header_corrupt_offset, 0) or die "Cannot seek back to offset $header_corrupt_offset: $!";
print $fh2 $corrupt_byte or die "Cannot write corrupted byte: $!";
close($fh2);

note("Header byte at offset $header_corrupt_offset changed from " . sprintf("0x%02X", ord($orig_byte)) .
     " to " . sprintf("0x%02X", ord($corrupt_byte)));

# Start the server - it should detect the corruption
note("Restarting server with corrupted header...");
$node->start;

# Try to load the file - should fail due to header validation
# Note: Header is validated BEFORE CRC check, so we get an ERROR (not WARNING)
# Use psql directly since safe_psql will die on ERROR
my ($header_ret, $header_stdout, $header_stderr) = $node->psql('postgres',
    'SELECT COUNT(*) FROM pg_track_optimizer;');

# The query should fail with an error
isnt($header_ret, 0, 'Query failed due to header corruption');
like($header_stderr, qr/has incompatible header version/,
     'stderr contains header error message');

# Verify the log shows header error (header is checked before CRC)
my $header_log = slurp_file($node->logfile);
like($header_log, qr/has incompatible header version/,
     'Header corruption detected by header validation');

# Verify this is an ERROR (not just WARNING like CRC errors)
like($header_log, qr/ERROR:.*has incompatible header version/,
     'Header corruption raises ERROR, not just WARNING');

# Count total CRC errors - should still be 1 (only the original data corruption)
# Header corruption is caught before CRC validation
my $total_crc_errors = () = $header_log =~ /has incorrect CRC32C checksum/g;
is($total_crc_errors, 1, 'CRC error count unchanged - header caught earlier');

pass('Header corruption correctly detected by header validation (before CRC check)');

# Test format version corruption - corrupt a byte in the format version field
note("Testing format version corruption detection");

# First, remove the corrupted header file and restart with clean state
$node->stop;
unlink($flush_file) or die "Cannot remove corrupted file: $!";
$node->start;

# Re-enable tracking and generate fresh data
$node->safe_psql('postgres',
	qq(ALTER SYSTEM SET pg_track_optimizer.mode = 'forced'));
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
$node->safe_psql('postgres', 'SELECT * FROM checksum_test WHERE x = 88;');

# Disable and flush
$node->safe_psql('postgres',
	qq(ALTER SYSTEM SET pg_track_optimizer.mode = 'disabled'));
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');

# Stop and corrupt format version (offset 4-7, we'll corrupt byte 5)
$node->stop;

my $version_corrupt_offset = 5;
note("Corrupting format version byte at offset $version_corrupt_offset");

open(my $fh3, '+<:raw', $flush_file) or die "Cannot open $flush_file: $!";
seek($fh3, $version_corrupt_offset, 0) or die "Cannot seek: $!";
my $ver_orig_byte;
read($fh3, $ver_orig_byte, 1) or die "Cannot read: $!";
my $ver_corrupt_byte = chr(ord($ver_orig_byte) ^ 0xFF);
seek($fh3, $version_corrupt_offset, 0) or die "Cannot seek back: $!";
print $fh3 $ver_corrupt_byte or die "Cannot write: $!";
close($fh3);

note("Format version byte changed from " . sprintf("0x%02X", ord($ver_orig_byte)) .
     " to " . sprintf("0x%02X", ord($ver_corrupt_byte)));

# Restart - should detect version corruption
note("Restarting with corrupted format version...");
$node->start;

# Should fail with version error (ERROR, not WARNING)
my ($ver_ret, $ver_stdout, $ver_stderr) = $node->psql('postgres',
    'SELECT COUNT(*) FROM pg_track_optimizer;');

isnt($ver_ret, 0, 'Query failed due to format version corruption');
like($ver_stderr, qr/has incompatible data format version/,
     'stderr contains format version error');

# Verify log shows version error
my $version_log = slurp_file($node->logfile);
like($version_log, qr/has incompatible data format version/,
     'Format version corruption detected');
like($version_log, qr/ERROR:.*has incompatible data format version/,
     'Format version corruption raises ERROR');

pass('Format version corruption correctly detected');

# Test platform version string corruption
note("Testing platform version string corruption detection");

# First, remove the corrupted format version file and restart with clean state
$node->stop;
unlink($flush_file) or die "Cannot remove corrupted file: $!";
$node->start;

# Re-enable, generate data, flush
$node->safe_psql('postgres',
	qq(ALTER SYSTEM SET pg_track_optimizer.mode = 'forced'));
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
$node->safe_psql('postgres', 'SELECT * FROM checksum_test WHERE x = 77;');
$node->safe_psql('postgres',
	qq(ALTER SYSTEM SET pg_track_optimizer.mode = 'disabled'));
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
$node->safe_psql('postgres', 'SELECT pg_track_optimizer_flush();');

# Stop and corrupt platform version string
# Version string starts after: header(4) + format_version(4) + verstr_len(4) = offset 12
# We'll corrupt byte at offset 15 (inside the version string)
$node->stop;

my $platver_corrupt_offset = 15;
note("Corrupting platform version string byte at offset $platver_corrupt_offset");

open(my $fh4, '+<:raw', $flush_file) or die "Cannot open $flush_file: $!";
seek($fh4, $platver_corrupt_offset, 0) or die "Cannot seek: $!";
my $plat_orig_byte;
read($fh4, $plat_orig_byte, 1) or die "Cannot read: $!";
my $plat_corrupt_byte = chr(ord($plat_orig_byte) ^ 0xFF);
seek($fh4, $platver_corrupt_offset, 0) or die "Cannot seek back: $!";
print $fh4 $plat_corrupt_byte or die "Cannot write: $!";
close($fh4);

note("Platform version byte changed from " . sprintf("0x%02X", ord($plat_orig_byte)) .
     " to " . sprintf("0x%02X", ord($plat_corrupt_byte)));

# Restart - platform version mismatch should give WARNING (not ERROR)
note("Restarting with corrupted platform version string...");
$node->start;

# Should succeed but with WARNING (different from header/version which ERROR)
my $platver_result = $node->safe_psql('postgres',
    'SELECT COUNT(*) FROM pg_track_optimizer;');
is($platver_result, '0', 'Platform version mismatch allows query but returns no data');

# Verify log shows WARNING (not ERROR) for platform mismatch
my $platver_log = slurp_file($node->logfile);
like($platver_log, qr/has been written on different platform/,
     'Platform version corruption detected');
like($platver_log, qr/WARNING:.*has been written on different platform/,
     'Platform version mismatch raises WARNING, not ERROR (more graceful)');

pass('Platform version string corruption correctly detected with graceful degradation');

done_testing();
