import { createServer } from "node:net";
import { appendFileSync, rmSync, mkdirSync, existsSync, writeFileSync } from "node:fs";
import { join } from "node:path";

type ServerInstance = {
  process: Bun.Subprocess;
  name: string;
  port: number;
};

type ProcessOutputTail = {
  stdout: string;
  stderr: string;
};

type MultiNodeConfig = {
  name: string;
  port: number;
  peerPort: number;
  dataDir: string;
  logDir: string;
  analyticsDir: string;
};

type SingleNodeState = {
  name: string;
  port: number;
  peeringPort: number;
  dataDir: string;
};

type ManagerOptions = {
  isolateBaseDir?: boolean;
};

const DEFAULT_SINGLE_API_PORT = 8108;
const DEFAULT_SINGLE_PEERING_PORT = 8107;
const DEFAULT_MULTI_API_PORTS = [5108, 6108, 7108];
const DEFAULT_MULTI_PEERING_PORTS = [5107, 6107, 7107];
const DEFAULT_HEALTH_TIMEOUT_MS = 60_000;
const SHUTDOWN_GRACE_MS = 5_000;
const PROCESS_OUTPUT_TAIL_MAX_CHARS = 8_000;

function parsePort(value: string | undefined): number | null {
  if (!value) return null;
  const parsed = Number.parseInt(value, 10);
  if (Number.isNaN(parsed) || parsed < 1 || parsed > 65535) return null;
  return parsed;
}

function parsePortList(value: string | undefined, expectedCount: number): number[] | null {
  if (!value) return null;
  const ports = value.split(",").map((entry) => parsePort(entry.trim()));
  if (ports.length !== expectedCount || ports.some((port) => port === null)) {
    return null;
  }
  return ports as number[];
}

export class TypesenseProcessManager {
  private static managers = new Set<TypesenseProcessManager>();
  private static signalHandlersInstalled = false;

  readonly baseDir: string;
  readonly binaryPath: string;
  readonly ipAddress: string;
  readonly nodesFile: string;
  readonly apiHost: string;
  readonly apiKey: string;
  readonly runId: string;

  processes: Map<string, ServerInstance> = new Map();
  private processExitCodes: Map<string, number> = new Map();
  private processOutputTails: Map<string, ProcessOutputTail> = new Map();

  private singleNodeState: SingleNodeState | null = null;
  private multiNodeConfigs: MultiNodeConfig[] | null = null;
  private configuredSingleApiPort: number | null;
  private configuredSinglePeeringPort: number | null;
  private configuredMultiApiPorts: number[] | null;
  private configuredMultiPeeringPorts: number[] | null;

  static additionalConfigs = [
    "--enable-cors",
    "--enable-search-analytics",
    "--analytics-flush-interval=3600",
    "--analytics-minute-rate-limit=1000",
  ];

  constructor(baseDir?: string, binaryPath?: string, options: ManagerOptions = {}) {
    const runIdFromEnv = process.env.TYPESENSE_TEST_RUN_ID;
    this.runId = runIdFromEnv && runIdFromEnv.length > 0
      ? runIdFromEnv
      : `run-${process.pid}-${Date.now()}-${Math.random().toString(16).slice(2, 8)}`;

    const configuredRootDir = baseDir
      ?? process.env.TYPESENSE_DATA_DIR
      ?? join(process.cwd(), "tmp", "test");

    const isolateBaseDir = options.isolateBaseDir ?? baseDir === undefined;
    this.baseDir = isolateBaseDir ? join(configuredRootDir, this.runId) : configuredRootDir;

    const resolvedBinaryPath = binaryPath ?? process.env.TYPESENSE_BINARY_PATH;
    if (!resolvedBinaryPath) {
      throw new Error("Typesense binary path is required (TYPESENSE_BINARY_PATH)");
    }
    this.binaryPath = resolvedBinaryPath;

    this.apiHost = process.env.TYPESENSE_API_HOST ?? "localhost";
    this.apiKey = process.env.TYPESENSE_API_KEY ?? "xyz";

    this.configuredSingleApiPort = parsePort(process.env.TYPESENSE_SINGLE_API_PORT);
    this.configuredSinglePeeringPort = parsePort(process.env.TYPESENSE_SINGLE_PEERING_PORT);
    this.configuredMultiApiPorts = parsePortList(process.env.TYPESENSE_MULTI_API_PORTS, 3);
    this.configuredMultiPeeringPorts = parsePortList(process.env.TYPESENSE_MULTI_PEERING_PORTS, 3);

    this.ipAddress = this.getIpAddress();
    this.nodesFile = join(this.baseDir, "nodes");

    mkdirSync(this.baseDir, { recursive: true });

    TypesenseProcessManager.managers.add(this);
    this.installSignalHandlers();
  }

  static async findFreePorts(count: number): Promise<number[]> {
    const ports = new Set<number>();
    while (ports.size < count) {
      ports.add(await TypesenseProcessManager.findFreePort());
    }
    return Array.from(ports);
  }

  getSingleNodeApiPort(): number | null {
    return this.singleNodeState?.port ?? this.configuredSingleApiPort;
  }

  getMultiNodeLeaderApiPort(): number | null {
    return this.multiNodeConfigs?.[0]?.port ?? this.configuredMultiApiPorts?.[0] ?? null;
  }

  getRuntimeEnv(): Record<string, string> {
    const runtimeEnv: Record<string, string> = {
      TYPESENSE_TEST_RUN_ID: this.runId,
      TYPESENSE_API_HOST: this.apiHost,
      TYPESENSE_API_KEY: this.apiKey,
      TYPESENSE_API_TEST_SANDBOX_DIR: this.baseDir,
    };

    const singlePort = this.getSingleNodeApiPort();
    if (singlePort !== null) {
      runtimeEnv.TYPESENSE_SINGLE_API_PORT = `${singlePort}`;
    }

    const singlePeerPort = this.singleNodeState?.peeringPort ?? this.configuredSinglePeeringPort;
    if (singlePeerPort !== null && singlePeerPort !== undefined) {
      runtimeEnv.TYPESENSE_SINGLE_PEERING_PORT = `${singlePeerPort}`;
    }

    const multiConfigs = this.multiNodeConfigs;
    if (multiConfigs && multiConfigs.length === 3) {
      runtimeEnv.TYPESENSE_MULTI_API_PORTS = multiConfigs.map((node) => node.port).join(",");
      runtimeEnv.TYPESENSE_MULTI_PEERING_PORTS = multiConfigs.map((node) => node.peerPort).join(",");
    } else if (this.configuredMultiApiPorts && this.configuredMultiPeeringPorts) {
      runtimeEnv.TYPESENSE_MULTI_API_PORTS = this.configuredMultiApiPorts.join(",");
      runtimeEnv.TYPESENSE_MULTI_PEERING_PORTS = this.configuredMultiPeeringPorts.join(",");
    }

    return runtimeEnv;
  }

  cleanDataDirs() {
    const dirs = [
      "typesense-data",
      "typesense-data/analytics_db",
      "typesense-data-1", "typesense-data-2", "typesense-data-3",
      "typesense-data-1/analytics_db", "typesense-data-2/analytics_db", "typesense-data-3/analytics_db",
      "logs/typesense",
      "logs/typesense-1", "logs/typesense-2", "logs/typesense-3",
      "logs/process",
      "snapshot/single-node",
      "snapshot/multi-node",
    ];

    mkdirSync(this.baseDir, { recursive: true });

    if (existsSync(this.nodesFile)) {
      rmSync(this.nodesFile, { force: true });
    }

    for (const dir of dirs) {
      const fullPath = join(this.baseDir, dir);
      if (existsSync(fullPath)) {
        rmSync(fullPath, { recursive: true, force: true });
      }
      mkdirSync(fullPath, { recursive: true });
    }
  }

  async startSingleNode(
    dataDir: string = "typesense-data",
    port?: number,
    peeringPort?: number,
    name: string = "single-node",
    healthTimeoutMs: number = DEFAULT_HEALTH_TIMEOUT_MS,
  ) {
    let resolvedPort = port;
    let resolvedPeeringPort = peeringPort;

    if (resolvedPort === undefined || resolvedPeeringPort === undefined) {
      const [defaultPort, defaultPeeringPort] = await this.resolveSinglePorts();
      resolvedPort = resolvedPort ?? defaultPort;
      resolvedPeeringPort = resolvedPeeringPort ?? defaultPeeringPort;
    }

    if (resolvedPort === undefined || resolvedPeeringPort === undefined) {
      throw new Error("Single-node port resolution failed");
    }

    if (resolvedPort === resolvedPeeringPort) {
      const freePorts = await TypesenseProcessManager.findFreePorts(2);
      resolvedPort = freePorts[0];
      resolvedPeeringPort = freePorts[1];
    }

    this.configuredSingleApiPort = resolvedPort;
    this.configuredSinglePeeringPort = resolvedPeeringPort;

    const resolvedDataDir = dataDir === "" ? this.baseDir : join(this.baseDir, dataDir);
    const analyticsDir = join(resolvedDataDir, "analytics_db");
    const logDir = join(this.baseDir, "logs", "typesense");

    mkdirSync(logDir, { recursive: true });
    mkdirSync(analyticsDir, { recursive: true });

    const args = [
      `--data-dir=${resolvedDataDir}`,
      `--api-key=${this.apiKey}`,
      `--api-port=${resolvedPort}`,
      "--api-address=0.0.0.0",
      `--log-dir=${logDir}`,
      `--analytics-dir=${analyticsDir}`,
      `--peering-address=${this.ipAddress}`,
      `--peering-port=${resolvedPeeringPort}`,
      ...TypesenseProcessManager.additionalConfigs,
    ];

    this.singleNodeState = {
      name,
      port: resolvedPort,
      peeringPort: resolvedPeeringPort,
      dataDir,
    };

    this.spawnServer(name, args, resolvedPort);
    await this.waitForHealth(resolvedPort, healthTimeoutMs);
  }

  async startMultiNode() {
    const configs = await this.resolveMultiNodeConfigs();

    const clusterStr = configs
      .map((node) => `${this.ipAddress}:${node.peerPort}:${node.port}`)
      .join(",");

    writeFileSync(this.nodesFile, clusterStr);

    for (const node of configs) {
      const args = [
        `--nodes=${this.nodesFile}`,
        `--peering-address=${this.ipAddress}`,
        `--data-dir=${join(this.baseDir, node.dataDir)}`,
        `--api-key=${this.apiKey}`,
        `--api-port=${node.port}`,
        "--api-address=0.0.0.0",
        `--peering-port=${node.peerPort}`,
        `--log-dir=${join(this.baseDir, "logs", node.logDir)}`,
        `--analytics-dir=${join(this.baseDir, node.analyticsDir)}`,
        ...TypesenseProcessManager.additionalConfigs,
      ];
      this.spawnServer(node.name, args, node.port);
    }

    for (const node of configs) {
      await this.waitForHealth(node.port);
    }

    await this.electLeader();
  }

  async stopServer(name: string) {
    const instance = this.processes.get(name);
    if (!instance) return;

    await this.terminateProcess(instance);
    this.processes.delete(name);
    this.processExitCodes.delete(name);
    this.processOutputTails.delete(name);
  }

  async restartSingleNode() {
    if (!this.singleNodeState) {
      throw new Error("Cannot restart single node before startSingleNode has run");
    }

    const { name, dataDir, port, peeringPort } = this.singleNodeState;
    await this.stopServer(name);
    await this.startSingleNode(dataDir, port, peeringPort, name);
  }

  async restartMultiNode() {
    if (!this.multiNodeConfigs) {
      throw new Error("Cannot restart multi node before startMultiNode has run");
    }

    for (const name of ["multi-node3", "multi-node2", "multi-node1"]) {
      await this.stopServer(name);
    }
    await this.startMultiNode();
  }

  async electLeader() {
    const leaderPort = this.getMultiNodeLeaderApiPort();
    if (leaderPort === null) {
      throw new Error("Cannot elect leader before multi-node ports are resolved");
    }

    const res = await fetch(`${this.apiBaseUrl(leaderPort)}/operations/vote`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-TYPESENSE-API-KEY": this.apiKey,
      },
    });

    if (!res.ok) {
      const body = await res.text();
      throw new Error(`Elect leader failed: ${res.status} ${res.statusText}. ${body}`);
    }
  }

  async createSnapshot(port: number, snapshotPath: string = "") {
    const singleNodePort = this.getSingleNodeApiPort();
    const mode = singleNodePort !== null && port === singleNodePort ? "single-node" : "multi-node";
    const resolvedSnapshotPath = snapshotPath || join(this.baseDir, "snapshot", mode);
    const encodedSnapshotPath = encodeURIComponent(resolvedSnapshotPath);
    const url = `${this.apiBaseUrl(port)}/operations/snapshot?snapshot_path=${encodedSnapshotPath}`;

    const res = await fetch(url, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-TYPESENSE-API-KEY": this.apiKey,
      },
    });

    if (!res.ok) {
      const body = await res.text();
      throw new Error(`Snapshot failed: ${res.status} ${res.statusText}. ${body}`);
    }
  }

  async shutdown() {
    const names = Array.from(this.processes.keys());
    for (const name of names) {
      await this.stopServer(name);
    }
    TypesenseProcessManager.managers.delete(this);
  }

  private spawnServer(name: string, args: string[], port: number) {
    const proc = Bun.spawn([this.binaryPath, ...args], {
      stdout: "pipe",
      stderr: "pipe",
      env: process.env,
      killSignal: "SIGINT",
    });

    this.processExitCodes.delete(name);
    this.processOutputTails.set(name, { stdout: "", stderr: "" });

    void this.captureProcessOutput(name, "stdout", proc.stdout);
    void this.captureProcessOutput(name, "stderr", proc.stderr);

    void proc.exited.then((code) => {
      this.processExitCodes.set(name, code);
    });

    this.processes.set(name, { process: proc, name, port });
  }

  private async waitForHealth(port: number, timeout = DEFAULT_HEALTH_TIMEOUT_MS) {
    const start = Date.now();
    while (Date.now() - start < timeout) {
      const instance = this.getProcessByPort(port);
      if (instance && typeof instance.process.exitCode === "number") {
        const sharedLibHint = instance.process.exitCode === 127
          ? " This usually indicates a missing runtime shared library (for example libonnxruntime.so.1)."
          : "";
        const outputTail = this.formatProcessOutputTail(instance.name);

        throw new Error(
          `Typesense process '${instance.name}' exited early with code ${instance.process.exitCode} before /health was ready on port ${port}.${sharedLibHint}${outputTail}`,
        );
      }

      if (instance && this.processExitCodes.has(instance.name)) {
        const code = this.processExitCodes.get(instance.name);
        const sharedLibHint = code === 127
          ? " This usually indicates a missing runtime shared library (for example libonnxruntime.so.1)."
          : "";
        const outputTail = this.formatProcessOutputTail(instance.name);
        throw new Error(
          `Typesense process '${instance.name}' exited early with code ${code} before /health was ready on port ${port}.${sharedLibHint}${outputTail}`,
        );
      }

      try {
        const res = await fetch(`${this.apiBaseUrl(port)}/health`);
        if (res.ok) return;
      } catch {
        // ignore and retry
      }
      await this.sleep(250);
    }
    throw new Error(`Timed out waiting for /health on port ${port} after ${timeout}ms`);
  }

  private getProcessByPort(port: number): ServerInstance | undefined {
    for (const instance of this.processes.values()) {
      if (instance.port === port) {
        return instance;
      }
    }

    return undefined;
  }

  private async resolveSinglePorts(): Promise<[number, number]> {
    if (
      this.configuredSingleApiPort !== null
      && this.configuredSinglePeeringPort !== null
      && this.configuredSingleApiPort !== this.configuredSinglePeeringPort
    ) {
      return [this.configuredSingleApiPort, this.configuredSinglePeeringPort];
    }

    if (this.singleNodeState) {
      return [this.singleNodeState.port, this.singleNodeState.peeringPort];
    }

    const candidateApiPort = this.configuredSingleApiPort ?? DEFAULT_SINGLE_API_PORT;
    const candidatePeeringPort = this.configuredSinglePeeringPort ?? DEFAULT_SINGLE_PEERING_PORT;

    if (candidateApiPort !== candidatePeeringPort) {
      const [apiPortAvailable, peeringPortAvailable] = await Promise.all([
        this.isPortFree(candidateApiPort),
        this.isPortFree(candidatePeeringPort),
      ]);

      if (apiPortAvailable && peeringPortAvailable) {
        this.configuredSingleApiPort = candidateApiPort;
        this.configuredSinglePeeringPort = candidatePeeringPort;
        return [candidateApiPort, candidatePeeringPort];
      }
    }

    const [fallbackApiPort, fallbackPeeringPort] = await TypesenseProcessManager.findFreePorts(2);
    this.configuredSingleApiPort = fallbackApiPort;
    this.configuredSinglePeeringPort = fallbackPeeringPort;
    return [fallbackApiPort, fallbackPeeringPort];
  }

  private async resolveMultiNodeConfigs(): Promise<MultiNodeConfig[]> {
    if (this.multiNodeConfigs) {
      return this.multiNodeConfigs;
    }

    let apiPorts = this.configuredMultiApiPorts;
    let peerPorts = this.configuredMultiPeeringPorts;

    if (!apiPorts || !peerPorts) {
      const defaultPortsAreFree = await Promise.all([
        ...DEFAULT_MULTI_API_PORTS.map((port) => this.isPortFree(port)),
        ...DEFAULT_MULTI_PEERING_PORTS.map((port) => this.isPortFree(port)),
      ]).then((results) => results.every(Boolean));

      if (defaultPortsAreFree) {
        apiPorts = DEFAULT_MULTI_API_PORTS;
        peerPorts = DEFAULT_MULTI_PEERING_PORTS;
      } else {
        const freePorts = await TypesenseProcessManager.findFreePorts(6);
        apiPorts = freePorts.slice(0, 3);
        peerPorts = freePorts.slice(3, 6);
      }
    }

    this.configuredMultiApiPorts = apiPorts;
    this.configuredMultiPeeringPorts = peerPorts;

    this.multiNodeConfigs = [
      {
        name: "multi-node1",
        port: apiPorts[0],
        peerPort: peerPorts[0],
        dataDir: "typesense-data-1",
        logDir: "typesense-1",
        analyticsDir: "typesense-data-1/analytics_db",
      },
      {
        name: "multi-node2",
        port: apiPorts[1],
        peerPort: peerPorts[1],
        dataDir: "typesense-data-2",
        logDir: "typesense-2",
        analyticsDir: "typesense-data-2/analytics_db",
      },
      {
        name: "multi-node3",
        port: apiPorts[2],
        peerPort: peerPorts[2],
        dataDir: "typesense-data-3",
        logDir: "typesense-3",
        analyticsDir: "typesense-data-3/analytics_db",
      },
    ];

    return this.multiNodeConfigs;
  }

  private async terminateProcess(instance: ServerInstance) {
    try {
      instance.process.kill("SIGINT");
    } catch {
      // Process may already be gone.
    }

    const gracefulExit = await Promise.race([
      instance.process.exited.then(() => true),
      this.sleep(SHUTDOWN_GRACE_MS).then(() => false),
    ]);

    if (!gracefulExit) {
      try {
        instance.process.kill("SIGKILL");
      } catch {
        // Process may have exited between checks.
      }
      await instance.process.exited;
    }
  }

  private installSignalHandlers() {
    if (TypesenseProcessManager.signalHandlersInstalled) {
      return;
    }

    TypesenseProcessManager.signalHandlersInstalled = true;

    process.on("SIGINT", () => {
      void TypesenseProcessManager.shutdownAll();
    });

    process.on("SIGTERM", () => {
      void TypesenseProcessManager.shutdownAll();
    });

    process.on("uncaughtException", (err) => {
      console.error("Uncaught Exception in api_tests:", err);
      void TypesenseProcessManager.shutdownAll().finally(() => process.exit(1));
    });

    process.on("unhandledRejection", (reason) => {
      console.error("Unhandled Rejection in api_tests:", reason);
      void TypesenseProcessManager.shutdownAll().finally(() => process.exit(1));
    });
  }

  private static async shutdownAll() {
    const managers = Array.from(TypesenseProcessManager.managers);
    for (const manager of managers) {
      await manager.shutdown();
    }
  }

  private apiBaseUrl(port: number): string {
    return `http://${this.apiHost}:${port}`;
  }

  private async isPortFree(port: number): Promise<boolean> {
    return new Promise((resolve) => {
      const server = createServer();
      server.unref();
      server.once("error", () => resolve(false));
      server.listen(port, "127.0.0.1", () => {
        server.close(() => resolve(true));
      });
    });
  }

  private static async findFreePort(): Promise<number> {
    return new Promise((resolve, reject) => {
      const server = createServer();
      server.unref();

      server.on("error", reject);
      server.listen(0, "127.0.0.1", () => {
        const address = server.address();
        if (!address || typeof address === "string") {
          server.close(() => reject(new Error("Unable to resolve free port")));
          return;
        }

        const port = address.port;
        server.close((err) => {
          if (err) {
            reject(err);
            return;
          }
          resolve(port);
        });
      });
    });
  }

  private sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  private async captureProcessOutput(
    name: string,
    streamName: keyof ProcessOutputTail,
    stream: ReadableStream<Uint8Array> | null,
  ) {
    if (!stream) {
      return;
    }

    const reader = stream.getReader();
    const decoder = new TextDecoder();

    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) {
          break;
        }
        if (value) {
          this.appendProcessOutput(name, streamName, decoder.decode(value, { stream: true }));
        }
      }
      const remaining = decoder.decode();
      if (remaining) {
        this.appendProcessOutput(name, streamName, remaining);
      }
    } catch {
      // Ignore output capture errors and keep health checks running.
    } finally {
      reader.releaseLock();
    }
  }

  private appendProcessOutput(name: string, streamName: keyof ProcessOutputTail, chunk: string) {
    const existing = this.processOutputTails.get(name) ?? { stdout: "", stderr: "" };
    const combined = `${existing[streamName]}${chunk}`;
    existing[streamName] = combined.slice(-PROCESS_OUTPUT_TAIL_MAX_CHARS);
    this.processOutputTails.set(name, existing);

    appendFileSync(this.getProcessOutputLogPath(name, streamName), chunk);
  }

  private getProcessOutputLogPath(name: string, streamName: keyof ProcessOutputTail): string {
    const processLogDir = join(this.baseDir, "logs", "process");
    mkdirSync(processLogDir, { recursive: true });
    return join(processLogDir, `${name}.${streamName}.log`);
  }

  private formatProcessOutputTail(name: string): string {
    const outputTail = this.processOutputTails.get(name);
    if (!outputTail) {
      return "";
    }

    const stderr = outputTail.stderr.trim();
    const stdout = outputTail.stdout.trim();
    const details: string[] = [];

    if (stderr.length > 0) {
      details.push(`stderr tail:\n${stderr}`);
    }
    if (stdout.length > 0) {
      details.push(`stdout tail:\n${stdout}`);
    }

    if (details.length === 0) {
      return "";
    }

    return `\nRecent process output for '${name}':\n${details.join("\n")}`;
  }

  private getIpAddress() {
    return process.env.TYPESENSE_PEERING_ADDRESS ?? "127.0.0.1";
  }
}
