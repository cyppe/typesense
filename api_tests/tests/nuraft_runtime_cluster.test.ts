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
    expect(createCollectionBody.success).toBe(true);
    expect(createCollectionBody.forwarded_to_leader).toBe(true);

    const createDocument = await fetchMultiNode(3, "/collections/books/documents", {
      method: "POST",
      body: JSON.stringify({
        id: "1",
        title: "Dune",
      }),
    });

    expect(createDocument.ok).toBe(true);
    const createDocumentBody: any = await createDocument.json();
    expect(createDocumentBody.success).toBe(true);
    expect(createDocumentBody.forwarded_to_leader).toBe(true);

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
    expect(statusBody.is_leader).toBe(false);
    expect(typeof statusBody.leader_url).toBe("string");
    expect(statusBody.leader_url.length).toBeGreaterThan(0);
  });
});
