#!/usr/bin/env bun

import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { isAbsolute, join, resolve } from "node:path";
import { Filters } from "./constants";
import { TypesenseTestRunner } from "./index";

type CliOptions = {
  filters: Filters[];
  testFile: string | null;
  downloadMigrationBinary: boolean;
  migrationVersion: string;
  migrationTarget: string;
  migrationOutputDir: string | null;
  singleNodeOnly: boolean;
};

function printHelp() {
  console.log(`Usage: typesense-api-tests [options] [path/to/file.test.ts]

Options:
  --no-secrets                         Exclude tests tagged with ${Filters.SECRETS}
  --single-node-only                   Run only single-node phases
  --download-migration-binary          Download and use migration source binary
  --migration-version <version>        Migration binary version (default: 29.0)
  --migration-target <target>          Migration binary target (default: linux-amd64)
  --migration-output-dir <path>        Directory to store downloaded migration binary
  -h, --help                           Show this help
`);
}

function getOptionValue(args: string[], index: number, flagName: string) {
  if (index + 1 >= args.length) {
    throw new Error(`Missing value for ${flagName}`);
  }

  return args[index + 1]!;
}

function parseArgs(args: string[]): CliOptions {
  let noSecrets = false;
  let testFile: string | null = null;
  let downloadMigrationBinary = false;
  let migrationVersion = "29.0";
  let migrationTarget = "linux-amd64";
  let migrationOutputDir: string | null = null;
  let singleNodeOnly = false;

  for (let i = 0; i < args.length; i += 1) {
    const arg = args[i]!;

    if (arg === "-h" || arg === "--help") {
      printHelp();
      process.exit(0);
    }

    if (arg === "--no-secrets") {
      noSecrets = true;
      continue;
    }

    if (arg === "--single-node-only") {
      singleNodeOnly = true;
      continue;
    }

    if (arg === "--download-migration-binary") {
      downloadMigrationBinary = true;
      continue;
    }

    if (arg === "--migration-version") {
      migrationVersion = getOptionValue(args, i, arg);
      i += 1;
      continue;
    }

    if (arg.startsWith("--migration-version=")) {
      migrationVersion = arg.slice("--migration-version=".length);
      continue;
    }

    if (arg === "--migration-target") {
      migrationTarget = getOptionValue(args, i, arg);
      i += 1;
      continue;
    }

    if (arg.startsWith("--migration-target=")) {
      migrationTarget = arg.slice("--migration-target=".length);
      continue;
    }

    if (arg === "--migration-output-dir") {
      migrationOutputDir = getOptionValue(args, i, arg);
      i += 1;
      continue;
    }

    if (arg.startsWith("--migration-output-dir=")) {
      migrationOutputDir = arg.slice("--migration-output-dir=".length);
      continue;
    }

    if (!arg.startsWith("--") && arg.endsWith(".test.ts") && testFile === null) {
      testFile = arg;
    }
  }

  return {
    filters: noSecrets ? [Filters.SECRETS] : [],
    testFile,
    downloadMigrationBinary,
    migrationVersion,
    migrationTarget,
    migrationOutputDir,
    singleNodeOnly,
  };
}

function getApiTestsRoot() {
  return resolve(fileURLToPath(new URL("..", import.meta.url)));
}

async function maybeDownloadMigrationBinary(options: CliOptions) {
  if (!options.downloadMigrationBinary) {
    return;
  }

  const apiTestsRoot = getApiTestsRoot();
  const majorVersion = options.migrationVersion.split(".")[0] ?? options.migrationVersion;
  const defaultOutputDir = join(apiTestsRoot, "artifacts", `v${majorVersion}`);
  const resolvedOutputDir = options.migrationOutputDir
    ? (isAbsolute(options.migrationOutputDir)
      ? options.migrationOutputDir
      : resolve(process.cwd(), options.migrationOutputDir))
    : defaultOutputDir;

  const scriptPath = join(apiTestsRoot, "scripts", "download_migration_binary.sh");
  if (!existsSync(scriptPath)) {
    throw new Error(`Migration download script not found: ${scriptPath}`);
  }

  const downloadProc = Bun.spawnSync({
    cmd: ["bash", scriptPath, options.migrationVersion, options.migrationTarget, resolvedOutputDir],
    stdout: "inherit",
    stderr: "inherit",
  });

  if (downloadProc.exitCode !== 0) {
    throw new Error(`Failed to download migration binary (exit ${downloadProc.exitCode})`);
  }

  const migrationBinaryPath = join(resolvedOutputDir, "typesense-server");
  if (!existsSync(migrationBinaryPath)) {
    throw new Error(`Migration binary not found after download: ${migrationBinaryPath}`);
  }

  process.env.TYPESENSE_MIGRATION_SOURCE_BINARY_PATH = migrationBinaryPath;
  console.log(`Using migration source binary: ${migrationBinaryPath}`);
}

async function main() {
  const args = process.argv.slice(2);
  const options = parseArgs(args);

  await maybeDownloadMigrationBinary(options);

  const runner = TypesenseTestRunner.getInstance();
  await runner.run(options.filters, options.testFile, { singleNodeOnly: options.singleNodeOnly });
}

main();
