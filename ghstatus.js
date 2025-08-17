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
  try {
    for (let page = 1; ; page += 1) {
      const resp = await fetch(
        `https://api.github.com/users/${user}/repos?per_page=100&type=public&page=${page}`,
      );
      if (!resp.ok) throw new Error("Failed to fetch repos");
      const data = await resp.json();
      repos.push(...data);
      if (data.length < 100) break;
    }
  } catch (err) {
    console.error("Failed to fetch repos:", err);
    throw err;
  }
  return repos.map((r) => r.full_name);
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

  const results = await Promise.allSettled(users.map(fetchRepos));
  const repos = [];

  results.forEach((result, index) => {
    if (result.status === "fulfilled") {
      repos.push(...result.value);
    } else {
      const li = document.createElement("li");
      li.textContent = `⚠️ Error fetching repositories for ${users[index]}`;
      list.appendChild(li);
    }
  });

  if (repos.length === 0) {
    if (list.children.length === 0) {
      list.innerHTML = "<li>No repositories found</li>";
    } else {
      const li = document.createElement("li");
      li.textContent = "No repositories found";
      list.appendChild(li);
    }
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
