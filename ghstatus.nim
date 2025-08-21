import httpclient, json, os, strformat, strutils, tables

let statusIcons = {
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
  "default": "➖"
}.toTable

proc iconFor(status: string): string =
  for key, icon in statusIcons:
    if key != "default" and status.contains(key):
      return icon
  return statusIcons["default"]

proc fetchRepos(user: string): seq[string] =
  var repos: seq[string] = @[]
  var page = 1
  let client = newHttpClient(userAgent = "ghstatus")
  try:
    while true:
      let url = fmt"https://api.github.com/users/{user}/repos?per_page=100&type=public&page={page}"
      let resp = client.getContent(url)
      let data = parseJson(resp)
      if data.len == 0:
        break
      for repo in data:
        repos.add repo["full_name"].getStr()
      if data.len < 100:
        break
      inc page
  except CatchableError:
    discard
  finally:
    client.close()
  return repos

proc fetchStatus(repo: string): string =
  let client = newHttpClient(userAgent = "ghstatus")
  try:
    let url = fmt"https://api.github.com/repos/{repo}/actions/runs?per_page=1"
    let resp = client.getContent(url)
    let data = parseJson(resp)
    let runs = data["workflow_runs"]
    if runs.len == 0:
      return "no_runs"
    let run = runs[0]
    if run.hasKey("conclusion") and run["conclusion"].getStr() != "":
      return run["status"].getStr() & " " & run["conclusion"].getStr()
    else:
      return run["status"].getStr()
  except CatchableError:
    return "error"
  finally:
    client.close()

when isMainModule:
  let users = commandLineParams()
  if users.len == 0:
    echo "Usage: ghstatus <user1> [user2 ...]"
    quit(1)
  var repos: seq[string] = @[]
  for user in users:
    repos.add(fetchRepos(user))
  if repos.len == 0:
    echo "No repositories found"
    quit(0)
  for repo in repos:
    let status = fetchStatus(repo)
    let icon = iconFor(status)
    echo icon, " ", repo, " - ", status
