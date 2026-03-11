#!/usr/bin/env bun

import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import { Filters } from "./constants";
import { TypesenseTestRunner } from "./index";

type CliOptions = {
  filters: Filters[];
  testFile: string | null;
  singleNodeOnly: boolean;
};

function printHelp() {
  console.log(`Usage: typesense-api-tests [options] [path/to/file.test.ts]

Options:
  --no-secrets                         Exclude tests tagged with ${Filters.SECRETS}
  --single-node-only                   Run only single-node phases
  -h, --help                           Show this help
`);
}

function parseArgs(args: string[]): CliOptions {
  let noSecrets = false;
  let testFile: string | null = null;
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

    if (!arg.startsWith("--") && arg.endsWith(".test.ts") && testFile === null) {
      testFile = arg;
    }
  }

  return {
    filters: noSecrets ? [Filters.SECRETS] : [],
    testFile,
    singleNodeOnly,
  };
}

async function main() {
  const args = process.argv.slice(2);
  const options = parseArgs(args);

  const runner = TypesenseTestRunner.getInstance();
  await runner.run(options.filters, options.testFile, { singleNodeOnly: options.singleNodeOnly });
}

main();
