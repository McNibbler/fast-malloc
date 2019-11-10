#!/usr/bin/perl
use 5.16.0;
use warnings FATAL => 'all';
use POSIX ":sys_wait_h";

use Time::HiRes qw(time);
use Test::Simple tests => 13;

sub get_time {
    my $data = `cat time.tmp | grep ^real`;
    $data =~ /^real\s+(.*)$/;
    return 0 + $1;
}

sub run_prog {
    my ($prog, $arg) = @_;
    system("rm -f outp.tmp time.tmp");

    my $cpid = fork();
    if ($cpid) {
        my $ii = 0;
        while (waitpid($cpid, WNOHANG) == 0) {
            sleep 1;
            if (++$ii > 20) {
                say "# timeout";
                system("killall $prog");
            }
        }
        #$code = $?;
    }
    else {
        system("time -p -o time.tmp ./$prog $arg > outp.tmp");
        exit(0);
    }

    return `cat outp.tmp`;
}

ok(-f "report.txt", "report.txt exists");
ok(-f "graph.png", "graph.png exists");

my $sys_v = run_prog("collatz-ivec-sys", 1000);
my $t_sv  = get_time();
ok($sys_v =~ /at 871: 178 steps/, "ivec-sys 1k");

my $sys_l = run_prog("collatz-list-sys", 1000);
my $t_sl  = get_time();
ok($sys_l =~ /at 871: 178 steps/, "list-sys 1k");

my $hw7_l = run_prog("collatz-list-hw7", 100);
ok($hw7_l =~ /at 97: 118 steps/, "list-hw7 100");

my $hw7_v = run_prog("collatz-ivec-hw7", 100);
ok($hw7_v =~ /at 97: 118 steps/, "ivec-hw7 100");

my $par_v = run_prog("collatz-ivec-par", 1000);
my $t_pv  = get_time();
my $pv_ok = $par_v =~ /at 871: 178 steps/;
ok($pv_ok, "ivec-par 1k");
ok($pv_ok && $t_pv < $t_sv, "ivec-par beat system time");

my $par_l = run_prog("collatz-list-par", 1000);
my $t_pl  = get_time();
my $pl_ok = $par_l =~ /at 871: 178 steps/;
ok($pl_ok, "list-par 1k");
ok($pl_ok && $t_pl < $t_sl, "list-par beat system time");

sub clang_check {
    my $errs = `clang-check *.c -- 2>&1`;
    chomp $errs;
    if ($errs eq "") {
        return 1;
    }
    else {
        warn $errs;
        return 0;
    }
}

ok(clang_check(), "clang check");

sub crc_check {
    my ($file, $expect) = @_;
    my $crc = `crc32 $file`;
    chomp $crc;
    return $crc eq $expect;
}

ok(crc_check("ivec_main.c", "cae7f5aa"), "ivec_main unchanged");
ok(crc_check("list_main.c", "0ae6f792"), "list_main unchanged");

