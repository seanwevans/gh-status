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
user's repositories. Optional flags allow customization of refresh timing and
concurrency:

```sh
./ghstatus [-p seconds] [-c count] <user> [user2 ...]
```

`-p` sets the refresh interval in seconds (default 300) and `-c` limits the
number of simultaneous fetches (default 32).

The tool relies on the GitHub CLI for API requests. To access private
repositories the CLI must be authenticated (`gh auth login`) and the account
must have permission to view those repositories. Without authentication or
appropriate access, private repository information cannot be displayed.

## Nim version

The `ghstatus.nim` program offers a simplified terminal monitor implemented in
Nim that queries the GitHub API directly. Compile with SSL support and run:

```sh
nim c -d:ssl ghstatus.nim
./ghstatus <user> [user2 ...]
```

It prints each repository with an emoji representing the latest workflow run
status.
