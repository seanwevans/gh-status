const std = @import("std");

pub const StatusMap = struct {
    match: []const u8,
    icon: []const u8,
    color: u8,
};

pub const status_map = [_]StatusMap{
    .{ .match = "success", .icon = "✅", .color = 1 },
    .{ .match = "failure", .icon = "❌", .color = 2 },
    .{ .match = "timed_out", .icon = "⌛", .color = 2 },
    .{ .match = "cancelled", .icon = "🛑", .color = 4 },
    .{ .match = "skipped", .icon = "⏭️", .color = 5 },
    .{ .match = "in_progress", .icon = "🔁", .color = 7 },
    .{ .match = "action_required", .icon = "⛔", .color = 6 },
    .{ .match = "neutral", .icon = "⭕", .color = 3 },
    .{ .match = "stale", .icon = "🥖", .color = 4 },
    .{ .match = "queued", .icon = "📋", .color = 3 },
    .{ .match = "loading", .icon = "🌀", .color = 3 },
    // default
    .{ .match = "", .icon = "➖", .color = 3 },
};

pub fn statusIcon(status: []const u8) []const u8 {
    for (status_map) |m| {
        if (m.match.len == 0) break; // default handled later
        if (std.mem.indexOf(u8, status, m.match) != null) return m.icon;
    }
    return status_map[status_map.len - 1].icon;
}

pub fn statusColor(status: []const u8) u8 {
    for (status_map) |m| {
        if (m.match.len == 0) break;
        if (std.mem.indexOf(u8, status, m.match) != null) return m.color;
    }
    return status_map[status_map.len - 1].color;
}

const testing = std.testing;

test "status icon mapping" {
    try testing.expectEqualStrings("✅", statusIcon("success"));
    try testing.expectEqualStrings("❌", statusIcon("failure"));
    try testing.expectEqualStrings("➖", statusIcon("unknown"));
}

test "status color mapping" {
    try testing.expectEqual(@as(u8, 1), statusColor("success"));
    try testing.expectEqual(@as(u8, 2), statusColor("failure"));
    try testing.expectEqual(@as(u8, 3), statusColor("unknown"));
}
