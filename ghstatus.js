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
  loading: "🌀",
  error: "⚠️",
  default: "➖",
};

function iconFor(status) {
  for (const key in statusIcons) {
    if (status.includes(key)) return statusIcons[key];
  }
  return statusIcons.default;
}

async function fetchRepos(user) {
  try {
    const resp = await fetch(
      `https://api.github.com/users/${user}/repos?per_page=100&type=public`,
    );
    if (!resp.ok) return [];
    const data = await resp.json();
    return data.map((r) => r.full_name);
  } catch (err) {
    console.error("Failed to fetch repos:", err);
    return [];
  }
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
    return `${run.status} ${run.conclusion}`;
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

  const repos = (await Promise.all(users.map(fetchRepos))).flat();

  if (repos.length === 0) {
    list.innerHTML =
      "<li>No repositories found or error fetching repositories</li>";
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
