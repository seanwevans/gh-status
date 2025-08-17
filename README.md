# GitHub Actions Build Monitor

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
user's repositories:

```sh
./ghstatus <user>
```

The tool relies on the GitHub CLI for API requests. To access private
repositories the CLI must be authenticated (`gh auth login`) and the account
must have permission to view those repositories. Without authentication or
appropriate access, private repository information cannot be displayed.
