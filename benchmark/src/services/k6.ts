import path from "path";
import type { TypesenseProcessManager } from "@/services/typesense-process";
import type { ErrorWithMessage } from "@/utils/error";
import type { IDockerComposeResult } from "docker-compose";
import type { Ora } from "ora";
import type { CollectionCreateSchema } from "typesense/lib/Typesense/Collections";

import { execSync } from "child_process";
import { run } from "docker-compose";
import { errAsync, okAsync, ResultAsync } from "neverthrow";

import { toErrorWithMessage } from "@/utils/error";
import { logger, LogLevel } from "@/utils/logger";
import { findRoot } from "@/utils/package-info";

interface BaseK6Env {
  API_KEY: string;
  PORT: number;
  COLLECTION_NAME: string;
  HOST: string;
  COMMIT_HASH: string;
}

export interface IndexK6Env extends BaseK6Env {
  BATCH_SIZE: number;
  INDEX_CHUNK_SIZE?: number;
}

export interface SearchK6Env extends BaseK6Env {
  DURATION: string;
}

export type K6Env = IndexK6Env & SearchK6Env;

interface LoadTestConfig {
  batchSize: number;
  duration: string;
  apiKey: string;
  port: number;
  commitHash: string;
  typesenseProcessManager: TypesenseProcessManager;
  spinner: Ora;
}

interface K6ExecutionResult {
  output: string;
}

export interface IndexBenchmarkExecutionResult {
  importDurationMs: number;
}

export class K6Benchmarks {
  private readonly config: LoadTestConfig;
  private readonly isInCi: boolean;
  public static readonly COLLECTION_NAME = "songs";
  public static readonly DATASET_URL = "https://dl.typesense.org/datasets/musicbrainz-1M-songs.jsonl.tar.gz";
  public static readonly COLLECTION_SCHEMA: CollectionCreateSchema = {
    name: K6Benchmarks.COLLECTION_NAME,
    fields: [
      { name: "album_name", type: "string" },
      { name: "country", type: "string", facet: true },
      { name: "genres", type: "string[]", facet: true },
      { name: "primary_artist_name", type: "string", facet: true },
      { name: "release_date", type: "int64" },
      { name: "release_decade", type: "string", facet: true },
      { name: "release_group_types", type: "string[]", facet: true },
      { name: "title", type: "string" },
      { name: "track_id", type: "string" },
      { name: "urls", type: "object[]", optional: true },
    ],
    enable_nested_fields: true,
  };
  public static readonly REQUIRED_SERVICES = ["grafana", "influxdb"];

  constructor(config: LoadTestConfig) {
    this.config = config;
    this.isInCi = Boolean(process.env.CI) || false;
  }

  public performSearchBenchmark(): ResultAsync<void, ErrorWithMessage> {
    return this.getSearchBenchmarkPath()
      .andThen((path) => this.executeK6Benchmark({ name: "search", scriptPath: path }))
      .map(() => {
        this.config.spinner.succeed("Search benchmark complete");
      });
  }

  public performIndexingBenchmark(): ResultAsync<IndexBenchmarkExecutionResult, ErrorWithMessage> {
    const indexChunkSize = this.isInCi ? 500 : 5000;
    return this.getIndexingBenchmarkPath().andThen((path) => {
      return this.createBenchmarkCollection()
        .andThen(() =>
          this.executeK6Benchmark({
            name: `indexing-${indexChunkSize}chunk`,
            scriptPath: path,
            additionalVars: {
              INDEX_CHUNK_SIZE: indexChunkSize,
            },
          }),
        )
        .andThen((result) => this.extractIndexBenchmarkExecutionResult(result.output))
        .map((result) => {
          this.config.spinner.succeed("Indexing benchmark complete");
          return result;
        });
    });
  }

  public performStressImportBenchmark(options?: {
    vus?: number;
    chunkSize?: number;
    duration?: string;
    serverBatchSize?: number;
  }): ResultAsync<void, ErrorWithMessage> {
    return this.getStressImportBenchmarkPath().andThen((scriptPath) => {
      return this.recreateBenchmarkCollection()
        .andThen(() =>
          this.executeK6Benchmark({
            name: `stress-import-${options?.vus ?? 4}vu-${options?.chunkSize ?? 5000}chunk`,
            scriptPath,
            additionalVars: {
              STRESS_VUS: options?.vus ?? 4,
              STRESS_CHUNK_SIZE: options?.chunkSize ?? 5000,
              STRESS_DURATION: options?.duration ?? "60s",
              STRESS_BATCH_SIZE: options?.serverBatchSize ?? 40,
            },
          }),
        )
        .map(() => {
          this.config.spinner.succeed("Stress import benchmark complete");
        });
    });
  }

  public performConcurrentBenchmark(options?: {
    searchVus?: number;
    importVus?: number;
    chunkSize?: number;
    duration?: string;
    serverBatchSize?: number;
  }): ResultAsync<void, ErrorWithMessage> {
    return this.getConcurrentBenchmarkPath().andThen((scriptPath) => {
      return this.recreateBenchmarkCollection()
        .andThen(() =>
          this.executeK6Benchmark({
            name: `concurrent-${options?.searchVus ?? 50}search-${options?.importVus ?? 4}import`,
            scriptPath,
            additionalVars: {
              CONCURRENT_SEARCH_VUS: options?.searchVus ?? 50,
              CONCURRENT_IMPORT_VUS: options?.importVus ?? 4,
              CONCURRENT_CHUNK_SIZE: options?.chunkSize ?? 1000,
              CONCURRENT_DURATION: options?.duration ?? "30s",
              CONCURRENT_BATCH_SIZE: options?.serverBatchSize ?? 40,
            },
          }),
        )
        .map(() => {
          this.config.spinner.succeed("Concurrent search+import benchmark complete");
        });
    });
  }

  public startMetricsCollection(duration: string): ResultAsync<void, ErrorWithMessage> {
    return this.getMetricsCollectorPath().andThen((scriptPath) => {
      return this.executeK6Benchmark({
        name: "metrics-collection",
        scriptPath,
        additionalVars: {
          METRICS_DURATION: duration,
          METRICS_INTERVAL: 2,
        },
      });
    });
  }

  /**
   * Start metrics collection in background (detached docker-compose container).
   * Returns the container ID so it can be stopped later.
   */
  public startMetricsCollectionInBackground(duration: string): ResultAsync<string, ErrorWithMessage> {
    return this.getMetricsCollectorPath().andThen((scriptPath) => {
      const envVarString = this.buildK6EnvironmentVars({
        METRICS_DURATION: duration,
        METRICS_INTERVAL: 2,
      });

      const command = `run ${envVarString} ${scriptPath}`;
      const cwd = findRoot(process.cwd());

      logger.info("Starting metrics collector in background...");
      return ResultAsync.fromPromise(
        (async () => {
          // Use docker compose run -d to start detached
          const result = execSync(
            `docker compose run -d --no-deps --remove-orphans k6 ${command}`,
            { cwd, encoding: "utf-8" },
          ).trim();
          // result is the container ID
          logger.info(`Metrics collector started: ${result.slice(0, 12)}`);
          return result;
        })(),
        toErrorWithMessage,
      );
    });
  }

  /**
   * Stop a detached metrics-collector container.
   */
  public stopMetricsCollection(containerId: string): ResultAsync<void, ErrorWithMessage> {
    logger.info(`Stopping metrics collector ${containerId.slice(0, 12)}...`);
    return ResultAsync.fromPromise(
      (async () => {
        try {
          execSync(`docker stop ${containerId}`, { encoding: "utf-8", timeout: 15000 });
        } catch {
          // Container may have already exited (duration expired)
        }
        try {
          execSync(`docker rm -f ${containerId}`, { encoding: "utf-8", timeout: 10000 });
        } catch {
          // Already removed
        }
        logger.info("Metrics collector stopped.");
      })(),
      toErrorWithMessage,
    );
  }

  private executeK6Benchmark(options: {
    scriptPath: string;
    name: string;
    additionalVars?: Record<string, unknown>;
  }): ResultAsync<K6ExecutionResult, ErrorWithMessage> {
    const { scriptPath, name } = options;
    const envVarString = this.buildK6EnvironmentVars(options.additionalVars);
    this.config.spinner.start(`Running ${name} benchmark\n`);

    const command = [
      "run",
      envVarString,
      scriptPath,
      logger.getLevel() <= LogLevel.DEBUG ? "--quiet" : "",
    ].join(" ");

    const errors: string[] = [];
    const warnings: string[] = [];

    return ResultAsync.fromPromise(
      run("k6", command, {
        commandOptions: ["--no-deps", "--remove-orphans"],
        log: logger.getLevel() === LogLevel.DEBUG,
        cwd: findRoot(process.cwd()),
        callback: (chunk) => this.processLogChunk(chunk, errors, warnings),
      }),
      toErrorWithMessage,
    ).andThen((result) => this.handleK6Result(result, errors, warnings));
  }

  private processLogChunk(chunk: Buffer, errors: string[], warnings: string[]): void {
    const log = chunk.toString();
    if (log.includes("level=error")) {
      errors.push(log.trim());
    }
    if (log.includes("level=warn")) {
      warnings.push(log.trim());
    }
  }

  private handleK6Result(
    result: IDockerComposeResult,
    errors: string[],
    warnings: string[],
  ): ResultAsync<K6ExecutionResult, ErrorWithMessage> {
    const cleanOutput = result.out.trim();

    // Handle empty output (k6 crashed or container failed to start)
    if (!cleanOutput) {
      const errDetail = result.err?.trim();
      return errAsync({
        message: `k6 produced no output (exit code: ${result.exitCode ?? "unknown"})${errDetail ? `\nDocker output: ${errDetail.slice(0, 500)}` : ""}`,
      });
    }

    // Try to find the checks pass rate from either format
    const checksPassRate = this.extractChecksPassRate(cleanOutput);

    this.config.spinner.stop();

    if (errors.length > 0) {
      logger.error(`Errors: \n\n${errors.join("\n")}`);
    }
    if (warnings.length > 0) {
      logger.warn(`Warnings: \n\n${warnings.join("\n")}`);
    }

    // No checks line found — benchmark may use a different output format (e.g. concurrent with only Trends/Counters)
    if (checksPassRate === null) {
      logger.warn("No checks line found in k6 output — assuming benchmark completed (custom metrics only)");
      this.config.spinner.succeed("Benchmark complete");
      return okAsync({ output: cleanOutput });
    }

    logger.info(`Checks pass rate: ${checksPassRate}%`);

    if (checksPassRate < 97) {
      return errAsync({
        message: `k6 tests failed - ${checksPassRate}% checks passed`,
      });
    }

    this.config.spinner.succeed("Benchmark complete");
    return okAsync({ output: cleanOutput });
  }

  private extractIndexBenchmarkExecutionResult(
    output: string,
  ): ResultAsync<IndexBenchmarkExecutionResult, ErrorWithMessage> {
    const summaryMatch = /Index benchmark summary .*?\bimport_duration_ms=(\d+)/.exec(output);
    if (!summaryMatch) {
      return errAsync({
        message: "Index benchmark completed but no import_duration_ms summary was found in k6 output",
      });
    }

    return okAsync({
      importDurationMs: Number.parseInt(summaryMatch[1], 10),
    });
  }

  private extractChecksPassRate(output: string): number | null {
    // Try new format: "checks_succeeded...................: 100.00% 2 out of 2"
    const newFormatLine = output.split("\n").find((line) => line.trim().startsWith("checks_succeeded"));
    if (newFormatLine) {
      const match = /([0-9.]+)%/.exec(newFormatLine);
      return match ? parseFloat(match[1]) : null;
    }

    // Try old format: "checks.........................: 100.00% ..."
    const oldFormatLine = output.split("\n").find((line) => line.trim().startsWith("checks"));
    if (oldFormatLine) {
      const match = /([0-9.]+)%/.exec(oldFormatLine);
      return match ? parseFloat(match[1]) : null;
    }

    return null;
  }

  private buildK6EnvironmentVars(additionalVars?: Record<string, unknown>): string {
    const envVarMap = {
      API_KEY: this.config.apiKey,
      DURATION: this.config.duration,
      BATCH_SIZE: this.config.batchSize,
      COLLECTION_NAME: K6Benchmarks.COLLECTION_NAME,
      PORT: this.config.port,
      HOST: `typesense-bench-${this.config.port}`,
      COMMIT_HASH: this.config.commitHash,
      ...additionalVars,
    };

    return Object.entries(envVarMap)
      .map(([key, value]) => `-e ${key}=${value}`)
      .join(" ");
  }

  private createBenchmarkCollection(): ResultAsync<void, ErrorWithMessage> {
    this.config.spinner.start("Creating benchmark collection");

    const process = this.config.typesenseProcessManager.getProcessByHttpPort(this.config.port);
    if (!process.isOk()) {
      return errAsync(process.error);
    }

    return ResultAsync.fromPromise(
      process.value.client.collections().create(K6Benchmarks.COLLECTION_SCHEMA),
      toErrorWithMessage,
    ).map(() => {
      this.config.spinner.succeed("Benchmark collection created");
    });
  }

  private recreateBenchmarkCollection(): ResultAsync<void, ErrorWithMessage> {
    this.config.spinner.start("Recreating benchmark collection");

    const process = this.config.typesenseProcessManager.getProcessByHttpPort(this.config.port);
    if (!process.isOk()) {
      return errAsync(process.error);
    }

    const client = process.value.client;
    return ResultAsync.fromPromise(
      client.collections(K6Benchmarks.COLLECTION_NAME).delete().catch(() => {
        // Collection may not exist, that's fine
      }),
      toErrorWithMessage,
    )
      .andThen(() =>
        ResultAsync.fromPromise(
          client.collections().create(K6Benchmarks.COLLECTION_SCHEMA),
          toErrorWithMessage,
        ),
      )
      .map(() => {
        this.config.spinner.succeed("Benchmark collection recreated");
      });
  }

  private getSearchBenchmarkPath(): ResultAsync<string, ErrorWithMessage> {
    return okAsync(path.join("/app", "src", "benchmarks", "search.ts"));
  }

  private getIndexingBenchmarkPath(): ResultAsync<string, ErrorWithMessage> {
    return okAsync(path.join("/app", "src", "benchmarks", "index.ts"));
  }

  private getStressImportBenchmarkPath(): ResultAsync<string, ErrorWithMessage> {
    return okAsync(path.join("/app", "src", "benchmarks", "stress-import.ts"));
  }

  private getConcurrentBenchmarkPath(): ResultAsync<string, ErrorWithMessage> {
    return okAsync(path.join("/app", "src", "benchmarks", "concurrent-search-import.ts"));
  }

  private getMetricsCollectorPath(): ResultAsync<string, ErrorWithMessage> {
    return okAsync(path.join("/app", "src", "benchmarks", "metrics-collector.ts"));
  }
}
