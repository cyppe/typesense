import type { K6Env } from "@/services/k6";
import type { EnvVariableKey } from "@/utils/types";
import type { Params } from "k6/http";
import type { Options } from "k6/options";

import { check, sleep } from "k6";
import { SharedArray } from "k6/data";
import http from "k6/http";
import { Counter, Gauge, Rate, Trend } from "k6/metrics";

import { validateK6Environment } from "./k6-utils.ts";

// Custom metrics for stress import
const batchDuration = new Trend("stress_import_batch_duration_ms");
const batchDocCount = new Trend("stress_import_batch_docs");
const importErrors = new Counter("stress_import_errors");
const import503s = new Counter("stress_import_503s");
const importSuccessRate = new Rate("stress_import_success_rate");
const importDocsTotal = new Counter("stress_import_docs_total");
const importDocsSuccess = new Counter("stress_import_docs_success");
const importDocsFailed = new Counter("stress_import_docs_failed");
const memoryActiveBytes = new Gauge("stress_memory_active_bytes");
const memoryAllocatedBytes = new Gauge("stress_memory_allocated_bytes");

// Use SharedArray so data is loaded ONCE and shared across all VUs (read-only).
// Without this, k6 duplicates module-level state per VU → can cause OOM at high VU counts.
const allLines = new SharedArray("stress-import-data", () => {
  const raw = open("../../data/data.json");
  return raw.split("\n").filter((l: string) => l.trim().length > 0);
});
const totalDocs = allLines.length;

// Configuration from env
const STRESS_VUS = parseInt(__ENV.STRESS_VUS ?? "4");
const STRESS_CHUNK_SIZE = parseInt(__ENV.STRESS_CHUNK_SIZE ?? "5000");
const STRESS_DURATION = __ENV.STRESS_DURATION ?? "60s";
const STRESS_BATCH_SIZE = __ENV.STRESS_BATCH_SIZE ?? "40"; // server-side batch_size param

export const options: Options = {
  scenarios: {
    // Parallel chunked import: multiple VUs continuously sending chunks of docs
    // This simulates real-world parallel bulk import (the recommended Typesense pattern)
    parallel_chunked_import: {
      executor: "constant-vus",
      vus: STRESS_VUS,
      duration: STRESS_DURATION,
      env: { SCENARIO: "parallel_chunked_import" },
    },
  },
  tags: {
    commitHash: __ENV.COMMIT_HASH ?? "unknown",
    testType: "stress_import",
  },
};

// Each VU maintains its own offset into the dataset
let vuOffset = 0;

function getNextChunk(): string {
  // Wrap around if we've gone past the end
  if (vuOffset >= totalDocs) {
    vuOffset = 0;
  }

  const start = vuOffset;
  const end = Math.min(start + STRESS_CHUNK_SIZE, totalDocs);
  vuOffset = end;

  return allLines.slice(start, end).join("\n");
}

export default function () {
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

  const baseUrl = `http://${__ENV.HOST}:${__ENV.PORT}`;
  const importUrl = `${baseUrl}/collections/${__ENV.COLLECTION_NAME}/documents/import?batch_size=${STRESS_BATCH_SIZE}&action=upsert`;
  const metricsUrl = `${baseUrl}/metrics.json`;

  const params: Params = {
    headers: {
      "Content-Type": "application/json",
      "X-TYPESENSE-API-KEY": __ENV.API_KEY ?? "xyz",
    },
    timeout: "300000", // 5 minutes per chunk
  };

  const readParams: Params = {
    headers: {
      "X-TYPESENSE-API-KEY": __ENV.API_KEY ?? "xyz",
    },
    timeout: "10000",
  };

  const tags = {
    commitHash: __ENV.COMMIT_HASH ?? "unknown",
    vus: String(STRESS_VUS),
    chunkSize: String(STRESS_CHUNK_SIZE),
  };

  // Get next chunk of docs to import
  const chunk = getNextChunk();
  const docsInChunk = chunk.split("\n").length;

  const startTime = new Date().getTime();
  const res = http.post(importUrl, chunk, params);
  const duration = new Date().getTime() - startTime;

  batchDuration.add(duration, tags);
  batchDocCount.add(docsInChunk, tags);
  importDocsTotal.add(docsInChunk, tags);

  // Track 503 backpressure specifically
  if (res.status === 503) {
    import503s.add(1, tags);
    importErrors.add(1, tags);
    importSuccessRate.add(0, tags);
    console.warn(`VU ${__VU}: Got 503 backpressure after ${duration}ms`);
    // Back off on 503 to avoid thundering herd
    sleep(2 + Math.random() * 3);
    return;
  }

  const success = check(res, {
    "status is 200": (r) => r.status === 200,
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
    console.error(`VU ${__VU}: Import failed with status ${res.status}: ${typeof res.body === "string" ? res.body.substring(0, 200) : "no body"}`);
  }

  // Collect memory metrics periodically (every 5th iteration to avoid overhead)
  if (__ITER % 5 === 0) {
    const metricsRes = http.get(metricsUrl, readParams);
    if (metricsRes.status === 200 && typeof metricsRes.body === "string") {
      try {
        const metrics = JSON.parse(metricsRes.body) as Record<string, string>;
        const memActive = parseFloat(metrics["typesense_memory_active_bytes"] ?? "0");
        const memAllocated = parseFloat(metrics["typesense_memory_allocated_bytes"] ?? "0");
        if (memActive > 0) memoryActiveBytes.add(memActive, tags);
        if (memAllocated > 0) memoryAllocatedBytes.add(memAllocated, tags);
      } catch {
        /* ignore parse errors */
      }
    }
  }
}
