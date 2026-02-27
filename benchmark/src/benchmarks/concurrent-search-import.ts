import type { K6Env } from "@/services/k6";
import type { Album, EnvVariableKey } from "@/utils/types";
import type { Options } from "k6/options";
import type { SearchResponse } from "typesense/lib/Typesense/Documents";

import { check, sleep } from "k6";
import { SharedArray } from "k6/data";
import http from "k6/http";
import { Counter, Rate, Trend } from "k6/metrics";

import { searchScenarios, validateK6Environment } from "./k6-utils.ts";

// --- Custom metrics ---

// Search metrics (tracked per scenario)
const searchProcessingTime = new Trend("concurrent_search_processing_time_ms");
const searchTimeouts = new Counter("concurrent_search_timeouts");

// Import metrics
const importBatchDuration = new Trend("concurrent_import_batch_duration_ms");
const importDocsSuccess = new Counter("concurrent_import_docs_success");
const importDocsFailed = new Counter("concurrent_import_docs_failed");
const importDocsTotal = new Counter("concurrent_import_docs_total");
const importErrors = new Counter("concurrent_import_errors");
const import503s = new Counter("concurrent_import_503s");
const importSuccessRate = new Rate("concurrent_import_success_rate");

// --- Configuration ---
const CONCURRENT_DURATION = __ENV.CONCURRENT_DURATION ?? "30s";
const CONCURRENT_SEARCH_VUS = parseInt(__ENV.CONCURRENT_SEARCH_VUS ?? "50");
const CONCURRENT_IMPORT_VUS = parseInt(__ENV.CONCURRENT_IMPORT_VUS ?? "4");
const CONCURRENT_CHUNK_SIZE = parseInt(__ENV.CONCURRENT_CHUNK_SIZE ?? "1000");
const CONCURRENT_BATCH_SIZE = __ENV.CONCURRENT_BATCH_SIZE ?? "40";

// Pick a representative subset of search scenarios for concurrent testing
// Use the heaviest ones that are most sensitive to lock contention
const CONCURRENT_SEARCH_SCENARIOS = ["facet", "sort_eval_score", "filter_simple"] as const;

// --- Load dataset at init time ---
// Use SharedArray so data is loaded ONCE and shared across all VUs (read-only).
// Without this, k6 duplicates module-level state per VU → 360MB × 54 VUs = OOM.
const MAX_IMPORT_DOCS = 50_000;
const allLines = new SharedArray("import-data", () => {
  const raw = open("../../data/data.json");
  return raw.split("\n").filter((l: string) => l.trim().length > 0).slice(0, MAX_IMPORT_DOCS);
});
const totalDocs = allLines.length;

// --- K6 scenario definitions ---
// Two concurrent executor groups: import VUs + search VUs running simultaneously
export const options: Options = {
  scenarios: {
    // Import VUs: continuously push chunks of documents
    concurrent_import: {
      executor: "constant-vus",
      vus: CONCURRENT_IMPORT_VUS,
      duration: CONCURRENT_DURATION,
      exec: "importDocs",
      env: { EXECUTOR: "import" },
    },
    // Search VUs: continuously run search queries
    // Start 3s after import to let some docs land first
    concurrent_search: {
      executor: "constant-vus",
      vus: CONCURRENT_SEARCH_VUS,
      duration: CONCURRENT_DURATION,
      startTime: "3s",
      exec: "searchDocs",
      env: { EXECUTOR: "search" },
    },
  },
  tags: {
    commitHash: __ENV.COMMIT_HASH ?? "unknown",
    testType: "concurrent",
  },
};

// --- Per-VU state for import ---
let vuOffset = 0;

function getNextChunk(): string {
  if (vuOffset >= totalDocs) {
    vuOffset = 0;
  }
  const start = vuOffset;
  const end = Math.min(start + CONCURRENT_CHUNK_SIZE, totalDocs);
  vuOffset = end;
  return allLines.slice(start, end).join("\n");
}

// --- Shared helpers ---
function getEnv(): K6Env {
  const env: EnvVariableKey<K6Env> = {
    API_KEY: __ENV.API_KEY,
    HOST: __ENV.HOST,
    PORT: __ENV.PORT,
    BATCH_SIZE: __ENV.BATCH_SIZE,
    COLLECTION_NAME: __ENV.COLLECTION_NAME,
    COMMIT_HASH: __ENV.COMMIT_HASH,
    DURATION: __ENV.DURATION,
  };

  const validation = validateK6Environment(env);
  if (!validation.isValid) {
    throw new Error(
      `Invalid environment configuration:\n${validation.errors
        .map((err) => `${String(err.key)}: ${err.reason} (got: ${err.value})`)
        .join("\n")}`,
    );
  }
  return validation.env;
}

// --- Import executor ---
export function importDocs(): void {
  const env = getEnv();
  const baseUrl = `http://${env.HOST}:${env.PORT}`;
  const importUrl = `${baseUrl}/collections/${env.COLLECTION_NAME}/documents/import?batch_size=${CONCURRENT_BATCH_SIZE}&action=upsert`;

  const params = {
    headers: {
      "Content-Type": "application/json",
      "X-TYPESENSE-API-KEY": env.API_KEY,
    },
    timeout: "300000",
  };

  const tags = {
    commitHash: env.COMMIT_HASH,
    vus: String(CONCURRENT_IMPORT_VUS),
    chunkSize: String(CONCURRENT_CHUNK_SIZE),
  };

  const chunk = getNextChunk();
  const docsInChunk = chunk.split("\n").length;

  const startTime = new Date().getTime();
  const res = http.post(importUrl, chunk, params);
  const duration = new Date().getTime() - startTime;

  importBatchDuration.add(duration, tags);
  importDocsTotal.add(docsInChunk, tags);

  if (res.status === 503) {
    import503s.add(1, tags);
    importErrors.add(1, tags);
    importSuccessRate.add(0, tags);
    sleep(2 + Math.random() * 3);
    return;
  }

  const success = check(res, {
    "import status is 200": (r) => r.status === 200,
  });

  if (success && typeof res.body === "string") {
    const lines = res.body.split("\n").filter((l) => l.trim());
    const successCount = lines.filter((l) => l.includes('"success":true')).length;
    const errorCount = lines.length - successCount;

    importDocsSuccess.add(successCount, tags);
    importDocsFailed.add(errorCount, tags);
    importSuccessRate.add(1, tags);

    if (errorCount > 0) {
      importErrors.add(errorCount, tags);
    }
  } else {
    importErrors.add(1, tags);
    importSuccessRate.add(0, tags);
  }
}

// --- Search executor ---
export function searchDocs(): void {
  const env = getEnv();
  const baseUrl = `http://${env.HOST}:${env.PORT}/collections/${env.COLLECTION_NAME}/documents/search`;

  const headers: Record<string, string> = {
    accept: "application/json",
    "x-typesense-api-key": env.API_KEY,
    "content-type": "application/json",
  };

  // Round-robin through the concurrent search scenarios
  const scenarioName = CONCURRENT_SEARCH_SCENARIOS[__ITER % CONCURRENT_SEARCH_SCENARIOS.length];
  const scenario = searchScenarios.find((s) => s.name === scenarioName);

  if (!scenario) {
    throw new Error(`Scenario ${scenarioName} not found`);
  }

  const tags = {
    commitHash: env.COMMIT_HASH,
    scenario: scenario.name,
    vus: String(CONCURRENT_SEARCH_VUS),
  };

  const queryParams = Object.entries(scenario.params)
    .map(([key, value]) => `${key}=${encodeURIComponent(String(value))}`)
    .join("&");

  const url = `${baseUrl}?${queryParams}`;
  const response = http.get(url, { headers });

  check(response, { "search status was 200": (r) => r.status === 200 }, { quiet: true });

  if (response.status === 200 && typeof response.body === "string") {
    const body = JSON.parse(response.body) as SearchResponse<Album>;
    searchProcessingTime.add(body.search_time_ms, tags);
  }

  if (response.error?.toLowerCase().includes("timeout")) {
    searchTimeouts.add(1, tags);
  }
}
