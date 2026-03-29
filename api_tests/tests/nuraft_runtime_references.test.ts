import { describe, expect, it } from "bun:test";
import { Phases } from "../src/constants";
import { fetchMultiNode, fetchMultiNodeRequest } from "../src/request";

// ---------------------------------------------------------------------------
// Validates that referenced collections (JOINs) work correctly across a
// 3-node NuRaft cluster, including after restart and snapshot restore.
//
// Regression test for: https://github.com/typesense/typesense/issues/2857
// The upstream bug: $REFERENCED_INS key in RocksDB was only written at
// shutdown, causing stale reverse-reference maps after snapshot restore.
// Our fork always recomputes from collection schemas on load.
// ---------------------------------------------------------------------------

// -- multi-fresh: create referenced collections and test JOINs ---------------

describe(Phases.MULTI_FRESH, () => {
  it("create categories collection (referenced target)", async () => {
    const res = await fetchMultiNode(1, "/collections", {
      method: "POST",
      body: JSON.stringify({
        name: "ref_categories",
        fields: [
          { name: "category_id", type: "string", facet: true },
          { name: "category_name", type: "string" },
        ],
      }),
    });
    expect(res.ok).toBe(true);
  });

  it("create products collection with reference to categories", async () => {
    const res = await fetchMultiNode(2, "/collections", {
      method: "POST",
      body: JSON.stringify({
        name: "ref_products",
        fields: [
          { name: "product_name", type: "string" },
          { name: "price", type: "int32", sort: true },
          { name: "category_id", type: "string", reference: "ref_categories.category_id" },
        ],
      }),
    });
    expect(res.ok).toBe(true);
    const body: any = await res.json();
    expect(body.name).toBe("ref_products");
  });

  it("insert category documents", async () => {
    const categories = [
      { id: "1", category_id: "cat-electronics", category_name: "Electronics" },
      { id: "2", category_id: "cat-books", category_name: "Books" },
    ];

    for (const cat of categories) {
      const res = await fetchMultiNode(1, "/collections/ref_categories/documents", {
        method: "POST",
        body: JSON.stringify(cat),
      });
      expect(res.ok).toBe(true);
    }
  });

  it("insert product documents referencing categories", async () => {
    const products = [
      { id: "1", product_name: "Laptop", price: 999, category_id: "cat-electronics" },
      { id: "2", product_name: "Phone", price: 699, category_id: "cat-electronics" },
      { id: "3", product_name: "Novel", price: 15, category_id: "cat-books" },
    ];

    for (const prod of products) {
      const res = await fetchMultiNode(3, "/collections/ref_products/documents", {
        method: "POST",
        body: JSON.stringify(prod),
      });
      expect(res.ok).toBe(true);
    }
  });

  it("JOIN query works on all 3 nodes", async () => {
    for (const node of [1, 2, 3]) {
      const res = await fetchMultiNode(node, `/collections/ref_categories/documents/search?q=*&query_by=category_name&filter_by=$ref_products(price:>100)&include_fields=category_id,category_name,$ref_products(product_name)`);
      expect(res.ok).toBe(true);
      const body: any = await res.json();
      expect(body.found).toBe(1); // only Electronics has products > 100
      expect(body.hits[0].document.category_name).toBe("Electronics");
    }
  });

  it("reverse JOIN query works on all 3 nodes", async () => {
    for (const node of [1, 2, 3]) {
      const res = await fetchMultiNode(node, `/collections/ref_products/documents/search?q=*&query_by=product_name&filter_by=category_id:=cat-electronics&include_fields=product_name,price,$ref_categories(category_name)&sort_by=price:desc`);
      expect(res.ok).toBe(true);
      const body: any = await res.json();
      expect(body.found).toBe(2); // Laptop and Phone
      expect(body.hits[0].document.product_name).toBe("Laptop");
      expect(body.hits[1].document.product_name).toBe("Phone");
    }
  });
});

// -- multi-restarted: JOINs survive cluster restart --------------------------

describe(Phases.MULTI_RESTARTED, () => {
  it("referenced collections exist after cluster restart", async () => {
    const catRes = await fetchMultiNode(1, "/collections/ref_categories");
    expect(catRes.ok).toBe(true);
    const catBody: any = await catRes.json();
    expect(catBody.num_documents).toBe(2);

    const prodRes = await fetchMultiNode(2, "/collections/ref_products");
    expect(prodRes.ok).toBe(true);
    const prodBody: any = await prodRes.json();
    expect(prodBody.num_documents).toBe(3);

    // Verify reference field is in schema
    const refField = prodBody.fields.find((f: any) => f.name === "category_id");
    expect(refField).toBeDefined();
    expect(refField.reference).toBe("ref_categories.category_id");
  });

  it("JOIN query works on all nodes after restart", async () => {
    for (const node of [1, 2, 3]) {
      const res = await fetchMultiNode(node, `/collections/ref_categories/documents/search?q=*&query_by=category_name&filter_by=$ref_products(price:>100)&include_fields=category_id,category_name,$ref_products(product_name)`);
      expect(res.ok).toBe(true);
      const body: any = await res.json();
      expect(body.found).toBe(1);
      expect(body.hits[0].document.category_name).toBe("Electronics");
    }
  });
});

// -- multi-snapshot: JOINs survive snapshot + restart (the #2857 scenario) ---

describe(Phases.MULTI_SNAPSHOT, () => {
  it("referenced collections exist after snapshot restore", async () => {
    const catRes = await fetchMultiNode(3, "/collections/ref_categories");
    expect(catRes.ok).toBe(true);
    const catBody: any = await catRes.json();
    expect(catBody.num_documents).toBe(2);

    const prodRes = await fetchMultiNode(1, "/collections/ref_products");
    expect(prodRes.ok).toBe(true);
    const prodBody: any = await prodRes.json();
    expect(prodBody.num_documents).toBe(3);
  });

  it("JOIN query works on all nodes after snapshot restore (#2857 regression)", async () => {
    // This is the exact scenario from github.com/typesense/typesense/issues/2857:
    // After a snapshot-based node restore, the in-memory reference index must be
    // correctly rebuilt so JOINs work. Upstream's $REFERENCED_INS key was stale;
    // our fork always recomputes from schemas.
    for (const node of [1, 2, 3]) {
      const res = await fetchMultiNode(node, `/collections/ref_categories/documents/search?q=*&query_by=category_name&filter_by=$ref_products(price:>100)&include_fields=category_id,category_name,$ref_products(product_name)`);
      expect(res.ok).toBe(true);
      const body: any = await res.json();
      expect(body.found).toBe(1);
      expect(body.hits[0].document.category_name).toBe("Electronics");
    }
  });

  it("reverse JOIN query works on all nodes after snapshot restore", async () => {
    for (const node of [1, 2, 3]) {
      const res = await fetchMultiNode(node, `/collections/ref_products/documents/search?q=*&query_by=product_name&filter_by=category_id:=cat-books&include_fields=product_name,$ref_categories(category_name)`);
      expect(res.ok).toBe(true);
      const body: any = await res.json();
      expect(body.found).toBe(1);
      expect(body.hits[0].document.product_name).toBe("Novel");
    }
  });
});
