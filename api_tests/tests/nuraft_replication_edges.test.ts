import { describe, expect, it } from "bun:test";
import { Phases } from "../src/constants";
import { fetchMultiNode, fetchMultiNodeRequest } from "../src/request";

// ---------------------------------------------------------------------------
// This file exercises NuRaft replication edge cases that the per-feature test
// files do not cover:
//
//   1. Follower-originated metadata writes (aliases, presets, stopwords)
//      auto-forwarded to the leader and visible across all nodes.
//   2. Persistence of replicated metadata across cluster restart.
//   3. Persistence of replicated metadata across leader snapshot + restart.
//   4. Committed-index convergence after mixed write origins.
// ---------------------------------------------------------------------------

// -- multi-fresh: create metadata through various nodes ---------------------

describe(Phases.MULTI_FRESH, () => {
  // Create prerequisite collection on leader first to let cluster stabilize.
  it("create prerequisite collection from leader", async () => {
    const res = await fetchMultiNode(1, "/collections", {
      method: "POST",
      body: JSON.stringify({
        name: "edge_products",
        fields: [
          { name: "title", type: "string" },
          { name: "price", type: "float" },
        ],
      }),
    });
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.name).toBe("edge_products");
  });

  // --- Aliases (written from follower node 3) ---

  it("create alias from follower node 3", async () => {
    const res = await fetchMultiNode(3, "/aliases/edge_products_alias", {
      method: "PUT",
      body: JSON.stringify({ collection_name: "edge_products" }),
    });
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    const name = body.name ?? body.result?.name;
    expect(name).toBe("edge_products_alias");
  });

  it("alias visible on leader node 1", async () => {
    const res = await fetchMultiNode(1, "/aliases/edge_products_alias");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.collection_name).toBe("edge_products");
  });

  it("alias visible on follower node 2", async () => {
    const res = await fetchMultiNode(2, "/aliases/edge_products_alias");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.collection_name).toBe("edge_products");
  });

  // --- Presets (written from follower node 2) ---

  it("create preset from follower node 2", async () => {
    const res = await fetchMultiNode(2, "/presets/edge_default_search", {
      method: "PUT",
      body: JSON.stringify({
        value: { searches: [{ q: "*", query_by: "title" }] },
      }),
    });
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    const name = body.name ?? body.result?.name;
    expect(name).toBe("edge_default_search");
  });

  it("preset visible on follower node 3", async () => {
    const res = await fetchMultiNode(3, "/presets/edge_default_search");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.name).toBe("edge_default_search");
  });

  // --- Stopwords (written from follower node 3 to test forwarding) ---

  it("create stopword set from follower node 3", async () => {
    const res = await fetchMultiNode(3, "/stopwords/edge_english_basic", {
      method: "PUT",
      body: JSON.stringify({
        stopwords: ["the", "a", "an", "is", "are"],
        locale: "en",
      }),
    });
    expect(res.ok).toBe(true);
  });

  it("stopwords visible on leader node 1", async () => {
    const res = await fetchMultiNode(1, "/stopwords/edge_english_basic");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    // GET /stopwords/:name returns { stopwords: { id, locale, stopwords: [...] } }
    expect(body.stopwords?.stopwords).toContain("the");
  });

  // --- Document write from follower to verify mixed-origin convergence ---

  it("write document from follower node 2", async () => {
    const res = await fetchMultiNode(2, "/collections/edge_products/documents", {
      method: "POST",
      body: JSON.stringify({ id: "edge-1", title: "Widget", price: 9.99 }),
    });
    expect(res.ok).toBe(true);
  });

  it("document visible on follower node 3", async () => {
    const res = await fetchMultiNode(3, "/collections/edge_products/documents/edge-1");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.title).toBe("Widget");
  });

  // --- Import forwarding from follower ---

  it("import documents from follower node 3", async () => {
    const jsonl = [
      JSON.stringify({ id: "edge-2", title: "Gadget", price: 19.99 }),
      JSON.stringify({ id: "edge-3", title: "Gizmo", price: 29.99 }),
    ].join("\n");

    const res = await fetchMultiNode(3, "/collections/edge_products/documents/import?action=create", {
      method: "POST",
      body: jsonl,
      headers: { "Content-Type": "text/plain" },
    });
    expect(res.ok).toBe(true);
    const lines = (await res.text()).trim().split("\n");
    for (const line of lines) {
      const result = JSON.parse(line);
      expect(result.success).toBe(true);
    }
  });

  it("imported documents visible on leader node 1", async () => {
    const res = await fetchMultiNode(1, "/collections/edge_products/documents/edge-2");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.title).toBe("Gadget");
  });

  // --- Committed index convergence ---

  it("all nodes converge to the same committed index", async () => {
    const [s1, s2, s3] = await Promise.all([
      fetchMultiNodeRequest(1, "/status"),
      fetchMultiNodeRequest(2, "/status"),
      fetchMultiNodeRequest(3, "/status"),
    ]);
    expect(s1.ok).toBe(true);
    expect(s2.ok).toBe(true);
    expect(s3.ok).toBe(true);

    const [d1, d2, d3]: any[] = await Promise.all([
      s1.json(), s2.json(), s3.json(),
    ]);

    expect(d1.committed_index).toBe(d2.committed_index);
    expect(d1.committed_index).toBe(d3.committed_index);
    expect(d1.committed_index).toBeGreaterThan(0);
  });
});

// -- multi-restarted: verify metadata persists across cluster restart -------

describe(Phases.MULTI_RESTARTED, () => {
  it("alias persists after restart", async () => {
    const res = await fetchMultiNode(1, "/aliases/edge_products_alias");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.collection_name).toBe("edge_products");
  });

  it("preset persists after restart", async () => {
    const res = await fetchMultiNode(2, "/presets/edge_default_search");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.name).toBe("edge_default_search");
  });

  it("stopwords persist after restart", async () => {
    const res = await fetchMultiNode(3, "/stopwords/edge_english_basic");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.stopwords?.stopwords).toContain("the");
  });

  it("document persists after restart", async () => {
    const res = await fetchMultiNode(1, "/collections/edge_products/documents/edge-1");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.title).toBe("Widget");
  });
});

// -- multi-snapshot: verify metadata persists after leader snapshot ----------

describe(Phases.MULTI_SNAPSHOT, () => {
  it("alias persists after snapshot", async () => {
    const res = await fetchMultiNode(2, "/aliases/edge_products_alias");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.collection_name).toBe("edge_products");
  });

  it("preset persists after snapshot", async () => {
    const res = await fetchMultiNode(3, "/presets/edge_default_search");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.name).toBe("edge_default_search");
  });

  it("stopwords persist after snapshot", async () => {
    const res = await fetchMultiNode(1, "/stopwords/edge_english_basic");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.stopwords?.stopwords).toContain("the");
  });

  it("document persists after snapshot", async () => {
    const res = await fetchMultiNode(3, "/collections/edge_products/documents/edge-1");
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.title).toBe("Widget");
  });
});
