import { describe, expect, it } from "bun:test";
import { Phases } from "../src/constants";
import { fetchMultiNode, fetchMultiNodeRequest } from "../src/request";

describe(Phases.MULTI_FRESH, () => {
  it("forwards follower-originated writes and makes them visible across the cluster", async () => {
    const createCollection = await fetchMultiNode(2, "/collections", {
      method: "POST",
      body: JSON.stringify({
        name: "books",
        fields: [
          { name: "title", type: "string" },
        ],
      }),
    });

    expect(createCollection.ok).toBe(true);
    const createCollectionBody: any = await createCollection.json();
    expect(createCollectionBody.name).toBe("books");

    const createDocument = await fetchMultiNode(3, "/collections/books/documents", {
      method: "POST",
      body: JSON.stringify({
        id: "1",
        title: "Dune",
      }),
    });

    expect(createDocument.ok).toBe(true);
    const createDocumentBody: any = await createDocument.json();
    expect(createDocumentBody.id).toBe("1");

    const collection = await fetchMultiNode(1, "/collections/books");
    expect(collection.ok).toBe(true);
    const collectionBody: any = await collection.json();
    expect(collectionBody.name).toBe("books");

    const document = await fetchMultiNode(2, "/collections/books/documents/1");
    expect(document.ok).toBe(true);
    const documentBody: any = await document.json();
    expect(documentBody.id).toBe("1");
    expect(documentBody.title).toBe("Dune");

    const leaderStatus = await fetchMultiNodeRequest(1, "/status");
    expect(leaderStatus.ok).toBe(true);
    const leaderStatusBody: any = await leaderStatus.json();
    expect(leaderStatusBody.is_leader).toBe(true);

    const followerStatus = await fetchMultiNodeRequest(2, "/status");
    expect(followerStatus.ok).toBe(true);
    const followerStatusBody: any = await followerStatus.json();
    expect(followerStatusBody.is_leader).toBe(false);
    expect(followerStatusBody.leader_url).toBe(leaderStatusBody.leader_url);
  });

  it("replicates auto-ID documents across the cluster", async () => {
    // Regression: documents POSTed without "id" must replicate to all nodes.
    const createAutoId = await fetchMultiNode(2, "/collections/books/documents", {
      method: "POST",
      body: JSON.stringify({ title: "Auto ID Cluster Test" }),
    });

    expect(createAutoId.ok).toBe(true);
    const autoIdBody: any = await createAutoId.json();
    expect(typeof autoIdBody.id).toBe("string");
    expect(autoIdBody.id.length).toBeGreaterThan(0);
    expect(autoIdBody.title).toBe("Auto ID Cluster Test");

    // Verify the document is visible on a different node.
    await new Promise((resolve) => setTimeout(resolve, 1000));
    const fetchDoc = await fetchMultiNodeRequest(3, `/collections/books/documents/${autoIdBody.id}`);
    expect(fetchDoc.ok).toBe(true);
    const fetchedBody: any = await fetchDoc.json();
    expect(fetchedBody.id).toBe(autoIdBody.id);
    expect(fetchedBody.title).toBe("Auto ID Cluster Test");
  });

  it("reports correct /debug state for leader and follower", async () => {
    // Find which node is leader vs follower.
    const debug1 = await fetchMultiNodeRequest(1, "/debug");
    const debug2 = await fetchMultiNodeRequest(2, "/debug");
    const debug3 = await fetchMultiNodeRequest(3, "/debug");

    expect(debug1.ok).toBe(true);
    expect(debug2.ok).toBe(true);
    expect(debug3.ok).toBe(true);

    const states = [
      (await debug1.json() as any).state,
      (await debug2.json() as any).state,
      (await debug3.json() as any).state,
    ];

    // Exactly one leader (state:1), the rest are followers (state:4).
    const leaders = states.filter((s) => s === 1);
    const followers = states.filter((s) => s === 4);
    expect(leaders.length).toBe(1);
    expect(followers.length).toBe(2);
  });
});

describe(Phases.MULTI_RESTARTED, () => {
  it("preserves the replicated document after cluster restart", async () => {
    const collection = await fetchMultiNode(3, "/collections/books");
    expect(collection.ok).toBe(true);
    const collectionBody: any = await collection.json();
    expect(collectionBody.name).toBe("books");

    const document = await fetchMultiNode(1, "/collections/books/documents/1");
    expect(document.ok).toBe(true);
    const documentBody: any = await document.json();
    expect(documentBody.id).toBe("1");
    expect(documentBody.title).toBe("Dune");
  });
});

describe(Phases.MULTI_SNAPSHOT, () => {
  it("preserves the replicated document after leader snapshot and cluster restart", async () => {
    const document = await fetchMultiNode(2, "/collections/books/documents/1");
    expect(document.ok).toBe(true);
    const documentBody: any = await document.json();
    expect(documentBody.id).toBe("1");
    expect(documentBody.title).toBe("Dune");

    const status = await fetchMultiNodeRequest(3, "/status");
    expect(status.ok).toBe(true);
    const statusBody: any = await status.json();
    expect(typeof statusBody.is_leader).toBe("boolean");
    expect(typeof statusBody.leader_url).toBe("string");
    expect(statusBody.leader_url.length).toBeGreaterThan(0);
  });
});
