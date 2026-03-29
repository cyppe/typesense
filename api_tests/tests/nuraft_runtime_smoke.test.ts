import { describe, expect, it } from "bun:test";
import { Phases } from "../src/constants";
import { fetchSingleNode } from "../src/request";

describe(Phases.SINGLE_FRESH, () => {
  it("creates a collection and document through the NuRaft runtime", async () => {
    const createCollection = await fetchSingleNode("/collections", {
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
    // Standard Typesense response: no Raft metadata fields.
    expect(createCollectionBody.success).toBeUndefined();
    expect(createCollectionBody.forwarded_to_leader).toBeUndefined();
    expect(createCollectionBody.target_server_id).toBeUndefined();
    expect(createCollectionBody.appended_index).toBeUndefined();
    // created_at must be a real timestamp, not 0.
    expect(createCollectionBody.created_at).toBeGreaterThan(0);

    const createDocument = await fetchSingleNode("/collections/books/documents", {
      method: "POST",
      body: JSON.stringify({
        id: "1",
        title: "Dune",
      }),
    });

    expect(createDocument.ok).toBe(true);
    const createDocumentBody: any = await createDocument.json();
    expect(createDocumentBody.id).toBe("1");
    expect(createDocumentBody.title).toBe("Dune");
    // Standard Typesense response: no Raft metadata.
    expect(createDocumentBody.success).toBeUndefined();
    expect(createDocumentBody.forwarded_to_leader).toBeUndefined();

    const collection = await fetchSingleNode("/collections/books");
    expect(collection.ok).toBe(true);
    const collectionBody: any = await collection.json();
    expect(collectionBody.name).toBe("books");
    expect(collectionBody.fields[0].name).toBe("title");

    const document = await fetchSingleNode("/collections/books/documents/1");
    expect(document.ok).toBe(true);
    const documentBody: any = await document.json();
    expect(documentBody.id).toBe("1");
    expect(documentBody.title).toBe("Dune");

    const status = await fetchSingleNode("/status");
    expect(status.ok).toBe(true);
    const statusBody: any = await status.json();
    expect(statusBody.is_leader).toBe(true);
    expect(statusBody.committed_index).toBeGreaterThanOrEqual(2);
    expect(statusBody.known_applied_index).toBeGreaterThanOrEqual(2);
  });

  it("supports auto-generated document IDs", async () => {
    // Regression test: documents POSTed without an "id" field must get a
    // server-generated ID returned in the response body.
    const createAutoId = await fetchSingleNode("/collections/books/documents", {
      method: "POST",
      body: JSON.stringify({ title: "Auto ID Book" }),
    });

    expect(createAutoId.ok).toBe(true);
    const autoIdBody: any = await createAutoId.json();
    expect(typeof autoIdBody.id).toBe("string");
    expect(autoIdBody.id.length).toBeGreaterThan(0);
    expect(autoIdBody.title).toBe("Auto ID Book");

    // Verify the document is retrievable by its generated ID.
    const fetchDoc = await fetchSingleNode(`/collections/books/documents/${autoIdBody.id}`);
    expect(fetchDoc.ok).toBe(true);
    const fetchedBody: any = await fetchDoc.json();
    expect(fetchedBody.id).toBe(autoIdBody.id);
    expect(fetchedBody.title).toBe("Auto ID Book");
  });

  it("returns 404 when deleting a non-existent collection", async () => {
    const deleteRes = await fetchSingleNode("/collections/nonexistent_xyz", {
      method: "DELETE",
    });
    expect(deleteRes.status).toBe(404);
    const deleteBody: any = await deleteRes.json();
    expect(deleteBody.message).toContain("No collection with name");
  });

  it("returns 404 when getting a non-existent document", async () => {
    const getRes = await fetchSingleNode("/collections/books/documents/does_not_exist");
    expect(getRes.status).toBe(404);
    const getBody: any = await getRes.json();
    expect(getBody.message).toContain("not found");
  });

  it("reports correct node state in /debug", async () => {
    const debugRes = await fetchSingleNode("/debug");
    expect(debugRes.ok).toBe(true);
    const debugBody: any = await debugRes.json();
    // Single-node should report state:1 (leader).
    expect(debugBody.state).toBe(1);
  });
});

describe(Phases.SINGLE_RESTARTED, () => {
  it("preserves the collection and document after restart", async () => {
    const collection = await fetchSingleNode("/collections/books");
    expect(collection.ok).toBe(true);
    const collectionBody: any = await collection.json();
    expect(collectionBody.name).toBe("books");

    const listed = await fetchSingleNode("/collections");
    expect(listed.ok).toBe(true);
    const listedBody: any = await listed.json();
    expect(Array.isArray(listedBody)).toBe(true);
    expect(listedBody.some((entry: any) => entry.name === "books")).toBe(true);

    const document = await fetchSingleNode("/collections/books/documents/1");
    expect(document.ok).toBe(true);
    const documentBody: any = await document.json();
    expect(documentBody.title).toBe("Dune");
  });
});

describe(Phases.SINGLE_SNAPSHOT, () => {
  it("preserves the collection and document after snapshot restore", async () => {
    const collection = await fetchSingleNode("/collections/books");
    expect(collection.ok).toBe(true);
    const collectionBody: any = await collection.json();
    expect(collectionBody.name).toBe("books");

    const document = await fetchSingleNode("/collections/books/documents/1");
    expect(document.ok).toBe(true);
    const documentBody: any = await document.json();
    expect(documentBody.id).toBe("1");
    expect(documentBody.title).toBe("Dune");

    const status = await fetchSingleNode("/status");
    expect(status.ok).toBe(true);
    const statusBody: any = await status.json();
    expect(statusBody.is_leader).toBe(true);
    expect(typeof statusBody.leader_url).toBe("string");
    expect(statusBody.leader_url.length).toBeGreaterThan(0);
  });
});
