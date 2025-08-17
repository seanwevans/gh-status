const statusIcons = {
  success: "✅",
  failure: "❌",
  cancelled: "🛑",
  skipped: "⏭️",
  timed_out: "⌛",
  action_required: "⛔",
  neutral: "⭕",
  stale: "🥖",
  in_progress: "🔁",
  queued: "📋",
  no_runs: "➖",
  completed: "➖",
  loading: "🌀",
  error: "⚠️",
  default: "➖",
};

function iconFor(status) {
  if (!status) return statusIcons.default;
  for (const [key, icon] of Object.entries(statusIcons)) {
    if (key !== "default" && status.includes(key)) return icon;
  }
  return statusIcons.default;
}

async function fetchRepos(user) {
  const repos = [];
  let errorOccurred = false;
  for (let page = 1; ; page += 1) {
    try {
      const resp = await fetch(
        `https://api.github.com/users/${user}/repos?per_page=100&type=public&page=${page}`,
      );
      if (!resp.ok) break;
      const data = await resp.json();
      repos.push(...data);
      if (data.length < 100) break;
    } catch (err) {
      console.error("Failed to fetch repos:", err);
      errorOccurred = true;
      break;
    }
  }
  const repoNames = repos.map((r) => r.full_name);
  return { names: repoNames, error: errorOccurred };
}

async function fetchStatus(repo) {
  try {
    const resp = await fetch(
      `https://api.github.com/repos/${repo}/actions/runs?per_page=1`,
    );
    if (!resp.ok) return "error";
    const data = await resp.json();
    if (data.workflow_runs.length === 0) return "no_runs";
    const run = data.workflow_runs[0];
    return run.conclusion ? `${run.status} ${run.conclusion}` : run.status;
  } catch (err) {
    console.error("Failed to fetch status:", err);
    return "error";
  }
}

async function load() {
  const input = document.getElementById("users");
  const users = input.value.split(/\s+/).filter(Boolean);
  const list = document.getElementById("results");
  list.innerHTML = "";

  const repoLists = await Promise.all(users.map(fetchRepos));
  const repoFetchFailed = repoLists.some((r) => r.error);
  const repos = repoLists.flatMap((r) => r.names);

  if (repoFetchFailed) {
    list.innerHTML = "<li>⚠️ Error fetching repositories</li>";
    return;
  }

  if (repos.length === 0) {
    list.innerHTML = "<li>No repositories found</li>";
    return;
  }

  for (const repo of repos) {
    const li = document.createElement("li");
    li.textContent = `${repo} - loading`;
    list.appendChild(li);
    fetchStatus(repo).then((status) => {
      if (status === "error") {
        li.textContent = `⚠️ ${repo} - error fetching status`;
        return;
      }
      const icon = iconFor(status);
      li.textContent = `${icon} ${repo} - ${status}`;
    });
  }
}

document.getElementById("load").addEventListener("click", load);
document.getElementById("users").addEventListener("keydown", (event) => {
  if (event.key === "Enter") {
    load();
  }
});
