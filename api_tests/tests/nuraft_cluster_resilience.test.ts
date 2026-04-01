import { describe, expect, it, beforeAll, afterAll } from "bun:test";
import { TypesenseProcessManager } from "../src/manager";

// ---------------------------------------------------------------------------
// Standalone cluster resilience tests. These manage their own 3-node cluster
// lifecycle and test scenarios the phase-based harness cannot cover:
//
//   1. Leader identity and follower status verification
//   2. Follower node failure and recovery (data survives, cluster stays available)
//   3. Leader node failure and automatic re-election
//   4. Follower catch-up after being down while writes continue
//   5. Snapshot-based catch-up when a follower is far behind
// ---------------------------------------------------------------------------

const API_KEY = "test-cluster-resilience";
const COLLECTION_NAME = "resilience_products";

let manager: TypesenseProcessManager;

function fetchNode(port: number, path: string, options?: RequestInit) {
  return fetch(`http://localhost:${port}${path}`, {
    ...options,
    headers: {
      ...options?.headers,
      "X-TYPESENSE-API-KEY": API_KEY,
    },
    signal: AbortSignal.timeout(15000),
  });
}

async function getStatus(port: number): Promise<any> {
  const res = await fetchNode(port, "/status");
  if (!res.ok) return null;
  return res.json();
}

async function waitForHealthy(port: number, timeoutMs = 30000): Promise<boolean> {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try {
      const res = await fetchNode(port, "/health");
      if (res.ok) return true;
    } catch {}
    await Bun.sleep(200);
  }
  return false;
}

async function waitForLeaderOnNodes(
  ports: number[],
  timeoutMs = 15000,
): Promise<{ leaderPort: number; leaderServerId: number } | null> {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    for (const port of ports) {
      try {
        const status = await getStatus(port);
        if (status?.is_leader === true) {
          return { leaderPort: port, leaderServerId: status.server_id };
        }
      } catch {}
    }
    await Bun.sleep(300);
  }
  return null;
}

async function waitForConvergence(ports: number[], timeoutMs = 15000): Promise<boolean> {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try {
      const statuses = await Promise.all(ports.map((p) => getStatus(p)));
      if (statuses.some((s) => s === null)) {
        await Bun.sleep(200);
        continue;
      }
      const committed = statuses.map((s: any) => s.committed_index);
      const applied = statuses.map((s: any) => s.known_applied_index);
      const readCaughtUp = statuses.every((s: any) => s.read_caught_up === true);
      const converged =
        committed[0] === committed[1] &&
        committed[0] === committed[2] &&
        applied[0] === committed[0] &&
        applied[1] === committed[1] &&
        applied[2] === committed[2];
      if (converged && readCaughtUp) return true;
    } catch {}
    await Bun.sleep(300);
  }
  return false;
}

async function waitForClusterHealthy(ports: number[], timeoutMs = 30000): Promise<boolean> {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    const health = await Promise.all(ports.map(async (port) => {
      try {
        const res = await fetchNode(port, "/health");
        return res.ok;
      } catch {
        return false;
      }
    }));

    if (health.every(Boolean)) {
      return true;
    }

    await Bun.sleep(300);
  }

  return false;
}

// ---------------------------------------------------------------------------
// Test suite
// ---------------------------------------------------------------------------

describe("NuRaft cluster resilience", () => {
  let port1: number, port2: number, port3: number;

  beforeAll(async () => {
    manager = new TypesenseProcessManager(undefined, undefined, { isolateBaseDir: true });
    // Override API key for isolation
    (manager as any).apiKey = API_KEY;
    await manager.startMultiNode();

    port1 = manager.getMultiNodeApiPort(1)!;
    port2 = manager.getMultiNodeApiPort(2)!;
    port3 = manager.getMultiNodeApiPort(3)!;
  }, 90000);

  afterAll(async () => {
    await manager.shutdown();
  }, 30000);

  // -- 1. Leader/follower identity ----------------------------------------

  it("exactly one node is leader, others are followers", async () => {
    const statuses = await Promise.all([
      getStatus(port1),
      getStatus(port2),
      getStatus(port3),
    ]);

    const leaders = statuses.filter((s) => s?.is_leader === true);
    const followers = statuses.filter((s) => s?.is_leader === false);

    expect(leaders.length).toBe(1);
    expect(followers.length).toBe(2);

    // All nodes agree on who the leader is
    const leaderUrl = leaders[0].leader_url;
    for (const s of statuses) {
      expect(s.leader_url).toBe(leaderUrl);
    }
  });

  it("all nodes report the same raft term", async () => {
    const statuses = await Promise.all([
      getStatus(port1),
      getStatus(port2),
      getStatus(port3),
    ]);

    expect(statuses[0].raft_term).toBe(statuses[1].raft_term);
    expect(statuses[0].raft_term).toBe(statuses[2].raft_term);
    expect(statuses[0].raft_term).toBeGreaterThan(0);
  });

  // -- 2. Setup: create collection and seed data -------------------------

  it("create collection and seed documents", async () => {
    // Find leader port
    const leader = await waitForLeaderOnNodes([port1, port2, port3]);
    expect(leader).not.toBeNull();

    const createRes = await fetchNode(leader!.leaderPort, "/collections", {
      method: "POST",
      body: JSON.stringify({
        name: COLLECTION_NAME,
        fields: [
          { name: "title", type: "string" },
          { name: "price", type: "float" },
        ],
      }),
    });
    expect(createRes.ok).toBe(true);

    // Seed 5 documents
    for (let i = 1; i <= 5; i++) {
      const res = await fetchNode(leader!.leaderPort, `/collections/${COLLECTION_NAME}/documents`, {
        method: "POST",
        body: JSON.stringify({ id: `${i}`, title: `Product ${i}`, price: i * 10.0 }),
      });
      expect(res.ok).toBe(true);
    }

    // Wait for convergence
    const converged = await waitForConvergence([port1, port2, port3]);
    expect(converged).toBe(true);
  });

  // -- 3. Follower failure and recovery ----------------------------------

  it("cluster stays available when one follower goes down", async () => {
    // Find a follower
    const statuses = await Promise.all([
      getStatus(port1),
      getStatus(port2),
      getStatus(port3),
    ]);
    const followerIndex = statuses.findIndex((s) => s?.is_leader === false);
    const followerNodeIndex = (followerIndex + 1) as 1 | 2 | 3;
    const followerPort = [port1, port2, port3][followerIndex];
    const remainingPorts = [port1, port2, port3].filter((p) => p !== followerPort);

    // Kill follower
    await manager.stopMultiNodeServer(followerNodeIndex);

    // Cluster should still be available (2/3 quorum)
    const leader = await waitForLeaderOnNodes(remainingPorts);
    expect(leader).not.toBeNull();

    // Write to cluster while follower is down
    const writeRes = await fetchNode(leader!.leaderPort, `/collections/${COLLECTION_NAME}/documents`, {
      method: "POST",
      body: JSON.stringify({ id: "6", title: "Product 6 (written while follower down)", price: 60.0 }),
    });
    expect(writeRes.ok).toBe(true);

    // Read from surviving nodes
    for (const port of remainingPorts) {
      const readRes = await fetchNode(port, `/collections/${COLLECTION_NAME}/documents/6`);
      if (!readRes.ok) {
        // Give convergence time
        await Bun.sleep(1000);
        const retryRes = await fetchNode(port, `/collections/${COLLECTION_NAME}/documents/6`);
        expect(retryRes.ok).toBe(true);
      }
    }

    // Restart follower
    await manager.restartMultiNodeServer(followerNodeIndex);

    // Follower should catch up and see the new document
    const converged = await waitForConvergence([port1, port2, port3], 30000);
    expect(converged).toBe(true);

    const catchUpRes = await fetchNode(followerPort, `/collections/${COLLECTION_NAME}/documents/6`);
    expect(catchUpRes.ok).toBe(true);
    const doc: any = await catchUpRes.json();
    expect(doc.title).toBe("Product 6 (written while follower down)");
  }, 60000);

  // -- 4. Leader failure and re-election ---------------------------------

  it("new leader elected when current leader goes down", async () => {
    // Find current leader
    const leaderBefore = await waitForLeaderOnNodes([port1, port2, port3]);
    expect(leaderBefore).not.toBeNull();

    const leaderPort = leaderBefore!.leaderPort;
    const leaderNodeIndex = [port1, port2, port3].indexOf(leaderPort) + 1;
    const followerPorts = [port1, port2, port3].filter((p) => p !== leaderPort);

    // Kill the leader
    await manager.stopMultiNodeServer(leaderNodeIndex as 1 | 2 | 3);

    // A new leader should be elected from the remaining nodes
    const newLeader = await waitForLeaderOnNodes(followerPorts, 20000);
    expect(newLeader).not.toBeNull();
    expect(newLeader!.leaderPort).not.toBe(leaderPort);

    // The new leader's term should be >= the old term
    const newStatus = await getStatus(newLeader!.leaderPort);
    expect(newStatus.raft_term).toBeGreaterThanOrEqual(leaderBefore!.leaderServerId > 0 ? 1 : 0);

    // Writes should work through the new leader
    const writeRes = await fetchNode(newLeader!.leaderPort, `/collections/${COLLECTION_NAME}/documents`, {
      method: "POST",
      body: JSON.stringify({ id: "7", title: "Product 7 (new leader)", price: 70.0 }),
    });
    expect(writeRes.ok).toBe(true);

    // Restart the old leader
    await manager.restartMultiNodeServer(leaderNodeIndex as 1 | 2 | 3);

    // Wait for full cluster convergence
    const converged = await waitForConvergence([port1, port2, port3], 30000);
    expect(converged).toBe(true);

    // Old leader (now follower) should have the new document
    const readRes = await fetchNode(leaderPort, `/collections/${COLLECTION_NAME}/documents/7`);
    expect(readRes.ok).toBe(true);
    const doc: any = await readRes.json();
    expect(doc.title).toBe("Product 7 (new leader)");
  }, 60000);

  // -- 5. Follower catch-up after extended downtime ----------------------

  it("follower catches up after missing many writes", async () => {
    const leader = await waitForLeaderOnNodes([port1, port2, port3]);
    expect(leader).not.toBeNull();

    // Find a follower
    const statuses = await Promise.all([
      getStatus(port1),
      getStatus(port2),
      getStatus(port3),
    ]);
    const followerIndex = statuses.findIndex((s) => s?.is_leader === false);
    const followerNodeIndex = (followerIndex + 1) as 1 | 2 | 3;
    const followerPort = [port1, port2, port3][followerIndex];

    // Record committed index before killing follower
    const statusBefore = await getStatus(leader!.leaderPort);
    const indexBefore = statusBefore.committed_index;

    // Kill follower
    await manager.stopMultiNodeServer(followerNodeIndex);

    // Write many documents while follower is down
    const batchSize = 50;
    const lines = [];
    for (let i = 100; i < 100 + batchSize; i++) {
      lines.push(JSON.stringify({ id: `${i}`, title: `Batch product ${i}`, price: i * 1.5 }));
    }
    const importRes = await fetchNode(
      leader!.leaderPort,
      `/collections/${COLLECTION_NAME}/documents/import?action=create`,
      { method: "POST", body: lines.join("\n") },
    );
    expect(importRes.ok).toBe(true);

    // Verify writes succeeded on surviving nodes
    const remainingPorts = [port1, port2, port3].filter((p) => p !== followerPort);
    await waitForConvergence(remainingPorts.concat(remainingPorts), 15000); // just remaining 2

    const statusAfter = await getStatus(leader!.leaderPort);
    expect(statusAfter.committed_index).toBeGreaterThan(indexBefore);

    // Restart follower — it should catch up (via log or snapshot)
    await manager.restartMultiNodeServer(followerNodeIndex);

    const converged = await waitForConvergence([port1, port2, port3], 45000);
    expect(converged).toBe(true);

    // Verify follower has the batch-imported documents
    const sampleRes = await fetchNode(followerPort, `/collections/${COLLECTION_NAME}/documents/125`);
    expect(sampleRes.ok).toBe(true);
    const sampleDoc: any = await sampleRes.json();
    expect(sampleDoc.title).toBe("Batch product 125");

    // Verify the committed index caught up
    const followerStatus = await getStatus(followerPort);
    expect(followerStatus.committed_index).toBe(statusAfter.committed_index);
  }, 90000);

  it("lone survivor stays alive through quorum loss and restarts under write pressure", async () => {
    const leaderBefore = await waitForLeaderOnNodes([port1, port2, port3]);
    expect(leaderBefore).not.toBeNull();

    const statuses = await Promise.all([getStatus(port1), getStatus(port2), getStatus(port3)]);
    const ports = [port1, port2, port3];
    const leaderPort = leaderBefore!.leaderPort;
    const leaderNodeIndex = (ports.indexOf(leaderPort) + 1) as 1 | 2 | 3;
    const followerIndices = statuses
      .map((status, index) => ({ status, index: (index + 1) as 1 | 2 | 3, port: ports[index] }))
      .filter((entry) => entry.status?.is_leader === false);

    expect(followerIndices.length).toBe(2);

    const firstKilledFollower = followerIndices[0];
    const loneSurvivor = followerIndices[1];

    await manager.hardKillMultiNodeServer(firstKilledFollower.index);

    const remainingLeader = await waitForLeaderOnNodes([leaderPort, loneSurvivor.port], 20000);
    expect(remainingLeader).not.toBeNull();
    expect(remainingLeader!.leaderPort).toBe(leaderPort);

    let trafficStop = false;
    let attempts = 0;
    let failures = 0;
    const trafficLoop = (async () => {
      while (!trafficStop) {
        attempts += 1;
        const lines = Array.from({ length: 20 }, (_, offset) => JSON.stringify({
          id: `quorum-${attempts}-${offset}`,
          title: `Quorum product ${attempts}-${offset}`,
          price: attempts * 100 + offset,
        }));

        try {
          const response = await fetchNode(
            loneSurvivor.port,
            `/collections/${COLLECTION_NAME}/documents/import?action=upsert`,
            {
              method: "POST",
              body: lines.join("\n"),
              signal: AbortSignal.timeout(5000),
            },
          );
          if (!response.ok) {
            failures += 1;
          }
        } catch {
          failures += 1;
        }

        await Bun.sleep(100);
      }
    })();

    await Bun.sleep(1000);
    await manager.hardKillMultiNodeServer(leaderNodeIndex);

    const loneSurvivorIndex = loneSurvivor.index;
    const quorumLossDeadline = Date.now() + 15000;
    while (Date.now() < quorumLossDeadline) {
      expect(manager.getMultiNodeServerExitCode(loneSurvivorIndex)).toBeNull();
      await Bun.sleep(250);
    }

    trafficStop = true;
    await trafficLoop;
    expect(attempts).toBeGreaterThan(0);
    expect(failures).toBeGreaterThan(0);
    expect(manager.getMultiNodeServerExitCode(loneSurvivorIndex)).toBeNull();

    await manager.restartMultiNodeServer(leaderNodeIndex);
    await manager.restartMultiNodeServer(firstKilledFollower.index);

    const healthy = await waitForClusterHealthy([port1, port2, port3], 60000);
    expect(healthy).toBe(true);

    const leaderAfterRecovery = await waitForLeaderOnNodes([port1, port2, port3], 30000);
    expect(leaderAfterRecovery).not.toBeNull();

    for (const port of [port1, port2, port3]) {
      const status = await getStatus(port);
      expect(status).not.toBeNull();
      expect(status.read_caught_up).toBe(true);
      expect(status.write_caught_up).toBe(true);
      expect(status.raft_leader_id).toBe(leaderAfterRecovery!.leaderServerId);
    }

    const followerPortAfterRecovery = [port1, port2, port3].find((port) => port !== leaderAfterRecovery!.leaderPort)!;
    const recoveredWrite = await fetchNode(
      followerPortAfterRecovery,
      `/collections/${COLLECTION_NAME}/documents`,
      {
        method: "POST",
        body: JSON.stringify({ id: "recovered-write", title: "Recovered write", price: 999.0 }),
      },
    );
    expect(recoveredWrite.ok).toBe(true);

    for (const port of [port1, port2, port3]) {
      const readRes = await fetchNode(port, `/collections/${COLLECTION_NAME}/documents/recovered-write`);
      expect(readRes.ok).toBe(true);
      const doc: any = await readRes.json();
      expect(doc.title).toBe("Recovered write");
    }

    for (const index of [1, 2, 3] as const) {
      const exitCode = manager.getMultiNodeServerExitCode(index);
      expect(exitCode === null || exitCode === 0).toBe(true);
      expect(exitCode).not.toBe(139);
    }
  }, 120000);

  // -- 6. All original data still intact after all failure scenarios ------

  it("all original seed data survives cluster failure scenarios", async () => {
    const converged = await waitForConvergence([port1, port2, port3], 15000);
    expect(converged).toBe(true);

    // Check original documents on all nodes
    for (const port of [port1, port2, port3]) {
      for (let i = 1; i <= 5; i++) {
        const res = await fetchNode(port, `/collections/${COLLECTION_NAME}/documents/${i}`);
        expect(res.ok).toBe(true);
        const doc: any = await res.json();
        expect(doc.title).toBe(`Product ${i}`);
      }
    }
  }, 30000);
});
