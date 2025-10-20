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

const statusColors = {
  success: "var(--success)",
  completed: "var(--success)",
  failure: "var(--failure)",
  cancelled: "var(--cancelled)",
  timed_out: "var(--failure)",
  neutral: "var(--neutral)",
  in_progress: "var(--in-progress)",
  queued: "var(--accent)",
  waiting: "var(--accent)",
  action_required: "var(--failure)",
  stale: "var(--muted)",
  skipped: "var(--muted)",
  null: "var(--muted)",
};

const RUN_HISTORY_LIMIT = 12;
const runJobsCache = new Map();

function escapeHtml(value) {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

let CONCURRENCY_LIMIT = window.CONCURRENCY_LIMIT || 5;
window.CONCURRENCY_LIMIT = CONCURRENCY_LIMIT;

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

function getStatusColor(status, fallback = "var(--muted)") {
  if (!status) return fallback;
  const normalized = status.toLowerCase();
  return statusColors[normalized] || fallback;
}

function formatDuration(ms) {
  if (!Number.isFinite(ms) || ms <= 0) return "&mdash;";
  const totalSeconds = Math.floor(ms / 1000);
  const parts = [];
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  if (hours) parts.push(`${hours}h`);
  if (minutes) parts.push(`${minutes}m`);
  if (!hours && seconds) parts.push(`${seconds}s`);
  return parts.length > 0 ? parts.join(" ") : "&lt;1s";
}

function formatDateTime(value) {
  if (!value) return "Unknown";
  const date = new Date(value);
  return date.toLocaleString(undefined, {
    hour: "2-digit",
    minute: "2-digit",
    year: "numeric",
    month: "short",
    day: "numeric",
  });
}

const relativeFormatter = new Intl.RelativeTimeFormat(undefined, {
  style: "narrow",
});

function formatRelative(value) {
  if (!value) return "Unknown time";
  const now = Date.now();
  const target = new Date(value).getTime();
  const diffSeconds = Math.round((target - now) / 1000);
  const divisions = [
    { amount: 60, unit: "second" },
    { amount: 60, unit: "minute" },
    { amount: 24, unit: "hour" },
    { amount: 7, unit: "day" },
    { amount: 4.34524, unit: "week" },
    { amount: 12, unit: "month" },
    { amount: Number.POSITIVE_INFINITY, unit: "year" },
  ];

  let duration = diffSeconds;
  let unit = "second";

  for (const division of divisions) {
    if (Math.abs(duration) < division.amount) {
      unit = division.unit;
      break;
    }
    duration /= division.amount;
  }

  return relativeFormatter.format(Math.round(duration), unit);
}

function getRunStatusKey(run) {
  if (!run) return "no_runs";
  if (run.conclusion) return run.conclusion.toLowerCase();
  if (run.status) return run.status.toLowerCase();
  return "unknown";
}

function summariseRunStatus(run) {
  const statusKey = getRunStatusKey(run);
  const { label } = getStatusDetails(statusKey);
  return label.replace(/^Conclusion: |^Status: /i, "");
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

async function fetchRuns(repo) {
  try {
    const resp = await fetch(
      `https://api.github.com/repos/${repo}/actions/runs?per_page=${RUN_HISTORY_LIMIT}`,
    );
    if (
      resp.status === 403 &&
      resp.headers.get("X-RateLimit-Remaining") === "0"
    ) {
      return { error: "rate_limit" };
    }
    if (!resp.ok) return { error: "error" };
    const data = await resp.json();
    return { runs: data.workflow_runs || [] };
  } catch (err) {
    console.error("Failed to fetch runs:", err);
    return { error: "error" };
  }
}

async function fetchRunJobs(run) {
  if (!run || !run.jobs_url) return { jobs: [] };
  if (runJobsCache.has(run.id)) {
    return runJobsCache.get(run.id);
  }

  const promise = (async () => {
    const jobs = [];
    for (let page = 1; ; page++) {
      const url = `${run.jobs_url}?per_page=100&page=${page}`;
      try {
        const resp = await fetch(url);
        if (
          resp.status === 403 &&
          resp.headers.get("X-RateLimit-Remaining") === "0"
        ) {
          return { error: "rate_limit" };
        }
        if (!resp.ok) {
          return { error: "error" };
        }
        const data = await resp.json();
        jobs.push(...(data.jobs || []));
        if (!data.jobs || data.jobs.length === 0 || jobs.length >= data.total_count) {
          break;
        }
      } catch (err) {
        console.error("Failed to fetch run jobs:", err);
        return { error: "error" };
      }
    }
    return { jobs };
  })();

  runJobsCache.set(run.id, promise);
  return promise;
}

function createRepoCard(repo) {
  const safeRepo = escapeHtml(repo);
  const container = document.createElement("article");
  container.className = "repo-card";
  container.innerHTML = `
    <header>
      <h3 class="repo-name">${safeRepo}</h3>
      <span class="latest-status"><span class="status-icon">🌀</span> Loading</span>
    </header>
    <div class="timeline" role="listbox" aria-label="Recent workflow runs">
      <span class="placeholder">Fetching workflow history…</span>
    </div>
    <div class="run-details placeholder">Select a run to inspect job durations.</div>
  `;
  return container;
}

function setTimelineActive(timeline, activeButton) {
  for (const button of timeline.querySelectorAll("button")) {
    button.classList.toggle("active", button === activeButton);
  }
}

function renderRunDetails(container, run) {
  const details = container.querySelector(".run-details");
  if (!run) {
    details.classList.add("placeholder");
    details.innerHTML = "No workflow runs yet.";
    delete details.dataset.runId;
    return;
  }

  details.classList.remove("placeholder");
  details.dataset.runId = String(run.id);
  const statusKey = getRunStatusKey(run);
  const statusText = summariseRunStatus(run);
  const color = getStatusColor(statusKey);
  const startedAt = run.created_at || run.run_started_at;
  const completedAt = run.updated_at || run.run_completed_at;
  const duration = startedAt && completedAt ? new Date(completedAt) - new Date(startedAt) : null;
  const runName = escapeHtml(run.name || "Workflow run");
  const runNumber = run.run_number ?? "";
  const runNumberLabel = runNumber !== "" ? ` #${escapeHtml(runNumber)}` : "";
  const htmlUrl = run.html_url ? escapeHtml(run.html_url) : "#";
  const event = escapeHtml(run.event || "unknown");
  const startedText = escapeHtml(formatDateTime(startedAt));
  const relativeText = escapeHtml(formatRelative(startedAt));

  details.innerHTML = `
    <h4>
      <span>${runName}${runNumberLabel}</span>
      <a href="${htmlUrl}" target="_blank" rel="noopener">View on GitHub</a>
    </h4>
    <div>
      <strong>Status:</strong>
      <span class="job-status">
        <span class="status-dot" style="background:${color}"></span>
        ${statusText}
      </span>
    </div>
    <div><strong>Event:</strong> ${event}</div>
    <div><strong>Started:</strong> ${startedText} (${relativeText})</div>
    <div><strong>Duration:</strong> ${formatDuration(duration)}
    </div>
    <div class="job-analytics"><span class="placeholder">Loading job analytics…</span></div>
  `;

  fetchRunJobs(run).then((result) => {
    const target = details.querySelector(".job-analytics");
    if (!target) return;
    if (details.dataset.runId !== String(run.id)) {
      return;
    }

    if (result.error === "rate_limit") {
      target.innerHTML =
        "⚠️ GitHub API rate limit hit while loading jobs. Try again shortly.";
      target.classList.add("placeholder");
      return;
    }

    if (result.error) {
      target.innerHTML = "⚠️ Unable to load jobs for this run.";
      target.classList.add("placeholder");
      return;
    }

    const jobs = result.jobs || [];
    if (jobs.length === 0) {
      target.innerHTML = "No jobs reported for this run.";
      target.classList.add("placeholder");
      return;
    }

    target.classList.remove("placeholder");
    const rows = jobs
      .map((job) => {
        const jobStatusKey = job.conclusion || job.status || "unknown";
        const jobColor = getStatusColor(jobStatusKey);
        const started = job.started_at ? new Date(job.started_at) : null;
        const completed = job.completed_at ? new Date(job.completed_at) : null;
        const jobDuration = started && completed ? completed - started : null;
        const jobName = escapeHtml(job.name || "Unnamed job");
        const jobStatus = escapeHtml(describeStatus(job.conclusion || job.status, "Unknown"));
        return `
          <tr>
            <td>${jobName}</td>
            <td>
              <span class="job-status">
                <span class="status-dot" style="background:${jobColor}"></span>
                ${jobStatus}
              </span>
            </td>
            <td>${formatDuration(jobDuration)}</td>
          </tr>
        `;
      })
      .join("");

    target.innerHTML = `
      <table>
        <thead>
          <tr><th>Job</th><th>Status</th><th>Duration</th></tr>
        </thead>
        <tbody>${rows}</tbody>
      </table>
    `;
  });
}

function renderTimeline(container, repo, runs) {
  const timeline = container.querySelector(".timeline");
  timeline.innerHTML = "";

  if (!runs || runs.length === 0) {
    timeline.innerHTML = "<span class=\"placeholder\">No workflow history yet.</span>";
    container.querySelector(".run-details").innerHTML =
      "<span class=\"placeholder\">No workflow runs available.</span>";
    return;
  }

  runs.forEach((run, index) => {
    const button = document.createElement("button");
    button.type = "button";
    const statusKey = getRunStatusKey(run);
    const color = getStatusColor(statusKey);
    const label = summariseRunStatus(run);
    button.style.color = color;
    button.innerHTML = `<span class="dot"></span>#${run.run_number ?? index + 1}`;
    button.title = `${label} • triggered ${formatRelative(run.created_at)}`;
    button.addEventListener("click", () => {
      setTimelineActive(timeline, button);
      renderRunDetails(container, run);
    });
    timeline.appendChild(button);

    if (index === 0) {
      button.classList.add("active");
      renderRunDetails(container, run);
    }
  });
}

function updateRepoHeader(container, run) {
  const statusSpan = container.querySelector(".latest-status");
  const icon = statusSpan.querySelector(".status-icon");
  const statusKey = getRunStatusKey(run);
  setStatusIcon(icon, statusKey || "no_runs");
  statusSpan.lastChild.remove?.();
  statusSpan.append(document.createTextNode(` ${summariseRunStatus(run)}`));
}

function showMessage(message, type = "info") {
  const el = document.getElementById("message");
  el.textContent = message;
  el.dataset.type = type;
  el.classList.add("visible");
}

function clearMessage() {
  const el = document.getElementById("message");
  el.textContent = "";
  el.dataset.type = "";
  el.classList.remove("visible");
}

const summaryState = {
  repos: 0,
  totalRuns: 0,
  durations: 0,
  durationSamples: 0,
  latest: {
    success: 0,
    failure: 0,
    other: 0,
  },
  inProgress: 0,
};

function resetSummary() {
  summaryState.repos = 0;
  summaryState.totalRuns = 0;
  summaryState.durations = 0;
  summaryState.durationSamples = 0;
  summaryState.latest.success = 0;
  summaryState.latest.failure = 0;
  summaryState.latest.other = 0;
  summaryState.inProgress = 0;
}

function accumulateSummary(runs) {
  summaryState.repos += 1;
  if (!runs || runs.length === 0) {
    summaryState.latest.other += 1;
    return;
  }
  summaryState.totalRuns += runs.length;

  const latest = runs[0];
  const latestKey = getRunStatusKey(latest);
  if (latestKey === "success") summaryState.latest.success += 1;
  else if (latestKey === "failure") summaryState.latest.failure += 1;
  else summaryState.latest.other += 1;

  runs.forEach((run) => {
    const key = getRunStatusKey(run);
    if (key === "in_progress" || key === "queued") {
      summaryState.inProgress += 1;
    }
    const started = run.created_at ? new Date(run.created_at) : null;
    const completed = run.updated_at ? new Date(run.updated_at) : null;
    if (started && completed && completed >= started) {
      summaryState.durations += completed - started;
      summaryState.durationSamples += 1;
    }
  });
}

function renderSummaryPanel() {
  const panel = document.getElementById("summary");
  if (summaryState.repos === 0) {
    panel.hidden = true;
    panel.innerHTML = "";
    return;
  }

  const average = summaryState.durationSamples
    ? formatDuration(summaryState.durations / summaryState.durationSamples)
    : "&mdash;";

  panel.hidden = false;
  panel.innerHTML = `
    <div class="summary-card">
      <span class="label">Repositories observed</span>
      <span class="value">${summaryState.repos}</span>
    </div>
    <div class="summary-card">
      <span class="label">Passing latest runs</span>
      <span class="value">${summaryState.latest.success}</span>
    </div>
    <div class="summary-card">
      <span class="label">Failing latest runs</span>
      <span class="value">${summaryState.latest.failure}</span>
    </div>
    <div class="summary-card">
      <span class="label">Other latest states</span>
      <span class="value">${summaryState.latest.other}</span>
    </div>
    <div class="summary-card">
      <span class="label">Runs in flight</span>
      <span class="value">${summaryState.inProgress}</span>
    </div>
    <div class="summary-card">
      <span class="label">Avg. runtime</span>
      <span class="value">${average}</span>
    </div>
  `;
}

async function load(event) {
  event?.preventDefault?.();
  clearMessage();
  resetSummary();

  const form = document.getElementById("user-form");
  const button = document.getElementById("load");
  if (button.disabled) {
    return;
  }
  const input = document.getElementById("users");
  const repoContainer = document.getElementById("repos");
  repoContainer.innerHTML = "";
  renderSummaryPanel();

  const users = input.value.split(/\s+/).filter(Boolean);
  if (users.length === 0) {
    showMessage("Please provide at least one GitHub username.", "warning");
    return;
  }

  button.disabled = true;
  form.classList.add("loading");

  let repoResults;
  try {
    repoResults = await Promise.allSettled(users.map(fetchRepos));
  } catch (err) {
    console.error("Failed to load repositories:", err);
    showMessage("⚠️ Error loading repositories.", "error");
    button.disabled = false;
    form.classList.remove("loading");
    return;
  }

  const rateLimited = repoResults.some(
    (r) => r.status === "fulfilled" && r.value.error === "rate_limit",
  );
  if (rateLimited) {
    showMessage("⚠️ GitHub API rate limit reached while fetching repositories.", "error");
    button.disabled = false;
    form.classList.remove("loading");
    return;
  }

  const repoFetchFailed = repoResults.some(
    (r) =>
      r.status === "rejected" ||
      (r.status === "fulfilled" && r.value.error && r.value.error !== "rate_limit"),
  );

  if (repoFetchFailed) {
    const failedUsers = repoResults
      .map((r, index) =>
        r.status === "fulfilled" && r.value.error && r.value.error !== "rate_limit"
          ? users[index]
          : null,
      )
      .filter(Boolean);
    if (failedUsers.length) {
      showMessage(`⚠️ Error fetching repositories for: ${failedUsers.join(", ")}.`, "error");
    }
  }

  const repoLists = repoResults.map((r) =>
    r.status === "fulfilled" ? r.value.names : [],
  );

  const repos = Array.from(new Set(repoLists.flat())).sort((a, b) =>
    a.localeCompare(b, undefined, { sensitivity: "base" }),
  );

  if (repos.length === 0) {
    if (!repoFetchFailed) {
      showMessage("No repositories found for the provided usernames.", "info");
    }
    button.disabled = false;
    form.classList.remove("loading");
    return;
  }

  const limiter = limitConcurrency(CONCURRENCY_LIMIT);

  const fetchTasks = repos.map((repo) => {
    const card = createRepoCard(repo);
    repoContainer.appendChild(card);

    return limiter(async () => {
      const { runs, error } = await fetchRuns(repo);
      if (error === "rate_limit") {
        card.querySelector(".timeline").innerHTML =
          "<span class=\"placeholder\">Rate limit exceeded for workflow runs.</span>";
        card.querySelector(".run-details").innerHTML =
          "<span class=\"placeholder\">Unable to load run details.</span>";
        showMessage(
          "⚠️ GitHub API rate limit reached while fetching workflow runs.",
          "error",
        );
        return;
      }
      if (error) {
        card.querySelector(".timeline").innerHTML =
          "<span class=\"placeholder\">Unable to load workflow history.</span>";
        card.querySelector(".run-details").innerHTML =
          "<span class=\"placeholder\">Run details unavailable.</span>";
        return;
      }

      const sortedRuns = [...runs].sort(
        (a, b) => new Date(b.created_at).getTime() - new Date(a.created_at).getTime(),
      );
      accumulateSummary(sortedRuns);
      renderTimeline(card, repo, sortedRuns);
      updateRepoHeader(card, sortedRuns[0]);
      renderSummaryPanel();
    });
  });

  await Promise.all(fetchTasks);

  button.disabled = false;
  form.classList.remove("loading");
}

document.getElementById("user-form").addEventListener("submit", load);
document.getElementById("load").addEventListener("click", load);

document.getElementById("users").addEventListener("keydown", (event) => {
  if (event.key === "Enter" && !event.shiftKey) {
    event.preventDefault();
    load(event);
  }
});

// Auto-load the default example when the page is first opened.
window.addEventListener("DOMContentLoaded", () => {
  load();
});
