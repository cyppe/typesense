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
    expect(createCollectionBody.success).toBe(true);
    expect(createCollectionBody.forwarded_to_leader).toBe(false);
    expect(createCollectionBody.target_server_id).toBeGreaterThan(0);
    expect(createCollectionBody.result.name).toBe("books");

    const createDocument = await fetchSingleNode("/collections/books/documents", {
      method: "POST",
      body: JSON.stringify({
        id: "1",
        title: "Dune",
      }),
    });

    expect(createDocument.ok).toBe(true);
    const createDocumentBody: any = await createDocument.json();
    expect(createDocumentBody.success).toBe(true);
    expect(createDocumentBody.result.id).toBe("1");

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
