#!/usr/bin/env perl
use strict;
use warnings;
use utf8;
use Getopt::Long;
use JSON::PP;

binmode STDOUT, ":utf8";

my $period = 300;
my $concurrency = 32; # unused, but kept for CLI compatibility
GetOptions(
    'p=i' => \$period,
    'c=i' => \$concurrency,
) or usage();

my @users = @ARGV;
usage() unless @users;

sub usage {
    die "Usage: $0 [-p seconds] [-c count] <user> [user2 ...]\n";
}

my %icons = (
    success => "✅",
    failure => "❌",
    cancelled => "⏹️",
    timed_out => "⏱️",
    action_required => "⚠️",
    unknown => "➖",
);

sub fetch_repos {
    my ($user) = @_;
    my $json = `gh repo list $user --limit 100 --json name`;
    die "failed to list repos for $user\n" if $? != 0;
    return decode_json($json);
}

sub fetch_status {
    my ($user, $repo) = @_;
    my $json = `gh api repos/$user/$repo/actions/runs?per_page=1`;
    return 'unknown' if $? != 0;
    my $data = decode_json($json);
    return $data->{workflow_runs}[0]{conclusion} // 'unknown';
}

while (1) {
    system('clear');
    for my $user (@users) {
        my $repos = fetch_repos($user);
        for my $repo (@$repos) {
            my $name = $repo->{name};
            my $status = fetch_status($user, $name);
            my $icon = $icons{$status} // $icons{unknown};
            print "$user/$name $icon $status\n";
        }
    }
    last if $period <= 0;
    sleep $period;
}
