import type { K6Env } from "@/services/k6";
import type { EnvVariableKey } from "@/utils/types";
import type { Params } from "k6/http";
import type { Options } from "k6/options";

import { check, sleep } from "k6";
import http from "k6/http";
import { Trend } from "k6/metrics";

import { validateK6Environment } from "./k6-utils.ts";

const importDuration = new Trend("import_duration");
const indexSummaryPrefix = "K6_INDEX_SUMMARY_JSON:";
const responseSnippetLength = 500;
const acceptedImportStatuses = new Set([200]);
const collectionSummaryTimeoutMs = "10000";
const collectionSummaryPollAttempts = 8;
const collectionSummaryPollIntervalSeconds = 3;
const indexMaxDuration = __ENV.INDEX_MAX_DURATION ?? "10m";
const fileContent = open("../../data/data.json", "b");
const expectedDocumentCount = 10 ** 6;

function summarizeImportResponse(body: string): {
  lineCount: number;
  successCount: number;
  errorCount: number;
  snippet: string;
} {
  const lines = body
    .split("\n")
    .map((line) => line.trim())
    .filter(Boolean);
  const successCount = lines.filter((line) => line.includes('"success":true')).length;

  return {
    lineCount: lines.length,
    successCount,
    errorCount: lines.length - successCount,
    snippet: body.substring(0, responseSnippetLength),
  };
}

function readImportedDocumentCount(collectionUrl: string, params: Params): {
  bodySnippet: string;
  documentCount: number;
  error: string;
  status: number;
} {
  const res = http.get(collectionUrl, params);
  const body = typeof res.body === "string" ? res.body : "";
  const bodySnippet = body.substring(0, responseSnippetLength);

  if (res.status !== 200) {
    return {
      bodySnippet,
      documentCount: -1,
      error: res.error ?? "",
      status: res.status,
    };
  }

  try {
    const parsed = JSON.parse(body) as { num_documents?: number };
    return {
      bodySnippet,
      documentCount: typeof parsed.num_documents === "number" ? parsed.num_documents : -1,
      error: "",
      status: res.status,
    };
  } catch (error) {
    return {
      bodySnippet,
      documentCount: -1,
      error: error instanceof Error ? error.message : String(error),
      status: res.status,
    };
  }
}

export const options: Options = {
  scenarios: {
    index_import: {
      executor: "per-vu-iterations",
      vus: 1,
      iterations: 1,
      maxDuration: indexMaxDuration,
    },
  },
  tags: {
    commitHash: __ENV.COMMIT_HASH ?? "unknown",
  },
};

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

  const baseUrl = `http://${__ENV.HOST}:${__ENV.PORT}/collections/${__ENV.COLLECTION_NAME}/documents/import`;

  const queryParamsMap = {
    batch_size: __ENV.BATCH_SIZE,
  };

  const queryParams = Object.entries(queryParamsMap)
    .filter(([, value]) => value !== undefined)
    .map(([key, value]) => `${key}=${value}`)
    .join("&");

  const url = queryParams ? `${baseUrl}?${queryParams}` : baseUrl;

  const params: Params = {
    headers: {
      "Content-Type": "application/json",
      "X-TYPESENSE-API-KEY": __ENV.API_KEY ?? "xyz",
    },
    timeout: 30_0000, // 5 minutes
  };
  const readParams: Params = {
    headers: {
      "X-TYPESENSE-API-KEY": __ENV.API_KEY ?? "xyz",
    },
    timeout: collectionSummaryTimeoutMs,
  };

  const benchmarkStart = new Date().getTime();
  const res = http.post(url, fileContent, params);
  const importSummary =
    typeof res.body === "string"
      ? summarizeImportResponse(res.body)
      : {
          lineCount: 0,
          successCount: 0,
          errorCount: 0,
          snippet: "<non-string body>",
        };

  const responseContractWarnings =
    res.status === 200 &&
    importSummary.lineCount === expectedDocumentCount &&
    importSummary.successCount === expectedDocumentCount
      ? 0
      : 1;

  if (responseContractWarnings !== 0) {
    console.warn(
      [
        `Index benchmark import response contract drift for ${url}`,
        `status=${res.status}`,
        `expected_docs=${expectedDocumentCount}`,
        `response_line_count=${importSummary.lineCount}`,
        `success_count=${importSummary.successCount}`,
        `error_count=${importSummary.errorCount}`,
        `response_snippet=${importSummary.snippet}`,
      ].join(" "),
    );
  }

  const importOnlyDuration = new Date().getTime() - benchmarkStart;
  const collectionUrl = `http://${__ENV.HOST}:${__ENV.PORT}/collections/${__ENV.COLLECTION_NAME}`;
  let lastCollectionSummary = {
    bodySnippet: "",
    documentCount: -1,
    error: "",
    status: 0,
  };

  for (let attempt = 0; attempt < collectionSummaryPollAttempts; attempt += 1) {
    lastCollectionSummary = readImportedDocumentCount(collectionUrl, readParams);
    if (lastCollectionSummary.status === 200 && lastCollectionSummary.documentCount === expectedDocumentCount) {
      break;
    }

    if (attempt < collectionSummaryPollAttempts - 1) {
      sleep(collectionSummaryPollIntervalSeconds);
    }
  }

  const importedDocumentCount = lastCollectionSummary.documentCount;
  const duration = new Date().getTime() - benchmarkStart;
  importDuration.add(importOnlyDuration);

  const benchmarkSummary = {
    importedDocumentCount,
    importStatus: res.status,
    responseContractWarnings,
    summaryError: lastCollectionSummary.error,
    summarySnippet: lastCollectionSummary.bodySnippet,
    summaryStatus: lastCollectionSummary.status,
  };

  const checksPassed = check(benchmarkSummary, {
    "status is 200": (summary) => acceptedImportStatuses.has(summary.importStatus),
    "operation successful": (summary) =>
      summary.summaryStatus === 200 &&
      summary.importedDocumentCount === expectedDocumentCount,
  });

  if (!checksPassed) {
    console.error(
      [
        `Index benchmark import failed for ${url}`,
        `status=${res.status}`,
        `import_duration_ms=${importOnlyDuration}`,
        `total_duration_ms=${duration}`,
        `expected_docs=${expectedDocumentCount}`,
        `imported_docs=${importedDocumentCount}`,
        `collection_summary_status=${lastCollectionSummary.status}`,
        `collection_summary_error=${lastCollectionSummary.error}`,
        `collection_summary_snippet=${lastCollectionSummary.bodySnippet}`,
        `response_contract_warnings=${responseContractWarnings}`,
        `response_line_count=${importSummary.lineCount}`,
        `response_success_count=${importSummary.successCount}`,
        `response_error_count=${importSummary.errorCount}`,
        `response_snippet=${importSummary.snippet}`,
      ].join(" "),
    );
  }

  console.log(
    [
      "Index benchmark summary",
      `import_duration_ms=${importOnlyDuration}`,
      `total_duration_ms=${duration}`,
      `expected_docs=${expectedDocumentCount}`,
      `imported_docs=${importedDocumentCount}`,
      `status=${res.status}`,
      `response_contract_warnings=${responseContractWarnings}`,
    ].join(" "),
  );
}

export function handleSummary(data: { metrics?: Record<string, { values?: Record<string, number>; passes?: number; fails?: number }> }) {
  const importDurationMs = data.metrics?.import_duration?.values?.avg ?? -1;
  const passedChecks = data.metrics?.checks?.passes ?? 0;
  const failedChecks = data.metrics?.checks?.fails ?? 0;

  return {
    stderr: `${indexSummaryPrefix}${JSON.stringify({
      checksPassed: passedChecks,
      checksTotal: passedChecks + failedChecks,
      importDurationMs: Math.round(importDurationMs),
    })}\n`,
  };
}
