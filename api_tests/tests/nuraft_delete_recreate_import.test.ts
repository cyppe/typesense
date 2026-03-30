import { describe, expect, it, beforeAll, afterAll } from "bun:test";
import { TypesenseProcessManager } from "../src/manager";

// ---------------------------------------------------------------------------
// TDD regression test for delete→create→import rapid succession.
//
// Bug: When a collection DELETE, CREATE, and IMPORT arrive in rapid succession,
// the leader's import handler can run before the CREATE has been applied locally
// (because write() uses a shared lock allowing concurrent execution). The import
// gets 404 "Collection not found" and aborts — the leader ends up with 0 docs
// while followers have the correct count.
//
// This test should FAIL with the unfixed code and PASS after the fix.
// ---------------------------------------------------------------------------

const API_KEY = "test-delete-recreate-import";
const COLLECTION_NAME = "rapid_recreate_products";
const DOC_COUNT = 700;

let manager: TypesenseProcessManager;

function fetchNode(port: number, path: string, options?: RequestInit) {
  return fetch(`http://localhost:${port}${path}`, {
    ...options,
    headers: {
      ...options?.headers,
      "X-TYPESENSE-API-KEY": API_KEY,
    },
    signal: AbortSignal.timeout(30000),
  });
}

async function getStatus(port: number): Promise<any> {
  const res = await fetchNode(port, "/status");
  if (!res.ok) return null;
  return res.json();
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

async function waitForConvergence(ports: number[], timeoutMs = 30000): Promise<boolean> {
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
      const converged =
        committed[0] === committed[1] &&
        committed[0] === committed[2] &&
        applied[0] === committed[0] &&
        applied[1] === committed[1] &&
        applied[2] === committed[2];
      if (converged) return true;
    } catch {}
    await Bun.sleep(300);
  }
  return false;
}

function generateDocuments(count: number): string {
  const lines: string[] = [];
  for (let i = 0; i < count; i++) {
    lines.push(JSON.stringify({
      id: `doc-${i}`,
      title: `Product ${i}`,
      price: i * 1.5,
    }));
  }
  return lines.join("\n");
}

describe("NuRaft delete-then-create-then-import rapid succession", () => {
  let port1: number, port2: number, port3: number;

  beforeAll(async () => {
    manager = new TypesenseProcessManager(undefined, undefined, { isolateBaseDir: true });
    (manager as any).apiKey = API_KEY;
    await manager.startMultiNode();

    port1 = manager.getMultiNodeApiPort(1)!;
    port2 = manager.getMultiNodeApiPort(2)!;
    port3 = manager.getMultiNodeApiPort(3)!;
  }, 90000);

  afterAll(async () => {
    await manager.shutdown();
  }, 30000);

  it("all nodes converge after rapid delete-create-import cycles", async () => {
    const ports = [port1, port2, port3];
    const leader = await waitForLeaderOnNodes(ports);
    expect(leader).not.toBeNull();
    const leaderPort = leader!.leaderPort;

    const ITERATIONS = 3;
    const jsonl = generateDocuments(DOC_COUNT);

    for (let cycle = 0; cycle < ITERATIONS; cycle++) {
      // Delete (first cycle may get 404 — that's fine)
      if (cycle > 0) {
        const delRes = await fetchNode(leaderPort, `/collections/${COLLECTION_NAME}`, {
          method: "DELETE",
        });
        // Could be 200 or 404 on first cycle
        expect(delRes.status === 200 || delRes.status === 404).toBe(true);
      }

      // Immediately create — no convergence wait
      const createRes = await fetchNode(leaderPort, "/collections", {
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

      // Immediately import 700 docs — no convergence wait
      const importRes = await fetchNode(
        leaderPort,
        `/collections/${COLLECTION_NAME}/documents/import?action=create`,
        { method: "POST", body: jsonl },
      );
      expect(importRes.ok).toBe(true);
      const importText = await importRes.text();
      const importLines = importText.trim().split("\n");
      expect(importLines).toHaveLength(DOC_COUNT);
      for (const line of importLines) {
        const parsed = JSON.parse(line);
        expect(parsed.success).toBe(true);
      }

      // NOW wait for convergence across all 3 nodes
      const converged = await waitForConvergence(ports, 30000);
      expect(converged).toBe(true);

      // Verify ALL nodes report the correct document count
      for (let nodeIdx = 0; nodeIdx < ports.length; nodeIdx++) {
        const collRes = await fetchNode(ports[nodeIdx], `/collections/${COLLECTION_NAME}`);
        expect(collRes.ok).toBe(true);
        const collBody: any = await collRes.json();
        expect(collBody.num_documents).toBe(DOC_COUNT);
      }
    }
  }, 180000);

  it("all nodes converge when import is sent via follower", async () => {
    const ports = [port1, port2, port3];
    const leader = await waitForLeaderOnNodes(ports);
    expect(leader).not.toBeNull();
    const leaderPort = leader!.leaderPort;
    // Pick a follower port
    const followerPort = ports.find((p) => p !== leaderPort)!;

    // Delete existing collection via leader
    await fetchNode(leaderPort, `/collections/${COLLECTION_NAME}`, { method: "DELETE" });
    await waitForConvergence(ports, 10000);

    // Create via follower (auto-forwarded to leader)
    const createRes = await fetchNode(followerPort, "/collections", {
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

    // Immediately import via same follower — no convergence wait
    const jsonl = generateDocuments(DOC_COUNT);
    const importRes = await fetchNode(
      followerPort,
      `/collections/${COLLECTION_NAME}/documents/import?action=create`,
      { method: "POST", body: jsonl },
    );
    expect(importRes.ok).toBe(true);

    // Wait for convergence
    const converged = await waitForConvergence(ports, 30000);
    expect(converged).toBe(true);

    // Verify ALL nodes
    for (const port of ports) {
      const collRes = await fetchNode(port, `/collections/${COLLECTION_NAME}`);
      expect(collRes.ok).toBe(true);
      const collBody: any = await collRes.json();
      expect(collBody.num_documents).toBe(DOC_COUNT);
    }
  }, 120000);
});
