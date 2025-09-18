const statusMap = [
  { match: "success", icon: "✅", label: "Conclusion: success" },
  { match: "failure", icon: "❌", label: "Conclusion: failure" },
  { match: "timed_out", icon: "⌛", label: "Conclusion: timed out" },
  { match: "cancelled", icon: "🛑", label: "Conclusion: cancelled" },
  { match: "skipped", icon: "⏭️", label: "Conclusion: skipped" },
  { match: "in_progress", icon: "🔁", label: "Status: in progress" },
  { match: "action_required", icon: "⛔", label: "Status: action required" },
  { match: "neutral", icon: "⭕", label: "Conclusion: neutral" },
  { match: "stale", icon: "🥖", label: "Status: stale" },
  { match: "queued", icon: "📋", label: "Status: queued" },
  { match: "loading", icon: "🌀", label: "Status: loading" },
  { match: "no_runs", icon: "➖", label: "No workflow runs" },
  { match: "rate_limit", icon: "⚠️", label: "Rate limit exceeded" },
  { match: "error", icon: "⚠️", label: "Error fetching status" },
  { match: null, icon: "➖", label: "Unknown status" },
];

// Maximum number of concurrent status requests. Adjust via console for testing.
let CONCURRENCY_LIMIT = window.CONCURRENCY_LIMIT || 5;
window.CONCURRENCY_LIMIT = CONCURRENCY_LIMIT;

// A tiny concurrency limiter similar to p-limit. It ensures that at most
// `limit` async operations are running at any given time.
function limitConcurrency(limit) {
  let active = 0;
  const queue = [];

  const next = () => {
    if (queue.length === 0 || active >= limit) return;
    active++;
    const { fn, resolve, reject } = queue.shift();
    fn()
      .then(resolve, reject)
      .finally(() => {
        active--;
        next();
      });
  };

  return function run(fn) {
    return new Promise((resolve, reject) => {
      queue.push({ fn, resolve, reject });
      next();
    });
  };
}

function getStatusDetails(status) {
  const normalized = (status || "").toLowerCase();
  for (const entry of statusMap) {
    if (entry.match && normalized.includes(entry.match)) {
      return entry;
    }
  }
  return statusMap[statusMap.length - 1];
}

function describeStatus(status, fallback) {
  if (!status) return fallback;
  return status
    .split(/\s+/)
    .map((part) => part.replace(/_/g, " "))
    .join(" ");
}

function setStatusIcon(iconElement, status) {
  const { icon, label } = getStatusDetails(status);
  iconElement.textContent = icon;
  iconElement.title = describeStatus(status, label);
}

function createRepoListItem(repo, status) {
  const li = document.createElement("li");
  const iconSpan = document.createElement("span");
  iconSpan.className = "status-icon";
  li.appendChild(iconSpan);
  li.appendChild(document.createTextNode(` ${repo}`));
  setStatusIcon(iconSpan, status);
  return { li, iconSpan };
}

async function fetchRepos(user) {
  const repos = [];
  let error = null;

  for (let page = 1; ; page++) {
    try {
      const resp = await fetch(
        `https://api.github.com/users/${user}/repos?per_page=100&type=public&page=${page}`,
      );
      if (
        resp.status === 403 &&
        resp.headers.get("X-RateLimit-Remaining") === "0"
      ) {
        error = "rate_limit";
        break;
      }
      if (!resp.ok) {
        error = "error";
        break;
      }
      const data = await resp.json();
      repos.push(...data);
      if (data.length < 100) break;
    } catch (err) {
      console.error("Failed to fetch repos:", err);
      error = "error";
      break;
    }
  }

  const repoNames = repos.map((r) => r.full_name);
  return { names: repoNames, error };
}

async function fetchStatus(repo) {
  try {
    const resp = await fetch(
      `https://api.github.com/repos/${repo}/actions/runs?per_page=1`,
    );
    if (
      resp.status === 403 &&
      resp.headers.get("X-RateLimit-Remaining") === "0"
    ) {
      return "rate_limit";
    }
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

  let repoResults;
  try {
    repoResults = await Promise.allSettled(users.map(fetchRepos));
  } catch (err) {
    console.error("Failed to load repositories:", err);
    list.innerHTML = "<li>⚠️ Error loading repositories</li>";
    return;
  }

  // Determine if any repository fetch requests hit the rate limit or failed
  const rateLimited = repoResults.some(
    (r) => r.status === "fulfilled" && r.value.error === "rate_limit",
  );
  const repoFetchFailed = repoResults.some(
    (r) =>
      r.status === "rejected" ||
      (r.status === "fulfilled" &&
        r.value.error &&
        r.value.error !== "rate_limit"),
  );
  const repoLists = repoResults.map((r) =>
    r.status === "fulfilled" ? r.value : { names: [], error: "error" },
  );

  if (rateLimited) {
    list.innerHTML = "<li>⚠️ Rate limit exceeded</li>";
    return;
  }

  if (repoFetchFailed) {
    const failedUsers = repoLists
      .map((r, index) =>
        r.error && r.error !== "rate_limit" ? users[index] : null,
      )
      .filter(Boolean);
    const li = document.createElement("li");
    li.textContent = `⚠️ Error fetching repositories for: ${failedUsers.join(
      ", ",
    )}`;
    list.appendChild(li);

    if (failedUsers.length === users.length) return;
  }

  const repos = repoLists.filter((r) => !r.error).flatMap((r) => r.names);

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

  const limiter = limitConcurrency(CONCURRENCY_LIMIT);
  for (const repo of repos) {
    const { li, iconSpan } = createRepoListItem(repo, "loading");
    list.appendChild(li);

    limiter(() => fetchStatus(repo)).then((status) => {
      setStatusIcon(iconSpan, status);
    });
  }
}

document.getElementById("load").addEventListener("click", load);
document.getElementById("users").addEventListener("keydown", (event) => {
  if (event.key === "Enter") {
    load();
  }
});
