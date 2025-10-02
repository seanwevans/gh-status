# GitHub Actions Build Monitor

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
customization of refresh timing and
concurrency:

```sh
./ghstatus [-p seconds] [-c count] <user> [user2 ...]
```

`-p` sets the refresh interval in seconds (default 300) and `-c` limits the
number of simultaneous fetches (default 32).

The tool relies on the GitHub CLI for API requests. To include private or
internal repositories in the results, ensure the CLI is authenticated
(`gh auth login`) with an account that has permission to view them. Without
authentication or appropriate access, only public repositories will appear.

## PL/pgSQL version

The `ghstatus.sql` script defines a PostgreSQL function that retrieves the
latest GitHub Actions workflow run for repositories owned by the provided
usernames. It relies on the [`http` extension](https://github.com/pramsey/pgsql-http)
to query the GitHub API directly from the database.

### Usage

Load the script and call the function with an array of usernames:

```sql
\i ghstatus.sql
SELECT * FROM ghstatus_latest_runs(ARRAY['octocat']);
```
