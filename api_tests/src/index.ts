import { TypesenseProcessManager } from "./manager";
import { Phases, Filters } from "./constants";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { isAbsolute, resolve } from "node:path";

type RunOptions = {
  singleNodeOnly?: boolean;
};

export class TypesenseTestRunner {
  private manager: TypesenseProcessManager;
  private static instance: TypesenseTestRunner;
  private exit_code: number = 0;
  private testFileContentCache: Map<string, string> = new Map();
  private readonly apiTestsRoot = resolve(fileURLToPath(new URL("..", import.meta.url)));
  private runOptions: RunOptions = {};

  constructor() {
    this.manager = new TypesenseProcessManager();
  }

  static getInstance() {
    if (!TypesenseTestRunner.instance) {
      TypesenseTestRunner.instance = new TypesenseTestRunner();
    }
    return TypesenseTestRunner.instance;
  }

  getTestNamePattern(phase: Phases, filters: Filters[]) {
    if (filters.includes(Filters.SECRETS)) {
      return `^(?=.*${phase})(?!.*${Filters.SECRETS}).*$`;
    }
    return phase;
  }

  private shouldAllowPhase(phase: Phases): boolean {
    if (this.runOptions.singleNodeOnly === true) {
      return phase !== Phases.MULTI_FRESH &&
        phase !== Phases.MULTI_RESTARTED &&
        phase !== Phases.MULTI_SNAPSHOT;
    }

    return true;
  }

  private async shouldRunPhase(phase: Phases, testFile: string | null): Promise<boolean> {
    if (!this.shouldAllowPhase(phase)) {
      return false;
    }

    if (!testFile) {
      return true;
    }

    let content = this.testFileContentCache.get(testFile);
    if (!content) {
      content = await Bun.file(testFile).text();
      this.testFileContentCache.set(testFile, content);
    }

    const enumTokenByPhase: Record<Phases, string> = {
      [Phases.SINGLE_FRESH]: "Phases.SINGLE_FRESH",
      [Phases.SINGLE_RESTARTED]: "Phases.SINGLE_RESTARTED",
      [Phases.SINGLE_SNAPSHOT]: "Phases.SINGLE_SNAPSHOT",
      [Phases.MULTI_FRESH]: "Phases.MULTI_FRESH",
      [Phases.MULTI_RESTARTED]: "Phases.MULTI_RESTARTED",
      [Phases.MULTI_SNAPSHOT]: "Phases.MULTI_SNAPSHOT",
      [Phases.NO_PHASE]: "Phases.NO_PHASE",
    };

    return (
      content.includes(enumTokenByPhase[phase])
      || content.includes(`"${phase}"`)
      || content.includes(`'${phase}'`)
    );
  }

  async run(filters: Filters[], testFile: string | null = null, runOptions: RunOptions = {}) {
    this.runOptions = runOptions;
    const resolvedTestFile = this.resolveTestFilePath(testFile);

    try {
      this.manager.cleanDataDirs();
      await this.singleServerTests(filters, resolvedTestFile);
      await this.multiServerTests(filters, resolvedTestFile);
      await this.noPhase(filters, resolvedTestFile);
      await this.manager.shutdown();
      process.exit(this.exit_code);
    } catch (err) {
      await this.manager.shutdown();
      throw err;
    }
  }

  async singleServerTests(filters: Filters[], testFile: string | null = null) {
    try {
      await this.singleFresh(filters, testFile);
    } catch (err) {
      console.error(err);
      this.printReplayHint(filters, testFile, Phases.SINGLE_FRESH);
      this.exit_code = 1;
      return;
    }

    try {
      await this.singleRestarted(filters, testFile);
    } catch (err) {
      console.error(err);
      this.printReplayHint(filters, testFile, Phases.SINGLE_RESTARTED);
      this.exit_code = 1;
      return;
    }

    try {
      await this.singleSnapshot(filters, testFile);
    } catch (err) {
      console.error(err);
      this.printReplayHint(filters, testFile, Phases.SINGLE_SNAPSHOT);
      this.exit_code = 1;
    }
  }

  async multiServerTests(filters: Filters[], testFile: string | null = null) {
    try {
      await this.multiFresh(filters, testFile);
    } catch (err) {
      console.error(err);
      this.printReplayHint(filters, testFile, Phases.MULTI_FRESH);
      this.exit_code = 1;
      return;
    }

    try {
      await this.multiRestarted(filters, testFile);
    } catch (err) {
      console.error(err);
      this.printReplayHint(filters, testFile, Phases.MULTI_RESTARTED);
      this.exit_code = 1;
      return;
    }

    try {
      await this.multiSnapshot(filters, testFile);
    } catch (err) {
      console.error(err);
      this.printReplayHint(filters, testFile, Phases.MULTI_SNAPSHOT);
      this.exit_code = 1;
    }
  }

  private buildTestCommand(pattern: string, testFile: string | null): string[] {
    const cmd = [
      "bun",
      "test",
      "--test-name-pattern",
      pattern,
      "--timeout",
      "100000",
      "--pass-with-no-tests",
    ];

    if (testFile) {
      cmd.push(testFile);
    } else {
      cmd.push("tests");
    }

    return cmd;
  }

  private resolveTestFilePath(testFile: string | null): string | null {
    if (testFile === null) {
      return null;
    }

    if (isAbsolute(testFile)) {
      return testFile;
    }

    const apiTestsResolved = resolve(this.apiTestsRoot, testFile);
    if (existsSync(apiTestsResolved)) {
      return apiTestsResolved;
    }

    const cwdResolved = resolve(process.cwd(), testFile);
    if (existsSync(cwdResolved)) {
      return cwdResolved;
    }

    return cwdResolved;
  }

  private buildTestEnv(): Record<string, string> {
    const inheritedEnv: Record<string, string> = {};
    for (const [key, value] of Object.entries(process.env)) {
      if (value !== undefined) {
        inheritedEnv[key] = value;
      }
    }

    return {
      ...inheritedEnv,
      ...this.manager.getRuntimeEnv(),
    };
  }

  private runPhaseTests(pattern: string, testFile: string | null) {
    return Bun.spawnSync({
      cmd: this.buildTestCommand(pattern, testFile),
      stderr: "inherit",
      stdout: "inherit",
      env: this.buildTestEnv(),
      cwd: this.apiTestsRoot,
    });
  }

  private printReplayHint(filters: Filters[], testFile: string | null, phase: Phases) {
    const replayArgs: string[] = [];
    if (filters.includes(Filters.SECRETS)) {
      replayArgs.push("--no-secrets");
    }
    if (this.runOptions.singleNodeOnly === true) {
      replayArgs.push("--single-node-only");
    }
    if (testFile) {
      replayArgs.push(testFile);
    }

    const envParts = [
      `TYPESENSE_BINARY_PATH=\"${this.manager.binaryPath}\"`,
      `TYPESENSE_DATA_DIR=\"${this.manager.baseDir}\"`,
    ];

    if (process.env.LD_LIBRARY_PATH) {
      envParts.push(`LD_LIBRARY_PATH=\"${process.env.LD_LIBRARY_PATH}\"`);
    }

    const replayCommand = `cd api_tests && ${envParts.join(" ")} bun src/cli.ts ${replayArgs.join(" ")}`.trim();
    console.error(`Replay hint for phase '${phase}': ${replayCommand}`);
    console.error(`API sandbox for this run: ${this.manager.baseDir}`);
  }

  async noPhase(filters: Filters[], testFile: string | null = null) {
    if (!(await this.shouldRunPhase(Phases.NO_PHASE, testFile))) {
      return;
    }

    // Migration port allocation removed — NuRaft server cannot load braft-era state.
    console.log(`\n=== ⭐ Running phase: ${Phases.NO_PHASE} ===\n`);
    const pattern = this.getTestNamePattern(Phases.NO_PHASE, filters);
    const proc = this.runPhaseTests(pattern, testFile);
    if (proc.exitCode !== 0) {
      console.error(`\n=== ❌ Phase ${Phases.NO_PHASE} failed ===\n`);
      this.printReplayHint(filters, testFile, Phases.NO_PHASE);
      this.exit_code = 1;
    }
  }

  async singleFresh(filters: Filters[], testFile: string | null = null) {
    if (!(await this.shouldRunPhase(Phases.SINGLE_FRESH, testFile))) {
      return;
    }

    await this.manager.startSingleNode();
    console.log(`\n=== ⭐ Running phase: ${Phases.SINGLE_FRESH} ===\n`);
    const pattern = this.getTestNamePattern(Phases.SINGLE_FRESH, filters);
    const proc = this.runPhaseTests(pattern, testFile);
    if (proc.exitCode !== 0) {
      console.error(`\n=== ❌ Phase ${Phases.SINGLE_FRESH} failed ===\n`);
      this.printReplayHint(filters, testFile, Phases.SINGLE_FRESH);
      this.exit_code = 1;
    }
  }

  async singleRestarted(filters: Filters[], testFile: string | null = null) {
    if (!(await this.shouldRunPhase(Phases.SINGLE_RESTARTED, testFile))) {
      return;
    }

    await this.manager.restartSingleNode();
    console.log(`\n=== ⭐ Running phase: ${Phases.SINGLE_RESTARTED} ===\n`);
    const pattern = this.getTestNamePattern(Phases.SINGLE_RESTARTED, filters);
    const proc = this.runPhaseTests(pattern, testFile);
    if (proc.exitCode !== 0) {
      console.error(`\n=== ❌ Phase ${Phases.SINGLE_RESTARTED} failed ===\n`);
      this.printReplayHint(filters, testFile, Phases.SINGLE_RESTARTED);
      this.exit_code = 1;
    }
  }

  async singleSnapshot(filters: Filters[], testFile: string | null = null) {
    if (!(await this.shouldRunPhase(Phases.SINGLE_SNAPSHOT, testFile))) {
      return;
    }

    const singleNodePort = this.manager.getSingleNodeApiPort();
    if (singleNodePort === null) {
      throw new Error("Single-node snapshot requested before single node started");
    }

    await this.manager.createSnapshot(singleNodePort);
    await this.manager.restartSingleNode();
    console.log(`\n=== ⭐ Running phase: ${Phases.SINGLE_SNAPSHOT} ===\n`);
    const pattern = this.getTestNamePattern(Phases.SINGLE_SNAPSHOT, filters);
    const proc = this.runPhaseTests(pattern, testFile);
    if (proc.exitCode !== 0) {
      console.error(`\n=== ❌ Phase ${Phases.SINGLE_SNAPSHOT} failed ===\n`);
      this.printReplayHint(filters, testFile, Phases.SINGLE_SNAPSHOT);
      this.exit_code = 1;
    }
  }

  async multiFresh(filters: Filters[], testFile: string | null = null) {
    if (!(await this.shouldRunPhase(Phases.MULTI_FRESH, testFile))) {
      return;
    }

    await this.manager.startMultiNode();
    console.log(`\n=== ⭐ Running phase: ${Phases.MULTI_FRESH} ===\n`);
    const pattern = this.getTestNamePattern(Phases.MULTI_FRESH, filters);
    const proc = this.runPhaseTests(pattern, testFile);
    if (proc.exitCode !== 0) {
      console.error(`\n=== ❌ Phase ${Phases.MULTI_FRESH} failed ===\n`);
      this.printReplayHint(filters, testFile, Phases.MULTI_FRESH);
      this.exit_code = 1;
    }
  }

  async multiRestarted(filters: Filters[], testFile: string | null = null) {
    if (!(await this.shouldRunPhase(Phases.MULTI_RESTARTED, testFile))) {
      return;
    }

    await this.manager.restartMultiNode();
    console.log(`\n=== ⭐ Running phase: ${Phases.MULTI_RESTARTED} ===\n`);
    const pattern = this.getTestNamePattern(Phases.MULTI_RESTARTED, filters);
    const proc = this.runPhaseTests(pattern, testFile);
    if (proc.exitCode !== 0) {
      console.error(`\n=== ❌ Phase ${Phases.MULTI_RESTARTED} failed ===\n`);
      this.printReplayHint(filters, testFile, Phases.MULTI_RESTARTED);
      this.exit_code = 1;
    }
  }

  async multiSnapshot(filters: Filters[], testFile: string | null = null) {
    if (!(await this.shouldRunPhase(Phases.MULTI_SNAPSHOT, testFile))) {
      return;
    }

    const leaderPort = this.manager.getMultiNodeLeaderApiPort();
    if (leaderPort === null) {
      throw new Error("Multi-node snapshot requested before multi node started");
    }

    await this.manager.createSnapshot(leaderPort);
    await this.manager.restartMultiNode();
    console.log(`\n=== ⭐ Running phase: ${Phases.MULTI_SNAPSHOT} ===\n`);
    const pattern = this.getTestNamePattern(Phases.MULTI_SNAPSHOT, filters);
    const proc = this.runPhaseTests(pattern, testFile);
    if (proc.exitCode !== 0) {
      console.error(`\n=== ❌ Phase ${Phases.MULTI_SNAPSHOT} failed ===\n`);
      this.printReplayHint(filters, testFile, Phases.MULTI_SNAPSHOT);
      this.exit_code = 1;
    }
  }
}
