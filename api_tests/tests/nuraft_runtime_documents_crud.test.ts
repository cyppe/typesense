import { describe, expect, it } from "bun:test";
import { Phases } from "../src/constants";
import { fetchMultiNode, fetchSingleNode } from "../src/request";
import { z } from "zod";

const DocumentSchema = z.object({
  id: z.string(),
  company_name: z.string(),
  num_employees: z.number(),
  country: z.string(),
});

function singleNodeBaseUrl(path: string): string {
  const host = process.env.TYPESENSE_API_HOST ?? "localhost";
  const port = Number.parseInt(process.env.TYPESENSE_SINGLE_API_PORT ?? "8108", 10);
  return `http://${host}:${port}${path}`;
}

function singleNodeApiKey(): string {
  return process.env.TYPESENSE_API_KEY ?? "xyz";
}

async function fetchSingleNodeStatus(): Promise<{ committed_index: number }> {
  const res = await fetchSingleNode("/status");
  if (!res.ok) {
    throw new Error(`Expected /status to succeed, got HTTP ${res.status}`);
  }

  return (await res.json()) as { committed_index: number };
}

async function waitForSingleNodeDocumentCount(collection: string, expected: number, timeoutMs = 10000): Promise<void> {
  const deadline = Date.now() + timeoutMs;

  while (Date.now() < deadline) {
    const res = await fetchSingleNode(`/collections/${collection}`);
    if (res.ok) {
      const body = (await res.json()) as { num_documents?: number };
      if (body.num_documents === expected) {
        return;
      }
    }

    await new Promise((resolve) => setTimeout(resolve, 100));
  }

  throw new Error(`Timed out waiting for ${expected} documents in collection ${collection}`);
}

async function streamedImport(path: string, body: string, chunkCount = 3): Promise<Response> {
  const encoder = new TextEncoder();
  const chunkSize = Math.ceil(body.length / chunkCount);
  const chunks = Array.from({ length: chunkCount }, (_, index) =>
    body.slice(index * chunkSize, Math.min((index + 1) * chunkSize, body.length)),
  ).filter((chunk) => chunk.length > 0);

  const stream = new ReadableStream<Uint8Array>({
    async start(controller) {
      for (const chunk of chunks) {
        controller.enqueue(encoder.encode(chunk));
        await new Promise((resolve) => setTimeout(resolve, 25));
      }
      controller.close();
    },
  });

  return fetch(singleNodeBaseUrl(path), {
    method: "POST",
    body: stream,
    duplex: "half",
    headers: {
      "Content-Type": "text/plain",
      "X-TYPESENSE-API-KEY": singleNodeApiKey(),
    },
    signal: AbortSignal.timeout(120000),
  } as RequestInit & { duplex: "half" });
}

describe(Phases.SINGLE_FRESH, () => {
  it("supports bounded document CRUD and import flows", async () => {
    let res = await fetchSingleNode("/collections", {
      method: "POST",
      body: JSON.stringify({
        name: "companies_docs_single",
        fields: [
          { name: "id", type: "string" },
          { name: "company_name", type: "string" },
          { name: "num_employees", type: "int32" },
          { name: "country", type: "string", facet: true },
        ],
      }),
    });
    expect(res.ok).toBe(true);

    res = await fetchSingleNode("/collections/companies_docs_single/documents", {
      method: "POST",
      body: JSON.stringify({
        id: "1",
        company_name: "Stark Industries",
        num_employees: 10000,
        country: "US",
      }),
    });
    expect(res.ok).toBe(true);
    let document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.id).toBe("1");

    res = await fetchSingleNode("/collections/companies_docs_single/documents/1");
    expect(res.ok).toBe(true);
    document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.company_name).toBe("Stark Industries");

    res = await fetchSingleNode("/collections/companies_docs_single/documents/1", {
      method: "PATCH",
      body: JSON.stringify({ num_employees: 12000 }),
    });
    expect(res.ok).toBe(true);
    document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.id).toBe("1");
    expect(document.data?.num_employees).toBe(12000);

    res = await fetchSingleNode("/collections/companies_docs_single/documents", {
      method: "POST",
      body: JSON.stringify({
        id: "2",
        company_name: "Acme Corp",
        num_employees: 50,
        country: "DE",
      }),
    });
    expect(res.ok).toBe(true);

    res = await fetchSingleNode("/collections/companies_docs_single/documents/2", {
      method: "DELETE",
    });
    expect(res.ok).toBe(true);
    document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.id).toBe("2");

    const jsonl = [
      JSON.stringify({ id: "1", company_name: "Stark Industries", num_employees: 13000, country: "US" }),
      JSON.stringify({ id: "3", company_name: "Umbrella Corp", num_employees: 100, country: "US" }),
    ].join("\n");

    res = await fetchSingleNode("/collections/companies_docs_single/documents/import?action=upsert", {
      method: "POST",
      body: jsonl,
    });
    expect(res.ok).toBe(true);
    expect(res.status).toBe(200);
    const lines = (await res.text()).trim().split("\n");
    expect(lines).toHaveLength(2);
    for (const line of lines) {
      const parsed = JSON.parse(line) as { success?: boolean };
      expect(parsed.success).toBe(true);
    }

    res = await fetchSingleNode("/collections/companies_docs_single/documents/1");
    expect(res.ok).toBe(true);
    document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.num_employees).toBe(13000);

    res = await fetchSingleNode("/collections/companies_docs_single/documents/3");
    expect(res.ok).toBe(true);
    document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.company_name).toBe("Umbrella Corp");
  });

  it("buffers streamed bulk imports into bounded logical NuRaft chunks", async () => {
    const collectionName = "companies_docs_streamed_import";
    let res = await fetchSingleNode("/collections", {
      method: "POST",
      body: JSON.stringify({
        name: collectionName,
        fields: [
          { name: "id", type: "string" },
          { name: "company_name", type: "string" },
          { name: "num_employees", type: "int32" },
          { name: "country", type: "string", facet: true },
        ],
      }),
    });
    expect(res.ok).toBe(true);

    const beforeStatus = await fetchSingleNodeStatus();
    const documents = Array.from({ length: 6000 }, (_, index) =>
      JSON.stringify({
        id: `stream-${index}`,
        company_name: `Streamed Company ${index}`,
        num_employees: 1000 + index,
        country: index % 2 === 0 ? "US" : "DE",
      }),
    );
    const jsonl = documents.join("\n");

    res = await streamedImport(
      `/collections/${collectionName}/documents/import?action=upsert&batch_size=100`,
      jsonl,
    );
    expect(res.ok).toBe(true);
    expect(res.status).toBe(200);

    const lines = (await res.text()).trim().split("\n");
    expect(lines).toHaveLength(documents.length);
    for (const line of lines) {
      const parsed = JSON.parse(line) as { success?: boolean };
      expect(parsed.success).toBe(true);
    }

    await waitForSingleNodeDocumentCount(collectionName, documents.length);

    const afterStatus = await fetchSingleNodeStatus();
    expect(afterStatus.committed_index).toBe(beforeStatus.committed_index + 2);

    res = await fetchSingleNode(`/collections/${collectionName}/documents/stream-5999`);
    expect(res.ok).toBe(true);
    const document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.company_name).toBe("Streamed Company 5999");
  });
});

describe(Phases.SINGLE_RESTARTED, () => {
  it("preserves document state after restart", async () => {
    const res = await fetchSingleNode("/collections/companies_docs_single/documents/1");
    expect(res.ok).toBe(true);
    const document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.num_employees).toBe(13000);
  });
});

describe(Phases.SINGLE_SNAPSHOT, () => {
  it("preserves document state after snapshot restore", async () => {
    const res = await fetchSingleNode("/collections/companies_docs_single/documents/3");
    expect(res.ok).toBe(true);
    const document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.company_name).toBe("Umbrella Corp");
  });
});

describe(Phases.MULTI_FRESH, () => {
  it("replicates follower-originated document CRUD across nodes", async () => {
    let res = await fetchMultiNode(1, "/collections", {
      method: "POST",
      body: JSON.stringify({
        name: "companies_docs_multi",
        fields: [
          { name: "id", type: "string" },
          { name: "company_name", type: "string" },
          { name: "num_employees", type: "int32" },
          { name: "country", type: "string", facet: true },
        ],
      }),
    });
    expect(res.ok).toBe(true);

    res = await fetchMultiNode(2, "/collections/companies_docs_multi/documents", {
      method: "POST",
      body: JSON.stringify({
        id: "10",
        company_name: "Wayne Enterprises",
        num_employees: 5000,
        country: "US",
      }),
    });
    expect(res.ok).toBe(true);

    res = await fetchMultiNode(3, "/collections/companies_docs_multi/documents/10", {
      method: "PATCH",
      body: JSON.stringify({ num_employees: 5500 }),
    });
    expect(res.ok).toBe(true);
    let document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.num_employees).toBe(5500);

    res = await fetchMultiNode(1, "/collections/companies_docs_multi/documents/10");
    expect(res.ok).toBe(true);
    document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.company_name).toBe("Wayne Enterprises");
    expect(document.data?.num_employees).toBe(5500);
  });
});

describe(Phases.MULTI_RESTARTED, () => {
  it("preserves replicated documents after cluster restart", async () => {
    const res = await fetchMultiNode(2, "/collections/companies_docs_multi/documents/10");
    expect(res.ok).toBe(true);
    const document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.num_employees).toBe(5500);
  });
});

describe(Phases.MULTI_SNAPSHOT, () => {
  it("preserves replicated documents after leader snapshot and cluster restart", async () => {
    const res = await fetchMultiNode(3, "/collections/companies_docs_multi/documents/10");
    expect(res.ok).toBe(true);
    const document = DocumentSchema.safeParse(await res.json());
    expect(document.success).toBe(true);
    expect(document.data?.company_name).toBe("Wayne Enterprises");
  });
});
