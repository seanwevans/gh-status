#!/usr/bin/env python3
"""GitHub Actions Build Monitor (Python version).

Fetches public repositories for the given GitHub users and displays the
latest workflow run status for each repository.  This is a lightweight
implementation of the JavaScript version shipped with this repository.

Usage:
    python ghstatus.py [-c COUNT] USER [USER ...]

Options:
    -c COUNT   Maximum number of concurrent status fetches (default 5).
"""
from __future__ import annotations

import argparse
import asyncio
from typing import Iterable, List

import aiohttp


STATUS_ICONS = {
    "success": "✅",
    "failure": "❌",
    "cancelled": "🛑",
    "skipped": "⏭️",
    "timed_out": "⌛",
    "action_required": "⛔",
    "neutral": "⭕",
    "stale": "🥖",
    "in_progress": "🔁",
    "queued": "📋",
    "no_runs": "➖",
    "completed": "➖",
    "loading": "🌀",
    "error": "⚠️",
    "default": "➖",
}


def icon_for(status: str | None) -> str:
    """Return an emoji icon for a status string."""
    if not status:
        return STATUS_ICONS["default"]
    for key, icon in STATUS_ICONS.items():
        if key != "default" and key in status:
            return icon
    return STATUS_ICONS["default"]


async def fetch_repos(session: aiohttp.ClientSession, user: str) -> List[str]:
    """Return a list of full repo names for ``user``.

    Raises ``RuntimeError('rate_limit')`` on API rate limit or
    ``RuntimeError('error')`` on other failures.
    """
    repos: List[str] = []
    page = 1
    while True:
        url = (
            f"https://api.github.com/users/{user}/repos?per_page=100&type=public&page={page}"
        )
        async with session.get(url) as resp:
            if resp.status == 403 and resp.headers.get("X-RateLimit-Remaining") == "0":
                raise RuntimeError("rate_limit")
            if resp.status != 200:
                raise RuntimeError("error")
            data = await resp.json()
        repos.extend(r["full_name"] for r in data)
        if len(data) < 100:
            break
        page += 1
    return repos


async def fetch_status(session: aiohttp.ClientSession, repo: str) -> str:
    """Return the workflow run status for ``repo``."""
    url = f"https://api.github.com/repos/{repo}/actions/runs?per_page=1"
    async with session.get(url) as resp:
        if resp.status == 403 and resp.headers.get("X-RateLimit-Remaining") == "0":
            return "rate_limit"
        if resp.status != 200:
            return "error"
        data = await resp.json()
    runs = data.get("workflow_runs", [])
    if not runs:
        return "no_runs"
    run = runs[0]
    conclusion = run.get("conclusion")
    status = run.get("status", "")
    return f"{status} {conclusion}" if conclusion else status


async def gather_statuses(users: Iterable[str], limit: int) -> None:
    """Fetch and display statuses for ``users`` with concurrency ``limit``."""
    connector = aiohttp.TCPConnector(limit_per_host=limit)
    async with aiohttp.ClientSession(connector=connector) as session:
        repos: List[str] = []
        for user in users:
            try:
                repos.extend(await fetch_repos(session, user))
            except RuntimeError as exc:
                print(f"⚠️  {user}: {exc}")
        if not repos:
            print("No repositories found")
            return
        sem = asyncio.Semaphore(limit)

        async def worker(repo: str) -> None:
            async with sem:
                status = await fetch_status(session, repo)
                if status == "rate_limit":
                    print(f"⚠️  {repo} - rate limit exceeded")
                    return
                if status == "error":
                    print(f"⚠️  {repo} - error fetching status")
                    return
                icon = icon_for(status)
                print(f"{icon} {repo} - {status}")

        await asyncio.gather(*(worker(repo) for repo in repos))


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("users", nargs="+", help="GitHub usernames")
    parser.add_argument(
        "-c",
        "--concurrency",
        type=int,
        default=5,
        help="maximum number of concurrent status requests (default: 5)",
    )
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> None:
    args = parse_args(argv)
    try:
        asyncio.run(gather_statuses(args.users, args.concurrency))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
