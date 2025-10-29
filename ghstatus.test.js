const test = require("node:test");
const assert = require("node:assert/strict");

const { accumulateSummary, resetSummary, summaryState } = require("./ghstatus.js");

test("accumulateSummary ignores active runs when averaging durations", () => {
  resetSummary();

  const baseTime = Date.UTC(2024, 0, 1, 0, 0, 0);
  const runs = [
    {
      status: "in_progress",
      conclusion: null,
      created_at: new Date(baseTime - 4_000).toISOString(),
      updated_at: new Date(baseTime).toISOString(),
    },
    {
      status: "completed",
      conclusion: "success",
      created_at: new Date(baseTime - 12_000).toISOString(),
      updated_at: new Date(baseTime - 7_000).toISOString(),
    },
    {
      status: "queued",
      conclusion: null,
      created_at: new Date(baseTime - 20_000).toISOString(),
      updated_at: new Date(baseTime - 15_000).toISOString(),
    },
    {
      status: "completed",
      conclusion: null,
      created_at: new Date(baseTime - 40_000).toISOString(),
      updated_at: new Date(baseTime - 30_000).toISOString(),
    },
  ];

  accumulateSummary(runs);

  assert.equal(summaryState.repos, 1);
  assert.equal(summaryState.totalRuns, runs.length);
  assert.equal(summaryState.inProgress, 2);
  assert.equal(summaryState.durationSamples, 2);
  assert.equal(summaryState.durations, 15_000);
});
