const std = @import("std");
const status = @import("status.zig");

fn getRepos(allocator: std.mem.Allocator, user: []const u8) ![][]const u8 {
    const argv = &[_][]const u8{
        "gh",     "repo",          "list", user,                "--public", "--limit", "500",
        "--json", "nameWithOwner", "--jq", ".[].nameWithOwner",
    };
    var result = try std.ChildProcess.exec(.{ .allocator = allocator, .argv = argv });
    defer {
        allocator.free(result.stdout);
        allocator.free(result.stderr);
    }

    var list = std.ArrayList([]const u8).init(allocator);
    defer list.deinit();

    var it = std.mem.split(u8, result.stdout, "\n");
    while (it.next()) |line| {
        if (line.len == 0) continue;
        try list.append(try allocator.dupe(u8, line));
    }
    return list.toOwnedSlice();
}

fn getRepoStatus(allocator: std.mem.Allocator, repo: []const u8) ![]const u8 {
    const path = try std.fmt.allocPrint(allocator, "repos/{s}/actions/runs?per_page=1", .{repo});
    defer allocator.free(path);

    const argv = &[_][]const u8{ "gh", "api", path, "--jq", ".workflow_runs[0].conclusion" };
    var result = try std.ChildProcess.exec(.{ .allocator = allocator, .argv = argv });
    defer {
        allocator.free(result.stdout);
        allocator.free(result.stderr);
    }

    const trimmed = std.mem.trimRight(u8, result.stdout, "\n");
    return allocator.dupe(u8, trimmed);
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer {
        const leaked = gpa.deinit();
        std.debug.assert(leaked == .ok);
    }
    const allocator = gpa.allocator();

    var args = std.process.args();
    _ = args.next(); // skip program name
    const first = args.next() orelse {
        std.debug.print("usage: ghstatus <user> [user2 ...]\n", .{});
        return;
    };

    var users = std.ArrayList([]const u8).init(allocator);
    defer users.deinit();
    try users.append(first);
    while (args.next()) |u| try users.append(u);

    for (users.items) |user| {
        std.debug.print("User: {s}\n", .{user});
        const repos = getRepos(allocator, user) catch |e| {
            std.debug.print("failed to list repos for {s}: {s}\n", .{ user, @errorName(e) });
            continue;
        };
        defer {
            for (repos) |r| allocator.free(r);
            allocator.free(repos);
        }
        for (repos) |repo| {
            const st = getRepoStatus(allocator, repo) catch |e| {
                std.debug.print("failed to fetch status for {s}: {s}\n", .{ repo, @errorName(e) });
                continue;
            };
            defer allocator.free(st);
            std.debug.print("{s} {s}\n", .{ status.statusIcon(st), repo });
        }
    }
}
