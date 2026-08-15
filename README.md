# GitHub Actions Build Monitor
<img width="256" alt="Minimalist Green Battery Icon" src="https://github.com/user-attachments/assets/4f512601-8ad3-45a7-bb81-5f4e4d92f277" />

## What it is
a github actions build monitor

https://github.com/user-attachments/assets/879a2730-b4ae-4862-93a5-d5c3f6ed00ba

## Web version

A JavaScript implementation (`index.html` and `ghstatus.js`) fetches public
repositories for the provided GitHub usernames and displays the latest workflow
run status using emoji icons. The site is automatically deployed to GitHub
Pages via the included workflow.

## Terminal version

The `ghstatus.c` program renders the build monitor in a terminal using
`ncurses`.

### Dependencies

- `gcc`
- `libncursesw`
- [GitHub CLI](https://cli.github.com/)

### Build

Run `make` to compile the executable:

```sh
make
```

### Usage

Invoke the program with a GitHub username to show workflow status for that
user's repositories. All repositories the authenticated GitHub CLI account can
access (public, private, and internal) are requested. Optional flags allow
customization of refresh timing, concurrency and hook delivery:

```sh
./ghstatus [-p seconds>=0] [-c count>=1] [-w [addr:]port] [-s secret-file] \
           <user> [user2 ...]
```

`-p` sets the refresh interval in seconds (default 300, minimum 1) and `-c`
limits the number of simultaneous fetches (default 32, minimum 1).

The tool relies on the GitHub CLI for API requests. To include private or
internal repositories in the results, ensure the CLI is authenticated
(`gh auth login`) with an account that has permission to view them. Without
authentication or appropriate access, only public repositories will appear.

### Hook mode

Polling re-queries every repository on a timer, which wastes API quota and
still shows a stale result for up to a full interval. With `-w`, the monitor
listens for GitHub webhook deliveries instead: each delivery refreshes only the
repository it names, so a build that changes state updates within a second and
the repositories that did not change are never re-queried.

```sh
./ghstatus -w 9000 -s ~/.config/ghstatus/secret <user>
```

`-w` takes a port (bound to `127.0.0.1`), a `host:port` pair, or `[v6addr]:port`.
Use `:9000` to bind every interface. `-s` names a file whose first line is the
webhook secret — passing it via a file (or the `GHSTATUS_WEBHOOK_SECRET`
environment variable, which is unset once read) keeps it out of `ps` output and
out of the environment inherited by `gh`. When a secret is configured every
delivery must carry a matching `X-Hub-Signature-256`; without one the monitor
warns at startup and accepts unsigned deliveries.

Configure the hook on GitHub (repository or organization settings → Webhooks)
with content type `application/json` and the `Workflow runs` event. Deliveries
for `workflow_run`, `workflow_job`, `check_run`, `check_suite` and `status`
trigger a refresh; anything else is acknowledged and ignored, as is a delivery
for a repository that is not being displayed.

For a local monitor with no public address, the GitHub CLI can forward
deliveries over your existing authentication:

```sh
gh extension install cli/gh-webhook
gh webhook forward --events=workflow_run --repo=<owner>/<repo> \
  --url=http://127.0.0.1:9000/
```

In hook mode the timer becomes a safety net rather than the primary source of
updates: it reconciles every 900 seconds by default, covering deliveries that
GitHub drops or that arrive while the monitor is down. Pass `-p` to change that
interval, or `-p 0` to disable timed refreshes entirely and rely on hooks alone.
The footer shows a 🪝 counter of accepted deliveries, and the top line reports
the listen address, whether signatures are enforced, and the last repository a
delivery refreshed.

## PL/pgSQL version

The `ghstatus.sql` script defines a PostgreSQL function that retrieves the
latest GitHub Actions workflow run for repositories owned by the provided
usernames. It relies on the [`http` extension](https://github.com/pramsey/pgsql-http)
to query the GitHub API directly from the database. Failed HTTP requests are
logged as PostgreSQL NOTICEs and skipped so that one bad response does not stop
the rest of the usernames from being processed.

### Usage

Load the script and call the function with an array of usernames:

```sql
\i ghstatus.sql
SELECT * FROM ghstatus_latest_runs(ARRAY['octocat']);
```
