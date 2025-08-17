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
  default: "➖",
};

function iconFor(status) {
  for (const key in statusIcons) {
    if (status.includes(key)) return statusIcons[key];
  }
  return statusIcons.default;
}

async function fetchRepos(user) {
  const repos = [];
  for (let page = 1; ; page += 1) {
    const resp = await fetch(
      `https://api.github.com/users/${user}/repos?per_page=100&type=public&page=${page}`,
    );
    if (!resp.ok) break;
    const data = await resp.json();
    repos.push(...data);
    if (data.length < 100) break;
  }
  return repos.map((r) => r.full_name);
}

async function fetchStatus(repo) {
  const resp = await fetch(
    `https://api.github.com/repos/${repo}/actions/runs?per_page=1`,
  );
  if (!resp.ok) return "unknown";
  const data = await resp.json();
  if (data.workflow_runs.length === 0) return "no_runs";
  const run = data.workflow_runs[0];
  return `${run.status} ${run.conclusion}`;
}

async function load() {
  const input = document.getElementById("users");
  const users = input.value.split(/\s+/).filter(Boolean);
  const list = document.getElementById("results");
  list.innerHTML = "";

  const repos = (await Promise.all(users.map(fetchRepos))).flat();

  for (const repo of repos) {
    const li = document.createElement("li");
    li.textContent = `${repo} - loading`;
    list.appendChild(li);
    fetchStatus(repo).then((status) => {
      const icon = iconFor(status);
      li.textContent = `${icon} ${repo} - ${status}`;
    });
  }
}

document.getElementById("load").addEventListener("click", load);
