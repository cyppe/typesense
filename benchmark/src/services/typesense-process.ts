import EventEmitter from "events";
import { constants, writeFile } from "fs/promises";
import { realpathSync } from "fs";
import { createServer } from "net";
import { networkInterfaces } from "os";
import path, { dirname } from "path";
import type { ErrorWithMessage } from "@/utils/error";
import type { ChildProcess } from "child_process";
import type { Options as ExecaOptions } from "execa";
import type { Result } from "neverthrow";
import type { Ora } from "ora";
import type { HealthResponse } from "typesense/lib/Typesense/Health";

import { execa, execaSync } from "execa";
import { err, errAsync, ok, okAsync, ResultAsync } from "neverthrow";
import ora from "ora";
import { Client } from "typesense";

import { toErrorWithMessage } from "@/utils/error";
import { exists, safeEmptyDir, safeMakeOrEmptyDir } from "@/utils/fs";
import { logger } from "@/utils/logger";
import { isStringifiable } from "@/utils/stringifiable";

interface AddressError {
  message: string;
  type: "address";
}

export interface SetupNodesOptions {
  skipCleanup?: boolean;
}

export interface NodeConfig {
  grpc: number;
  http: (typeof TypesenseProcessManager.defaultNodeToPortMap)[number]["http"];
  dataDir: string;
}

/**
 * This is a helper type to ensure that the number of elements in the tuple is the same as the number of elements in the array.
 * It is used to ensure that the number of directories is the same as the number of nodes.
 */
type StringTupleOfLength<T extends readonly unknown[], V = string> = {
  [K in keyof T]: V;
} & { length: T["length"] };

export class TypesenseProcessController extends EventEmitter {
  process: ChildProcess | null;
  exitCode: number | null = null;
  http: number;
  private readonly apiKey: string;
  error: Error | null = null;
  client: Client;
  node: NodeConfig;

  constructor(process: ChildProcess, http: number, apiKey: string, node: NodeConfig) {
    super();
    this.http = http;
    this.process = process;
    this.apiKey = apiKey;
    this.node = node;

    this.client = new Client({
      nodes: [
        {
          host: "localhost",
          port: http,
          protocol: "http",
        },
      ],
      logLevel: "DEBUG",
      apiKey: this.apiKey,
      connectionTimeoutSeconds: 100,
      retryIntervalSeconds: 0.2,
      numRetries: 20 * 30,
    });

    this.process.on("close", (code, signal) => {
      this.exitCode = code;
      const logLevel = code === 0 ? "info" : "error";
      logger[logLevel](`[Node on port ${this.http}] Process exited with code=${code} signal=${signal}`);
      this.exitCode = code;
      this.emit("exit", { code, http: this.http });
    });

    this.process.on("exit", (code, signal) => {
      this.exitCode = code;
      const logLevel = code === 0 ? "info" : "error";
      logger[logLevel](`[Node on port ${this.http}] Process exited with code=${code} signal=${signal}`);
      this.exitCode = code;
      this.emit("exit", { code, http: this.http });
    });

    this.process.on("error", (error) => {
      this.error = error;
      logger.error(`[Node on port ${this.http}] Process error: ${error.message}`);
      this.emit("error", { error, http: this.http });
    });
  }

  public dispose(): ResultAsync<void, ErrorWithMessage> {
    if (!this.process || this.process.killed) {
      return okAsync(undefined);
    }

    return ResultAsync.fromPromise(
      new Promise<void>((resolve, reject) => {
        try {
          this.process!.kill("SIGTERM");
          this.process!.removeAllListeners();
          this.removeAllListeners();

          const killTimeout = setTimeout(() => {
            if (this.process && !this.process.killed) {
              this.process.kill("SIGKILL");
              this.process.removeAllListeners();
            }
          }, 20_000);

          this.process!.once("exit", () => {
            clearTimeout(killTimeout);
            this.process = null;
            this.error = null;
            this.exitCode = null;
            logger.info(`[Node on port ${this.http}] Process killed`);
            resolve();
          });

          this.process!.once("error", (error) => {
            clearTimeout(killTimeout);
            if (error instanceof Error && "signal" in error) {
              if (error.signal === "SIGTERM" || error.signal === "SIGKILL") {
                resolve();
                return;
              }
            }
            reject(new Error(error instanceof Error ? error.message : String(error)));
          });
        } catch (error) {
          if (error instanceof Error && "signal" in error) {
            if (error.signal === "SIGTERM" || error.signal === "SIGKILL") {
              resolve();
              return;
            }
          }
          reject(new Error(error instanceof Error ? error.message : String(error)));
        }
      }),
      toErrorWithMessage,
    );
  }
}

export class TypesenseProcessManager {
  public processes = new Map<number, TypesenseProcessController>();
  private readonly isInCi = process.env.CI === "true";
  private readonly ipAddress?: string;
  private readonly snapshotPath: string;
  private hasSetupExitHandler = false;

  constructor(
    private readonly spinner: Ora,
    private readonly binaryPath: string,
    private readonly apiKey: string,
    private readonly workingDirectory: string,
    snapshotPath?: string,
    ipAddress?: string,
    baseHttpPort?: number,
    private readonly extraServerArgs?: string[],
  ) {
    this.ipAddress = ipAddress;
    this.snapshotPath = snapshotPath ?? path.join(this.workingDirectory, "snapshots");
    this.nodeToPortMap = baseHttpPort
      ? TypesenseProcessManager.buildPortMap(baseHttpPort)
      : [...TypesenseProcessManager.defaultNodeToPortMap];
    this.setupGlobalExitHandler();
  }

  public static readonly defaultNodeToPortMap = [
    { grpc: 8107, http: 8108 },
    { grpc: 7107, http: 7108 },
    { grpc: 9107, http: 9108 },
  ] as const;

  public readonly nodeToPortMap: { grpc: number; http: number }[];

  public static buildPortMap(baseHttpPort: number): { grpc: number; http: number }[] {
    const offset = baseHttpPort - 8108;
    return TypesenseProcessManager.defaultNodeToPortMap.map(({ grpc, http }) => ({
      grpc: grpc + offset,
      http: http + offset,
    }));
  }

  /**
   * Check if a port is available by attempting to bind to it.
   * Returns an error with details about what's using the port if it's occupied.
   */
  private static checkPortAvailable(port: number): ResultAsync<void, ErrorWithMessage> {
    return ResultAsync.fromPromise(
      new Promise<void>((resolve, reject) => {
        const server = createServer();
        server.once("error", (err: NodeJS.ErrnoException) => {
          if (err.code === "EADDRINUSE") {
            // Try to identify what's using the port
            let detail = `Port ${port} is already in use.`;
            try {
              const containers = execaSync("docker", [
                "ps", "--format", "{{.Names}}\t{{.Image}}\t{{.Ports}}",
                "--filter", `publish=${port}`,
              ]).stdout.trim();
              if (containers) {
                detail += ` Docker container(s) on this port:\n${containers}`;
              }
            } catch { /* docker check failed, give generic message */ }
            reject(new Error(detail));
          } else {
            reject(err);
          }
        });
        server.listen(port, "0.0.0.0", () => {
          server.close(() => resolve());
        });
      }),
      toErrorWithMessage,
    );
  }

  /**
   * Verify all ports in the port map are available before starting processes.
   */
  public ensurePortsAvailable(): ResultAsync<void, ErrorWithMessage> {
    const ports = this.nodeToPortMap.flatMap(({ grpc, http }) => [grpc, http]);
    return ResultAsync.combine(
      ports.map((port) => TypesenseProcessManager.checkPortAvailable(port)),
    ).map(() => undefined);
  }

  /**
   * Clean up any orphaned typesense-bench containers from previous runs.
   * This prevents port conflicts and resource leaks.
   */
  public cleanupStaleContainers(): ResultAsync<void, ErrorWithMessage> {
    return ResultAsync.fromPromise(
      (async () => {
        try {
          const result = execaSync("docker", [
            "ps", "-aq", "--filter", "name=typesense-bench-",
          ]).stdout.trim();
          if (result) {
            const ids = result.split("\n").filter(Boolean);
            logger.info(`Cleaning up ${ids.length} stale benchmark container(s)...`);
            execaSync("docker", ["rm", "-f", ...ids]);
            logger.info("Stale containers removed.");
          }
        } catch {
          // No containers to clean or docker not available
        }
      })(),
      toErrorWithMessage,
    );
  }

  restartProcess(port: (typeof TypesenseProcessManager.defaultNodeToPortMap)[number]["http"]) {
    const process = this.getProcessByHttpPort(port);

    if (process.isErr()) {
      return err(process.error);
    }

    this.spinner.start(`Restarting Process on ${process.value.http}`);

    return process.value.dispose().map(() => {
      this.processes.delete(process.value.http);
      return this.startProcess(process.value.node).map((newProcess) => {
        this.processes.set(newProcess.http, newProcess);
        this.spinner.succeed(`Restarted Process on ${newProcess.http}`);
      });
    });
  }

  stopProcess(
    port: (typeof TypesenseProcessManager.defaultNodeToPortMap)[number]["http"],
  ): ResultAsync<void, ErrorWithMessage> {
    const process = this.getProcessByHttpPort(port);

    if (process.isErr()) {
      return errAsync(process.error);
    }

    this.spinner.start("Stopping Typesense process");
    return process.value
      .dispose()
      .andThen(() => {
        this.processes.delete(process.value.http);
        this.spinner.succeed(`Stopped Typesense process on port ${process.value.http}`);
        return okAsync(undefined);
      })
      .mapErr((error) => {
        this.spinner.fail(`Failed to stop Typesense process: ${error.message}`);
        return error;
      });
  }

  getHealth(
    port: (typeof TypesenseProcessManager.defaultNodeToPortMap)[number]["http"],
  ): ResultAsync<HealthResponse, ErrorWithMessage> {
    const process = this.getProcessByHttpPort(port);

    if (process.isErr()) {
      return errAsync(process.error);
    }

    const spinner = ora().start(`Calling out to Typesense process on node ${process.value.http}\n`);

    return ResultAsync.fromPromise(process.value.client.health.retrieve(), toErrorWithMessage).andThen((res) => {
      if (!res.ok) {
        spinner.fail(`Typesense process on node ${process.value.http} is not healthy`);
        return errAsync({
          message: `Node ${process.value.http} health check failed`,
        });
      }
      spinner.succeed(`Typesense process on node ${process.value.http} is healthy`);
      return okAsync(res);
    });
  }

  snapshot(port: (typeof TypesenseProcessManager.defaultNodeToPortMap)[number]["http"]) {
    const process = this.getProcessByHttpPort(port);

    if (process.isErr()) {
      return err(process.error);
    }

    this.spinner.start(`Taking snapshot of Typesense process on node ${process.value.http}\n`);

    return ResultAsync.fromPromise(
      process.value.client.operations.perform("snapshot", {
        snapshot_path: this.snapshotPath,
      }),
      toErrorWithMessage,
    ).map(() => {
      this.spinner.succeed(`Took snapshot of Typesense process on node ${process.value.http}`);
      return okAsync(undefined);
    });
  }

  initNode(
    dataDir: string,
    port: (typeof TypesenseProcessManager.defaultNodeToPortMap)[number]["http"],
  ): ResultAsync<NodeConfig, ErrorWithMessage> {
    return exists(dataDir).andThen((exists) => {
      if (!exists) {
        return errAsync({ message: `${dataDir} does not exist` });
      }

      const portObj = this.nodeToPortMap.find((ports) => ports.http === port);

      if (!portObj) {
        return errAsync({ message: `${port} is not a valid port` });
      }

      return okAsync({
        dataDir: dataDir,
        grpc: portObj.grpc,
        http: portObj.http,
      });
    });
  }

  startProcess(
    node: NodeConfig,
    options?: { multiNode?: false },
  ): ResultAsync<TypesenseProcessController, ErrorWithMessage> {
    const { grpc, http } = node;
    this.spinner.start(`Starting Typesense process on node ${http}\n`);

    return exists(this.workingDirectory)
      .andThen((exists) =>
        exists ?
          okAsync(undefined)
        : errAsync({
            message: `${this.workingDirectory} does not exist`,
          }),
      )
      .andThen(() =>
        exists(this.binaryPath, constants.X_OK | constants.F_OK).andThen((exists) => {
          if (!exists) {
            return errAsync({
              message: `${this.binaryPath} does not exist or is not executable`,
            });
          }

          const args = this.buildProcessArgs(node, options);
          if (args.isErr()) {
            return errAsync(args.error);
          }

          const execaOptions: ExecaOptions = {
            cwd: this.workingDirectory,
            stdio: "pipe",
            shell: false,
            windowsHide: true,
            cleanup: true,
            extendEnv: true,
            buffer: false,
          };

          const containerName = `typesense-bench-${http}`;
          const realBinaryPath = realpathSync(this.binaryPath);
          const binDir = dirname(realBinaryPath);

          const requestTimeoutMs = process.env.TYPESENSE_REQUEST_TIMEOUT_MS;
          const dockerEnvArgs = requestTimeoutMs ? ["-e", `TYPESENSE_REQUEST_TIMEOUT_MS=${requestTimeoutMs}`] : [];

          const dockerArgs = [
            "run", "--rm", "--init",
            "--name", containerName,
            "--user", `${process.getuid!()}:${process.getgid!()}`,
            "--network", "benchmark_k6",
            "--hostname", containerName,
            "-p", `${http}:${http}`,
            "-p", `${grpc}:${grpc}`,
            "-e", `LD_LIBRARY_PATH=${binDir}`,
            ...dockerEnvArgs,
            "-v", `${binDir}:${binDir}:ro`,
            "-v", `${node.dataDir}:${node.dataDir}`,
            "ubuntu:24.04",
            realBinaryPath, ...args.value,
          ];

          logger.info(`[Node on port ${http}] Starting process with ports HTTP=${http} gRPC=${grpc}\n`);
          logger.info(`[Node on port ${http}] Command: docker ${dockerArgs.join(" ")}`);

          const typesenseProcess = execa("docker", dockerArgs, execaOptions);

          typesenseProcess.stdout?.on("data", (data) => {
            const message = isStringifiable(data) ? data.toString().trim() : "Not a stringifiable object";
            logger.info(`[Node on port ${http}] stdout: ${message}`);
          });

          typesenseProcess.stderr?.on("data", (data) => {
            const message = isStringifiable(data) ? data.toString().trim() : "Not a stringifiable object";
            logger.info(`[Node on port ${http}] stderr: ${message}`);
          });

          const typesenseInfo = new TypesenseProcessController(typesenseProcess, http, this.apiKey, node);

          this.processes.set(http, typesenseInfo);
          this.spinner.succeed(`Started Typesense process on node ${http}`);
          return okAsync(typesenseInfo);
        }),
      );
  }

  setupNodes(options?: SetupNodesOptions): ResultAsync<NodeConfig[], ErrorWithMessage> {
    return this.writeToNodesFile()
      .andThen(() => {
        if (options?.skipCleanup) {
          return okAsync(this.mapNodesToDirectories());
        }

        return this.createDataDirectories();
      })
      .andThen((directories) => {
        if (!this.verifyDataDirectories(directories)) {
          return errAsync({ message: "Number of directories does not match number of nodes" });
        }

        return okAsync(directories);
      })
      .andThen((directories) =>
        this.verifyDirectoriesExist(directories).andThen((exists) => {
          if (!exists) {
            return errAsync({ message: "Directories do not exist" });
          }

          this.spinner.succeed("All Directories exist");
          return okAsync(directories);
        }),
      )
      .andThen((directories) =>
        ResultAsync.combine(
          this.nodeToPortMap.map(({ http }, index) => this.initNode(directories[index]!, http)),
        ),
      );
  }

  private buildProcessArgs(node: NodeConfig, options?: { multiNode?: false }): Result<string[], AddressError> {
    const address = this.findAdressOrThrow();

    if (address.isErr()) {
      return err(address.error);
    }

    const { grpc, http, dataDir } = node;
    const multiNodeArgs = [`--nodes`, this.buildNodesConfig(address.value)];
    const networkArgs = ["--node-host", address.value, "--listen-address", "0.0.0.0"];
    const baseArgs = [
      `--data-dir=${dataDir}`,
      `--api-key=${this.apiKey}`,
      `--api-port`,
      `${http}`,
      `--peering-port`,
      `${grpc}`,
    ];

    const args: string[] = [];
    if (options?.multiNode !== false) {
      args.push(...multiNodeArgs);
    }
    args.push(...networkArgs);
    args.push(...baseArgs);
    if (this.extraServerArgs?.length) {
      args.push(...this.extraServerArgs);
    }
    return ok(args);
  }

  private findAdressOrThrow(): Result<string, AddressError> {
    if (this.ipAddress) {
      return ok(this.ipAddress);
    }

    const defaultAddress = this.findDefaultNetworkAddress();
    if (!defaultAddress) {
      return err({
        message: "[TypesenseProcessManager]: No default network address found",
        type: "address",
      });
    }

    return ok(defaultAddress);
  }

  private findDefaultNetworkAddress(): string | null {
    const interfaces = networkInterfaces();

    if (!this.isInCi) {
      if (process.env.IP_ADDRESS) {
        return process.env.IP_ADDRESS;
      }

      const networkAddress = Object.values(interfaces)
        .flatMap((interfaceInfo) => interfaceInfo ?? [])
        .find((info) => info.family === "IPv4" && !info.internal && !info.address.startsWith("127."))?.address;

      if (networkAddress) {
        return networkAddress;
      }

      return "127.0.0.1";
    }

    // First try to find a 10.1.0.* address
    const preferredAddress = Object.values(interfaces)
      .flatMap((interfaceInfo) => interfaceInfo ?? [])
      .find((info) => info.family === "IPv4" && info.address.startsWith("10.1.0."))?.address;

    if (preferredAddress) {
      return preferredAddress;
    }

    // Fallback: find any non-internal IPv4 address
    return (
      Object.values(interfaces)
        .flatMap((interfaceInfo) => interfaceInfo ?? [])
        .find((info) => info.family === "IPv4" && !info.internal)?.address ?? null
    );
  }

  getProcessByHttpPort(httpPort: number): Result<TypesenseProcessController, ErrorWithMessage> {
    const process = this.processes.get(httpPort);
    if (!process) {
      return err({
        message: `[TypesenseProcessManager]: Process on port ${httpPort} not found`,
        type: "process",
      });
    }

    if (!process.process) {
      return err({
        message: `[TypesenseProcessManager]: Process on port ${httpPort} not found. Has it been started?`,
        type: "process",
      });
    }

    return ok(process);
  }

  private setupGlobalExitHandler() {
    if (this.hasSetupExitHandler) return;

    const handleExit = () => {
      for (const process of this.processes.values()) {
        process.dispose();
      }
      // Clean up any orphaned benchmark containers
      try {
        const ids = execaSync("docker", ["ps", "-q", "--filter", "name=typesense-bench-"]).stdout.trim();
        if (ids) {
          execaSync("docker", ["rm", "-f", ...ids.split("\n")]);
        }
      } catch { /* ignore */ }
    };

    const currentCount = global.process.listenerCount("exit");
    const needed = currentCount + 1;
    if (global.process.getMaxListeners() < needed) {
      global.process.setMaxListeners(needed);
    }

    global.process.on("exit", handleExit);
    this.hasSetupExitHandler = true;
  }

  private mapNodesToDirectories() {
    return Array.from({ length: 3 }, (_, i) => path.join(this.workingDirectory, `typesense-data-${i + 1}`));
  }

  private createDataDirectories(): ResultAsync<string[], ErrorWithMessage> {
    return ResultAsync.combine(
      this.mapNodesToDirectories().map((directory) =>
        safeMakeOrEmptyDir({
          directory,
          options: { recursive: true },
        }),
      ),
    ).map((values) =>
      values.map((value) => {
        ora().succeed(`Created data directory ${value}`);
        return value;
      }),
    );
  }

  private verifyDataDirectories(
    directories: readonly string[],
  ): directories is StringTupleOfLength<typeof TypesenseProcessManager.defaultNodeToPortMap> {
    return directories.length == this.nodeToPortMap.length;
  }

  private verifyDirectoriesExist(
    directories: StringTupleOfLength<typeof TypesenseProcessManager.defaultNodeToPortMap>,
  ): ResultAsync<boolean, ErrorWithMessage> {
    return ResultAsync.combine(directories.map((dir) => exists(dir))).map(() => true);
  }

  private buildNodesConfig(ipAddress: string): string {
    return this.nodeToPortMap
      .map(({ grpc, http }) => `${ipAddress}:${grpc}:${http}`)
      .join(",");
  }

  private writeToNodesFile(): ResultAsync<string, ErrorWithMessage> {
    this.spinner.start("Writing nodes file");
    const nodesFile = path.join(this.workingDirectory, "nodes");

    const ipAddress = this.findAdressOrThrow();
    if (ipAddress.isErr()) {
      return errAsync(ipAddress.error);
    }

    const contents = this.buildNodesConfig(ipAddress.value);

    logger.info(`Writing nodes file to ${nodesFile} with contents:\n${contents}`);
    return ResultAsync.fromPromise(writeFile(nodesFile, contents, { encoding: "utf-8" }), toErrorWithMessage).map(
      () => {
        this.spinner.succeed("Nodes file written successfully");
        return nodesFile;
      },
    );
  }

  private emptyDataDirectories(): ResultAsync<void[], ErrorWithMessage> {
    return ResultAsync.combine(
      Array.from(this.processes.values()).map((value) =>
        ResultAsync.fromPromise(safeEmptyDir(value.node.dataDir), toErrorWithMessage).map(() => {
          ora().succeed(`Emptied data directory for node ${value.http}`);
        }),
      ),
    );
  }
}
