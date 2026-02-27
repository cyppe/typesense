import { describe, it, expect, beforeAll } from "bun:test";
import { TypesenseProcessManager } from "../src/manager";
import { Phases } from "../src/constants";
import { fetchSingleNode } from "../src/request";
import { join } from "path";
import { rmSync, mkdirSync } from "node:fs";

const sandboxRoot = process.env.TYPESENSE_API_TEST_SANDBOX_DIR
  ? join(process.env.TYPESENSE_API_TEST_SANDBOX_DIR, "migrate")
  : join(process.cwd(), "./data");

const v29DataDir = join(sandboxRoot, "v29-typesense-data");
const v29SnapshotDir = join(sandboxRoot, "snapshot", "v29-snapshot");
const migrationSourceBinaryPath = process.env.TYPESENSE_MIGRATION_SOURCE_BINARY_PATH;
const currentBinaryPath = process.env.TYPESENSE_BINARY_PATH;
const migrationSourceApiPortEnv = process.env.TYPESENSE_MIGRATION_SOURCE_API_PORT;
const migrationSourcePeeringPortEnv = process.env.TYPESENSE_MIGRATION_SOURCE_PEERING_PORT;
const V29_API_PORT = Number(migrationSourceApiPortEnv ?? "8109");
const V29_PEERING_PORT = Number(migrationSourcePeeringPortEnv ?? "8106");

const hasUsableMigrationBinary = Boolean(
  migrationSourceBinaryPath &&
  currentBinaryPath &&
  migrationSourceBinaryPath !== currentBinaryPath,
);

if (!hasUsableMigrationBinary) {
  console.warn(
    "Skipping migration tests: set TYPESENSE_MIGRATION_SOURCE_BINARY_PATH to a legacy binary different from TYPESENSE_BINARY_PATH.",
  );
}

const migrationDescribe = hasUsableMigrationBinary ? describe : describe.skip;

migrationDescribe(Phases.NO_PHASE, () => {
  beforeAll(() => {
    rmSync(v29DataDir, { recursive: true, force: true });
    mkdirSync(v29DataDir, { recursive: true });
    rmSync(v29SnapshotDir, { recursive: true, force: true });
    mkdirSync(v29SnapshotDir, { recursive: true });
  });

  it("create analytics rules in v29", async () => {
    let manager: TypesenseProcessManager;
    manager = new TypesenseProcessManager(v29DataDir, migrationSourceBinaryPath!);
    try {
      await manager.startSingleNode("", V29_API_PORT, V29_PEERING_PORT, "v29-snapshot-server");
      const createCollection = async(body: any) => {
        const res = await fetchSingleNode("/collections", { method: "POST", body: JSON.stringify(body) }, V29_API_PORT);
        expect(res.ok).toBe(true);
        return res.json();
      }

      await Promise.all([
        createCollection({
          name: "products",
          fields: [
            { name: "company_name", type: "string" },
            { name: "num_employees", type: "int32" },
            { name: "country", type: "string", facet: true },
            { name: "popularity", type: "int32", optional: true },
          ],
          default_sorting_field: "num_employees",
        }),
        createCollection({
          name: "products_1",
          fields: [
            { name: "company_name", type: "string" },
            { name: "num_employees", type: "int32" },
            { name: "country", type: "string", facet: true },
            { name: "popularity", type: "int32", optional: true },
          ],
          default_sorting_field: "num_employees",
        }),
        createCollection({
          name: "queries",
          fields: [
            { name: "q", type: "string" },
            { name: "count", type: "int32" },
          ],
        }),
        createCollection({
          name: "queries_1",
          fields: [
            { name: "q", type: "string" },
            { name: "count", type: "int32" },
          ],
        }),
        createCollection({
          name: "no_hits_queries",
          fields: [
            { name: "q", type: "string" },
            { name: "count", type: "int32" },
          ],
        }),
        createCollection({
          name: "no_hits_queries_1",
          fields: [
            { name: "q", type: "string" },
            { name: "count", type: "int32" },
          ],
        }),
        createCollection({
          name: "product_queries",
          fields: [
            { name: "q", type: "string" },
            { name: "count", type: "int32" },
          ],
        }),
        createCollection({
          name: "product_queries_1",
          fields: [
            { name: "q", type: "string" },
            { name: "count", type: "int32" },
          ],
        }),
      ]);

      await fetchSingleNode(
        "/collections/products/documents/import?action=upsert",
        {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body:
            "{" +
            '"company_name": "Typesense", "num_employees": 100, "country": "USA"' +
            "}\n{" +
            '"company_name": "Stark Industries", "num_employees": 1000, "country": "USA"' +
            "}\n{" +
            '"company_name": "Far Cry Industries", "num_employees": 200, "country": "USA"' +
            "}",
        },
        V29_API_PORT
      );

      const postLegacyRule = async (body: any) => {
        const res = await fetchSingleNode("/analytics/rules", { method: "POST", body: JSON.stringify(body) }, V29_API_PORT);
        expect(res.ok).toBe(true);
        return res.json();
      }

      await postLegacyRule({
        name: "product_queries_aggregation",
        type: "popular_queries",
        params: {
          source: { collections: ["products"] },
          destination: { collection: "queries" },
          limit: 1000,
        },
      });

      await postLegacyRule({
        name: "product_queries_aggregation_1",
        type: "popular_queries",
        params: {
          source: {
            collections: ["products"],
            enable_auto_aggregation: false,
            events: [{ type: "search", name: "products_search_event" }],
          },
          destination: { collection: "product_queries" },
          limit: 1000,
        },
      });

      await postLegacyRule({
        name: "product_no_hits",
        type: "nohits_queries",
        params: {
          source: { collections: ["products"] },
          destination: { collection: "no_hits_queries" },
          limit: 1000,
        },
      });

      await postLegacyRule({
        name: "product_clicks",
        type: "counter",
        params: {
          source: {
            collections: ["products"],
            events: [{ type: "click", weight: 1, name: "products_click_event" }],
          },
          destination: { collection: "products", counter_field: "popularity" },
        },
      });

      await postLegacyRule({
        name: "product_queries_aggregation_2",
        type: "popular_queries",
        params: {
          source: { collections: ["products", "products_1"] },
          destination: { collection: "queries_1" },
          limit: 1000,
        },
      });

      await postLegacyRule({
        name: "product_queries_aggregation_3",
        type: "popular_queries",
        params: {
          source: {
            collections: ["products", "products_1"],
            enable_auto_aggregation: false,
            events: [
              { type: "search", name: "products_search_event_1" },
              { type: "search", name: "products_search_event_2" },
            ],
          },
          destination: { collection: "product_queries_1" },
          limit: 1000,
        },
      });

      await postLegacyRule({
        name: "product_no_hits_1",
        type: "nohits_queries",
        params: {
          source: { collections: ["products", "products_1"] },
          destination: { collection: "no_hits_queries_1" },
          limit: 1000,
        },
      });

      await postLegacyRule({
        name: "product_clicks_1",
        type: "counter",
        params: {
          source: {
            collections: ["products", "products_1"],
            events: [
              { type: "click", weight: 1, name: "products_click_event_1" },
              { type: "conversion", weight: 2, name: "products_conversion_event" },
            ],
          },
          destination: { collection: "products_1", counter_field: "popularity" },
        },
      });
      await manager.createSnapshot(V29_API_PORT, v29SnapshotDir);
    } finally {
      await manager.shutdown();
    }
  });

  it("create legacy collection synonyms in v29", async () => {
    let manager: TypesenseProcessManager;
    manager = new TypesenseProcessManager(v29DataDir, migrationSourceBinaryPath!);
    await manager.startSingleNode("", V29_API_PORT, V29_PEERING_PORT, "v29-snapshot-server");
    try {
      // v30 moved synonyms to top-level synonym sets.
      // This intentionally seeds legacy collection-scoped synonyms so we can
      // validate upgrade migration behavior and downgrade compatibility.
      const coat_synonyms = await fetchSingleNode(
        "/collections/products/synonyms/coat-synonyms",
        {
          method: "PUT",
          body: JSON.stringify({ synonyms: ["blazer", "coat", "jacket"] }),
        },
        V29_API_PORT
      );
      expect(coat_synonyms.ok).toBe(true);

      const smart_phone_synonyms = await fetchSingleNode(
        "/collections/products/synonyms/smart-phone-synonyms",
        {
          method: "PUT",
          body: JSON.stringify({ root: "smart phone", synonyms: ["iphone", "android"] }),
        },
        V29_API_PORT
      );
      expect(smart_phone_synonyms.ok).toBe(true);
    } finally {
      await manager.shutdown();
    }
  });

  it("create curation sets in v29", async () => {
    let manager: TypesenseProcessManager;
    manager = new TypesenseProcessManager(v29DataDir, migrationSourceBinaryPath!);
    await manager.startSingleNode("", V29_API_PORT, V29_PEERING_PORT, "v29-snapshot-server");
    try {
      const customize_apple_curation = await fetchSingleNode(
        "/collections/products/overrides/customize-apple",
        {
          method: "PUT",
          body: JSON.stringify({
            rule: {
              query: "apple",
              match: "exact"
            },
            includes: [
              {id: "422", position: 1},
              {id: "54", position: 2}
            ],
            excludes: [
              {id: "287"}
            ]
          }),
        },
        V29_API_PORT
      );
      expect(customize_apple_curation.ok).toBe(true);
      const brand_filter_curation = await fetchSingleNode(
        "/collections/products/overrides/brand-filter",
        {
          method: "PUT",
          body: JSON.stringify({
            rule: {
              query: "{brand} phone",
              match: "contains"
            },
            filter_by: "brand:={brand}",
            remove_matched_tokens: true
          }),
        },
        V29_API_PORT
      );
      expect(brand_filter_curation.ok).toBe(true);
      const dynamic_sort_curation = await fetchSingleNode(
        "/collections/products/overrides/dynamic-sort",
        {
          method: "PUT",
          body: JSON.stringify({
            rule: {
              query: "{store}",
              match: "exact"
            },
            remove_matched_tokens: true,
            sort_by: "sales.{store}:desc, inventory.{store}:desc"
          }),
        },
        V29_API_PORT
      );
      expect(dynamic_sort_curation.ok).toBe(true);
      await manager.createSnapshot(V29_API_PORT, v29SnapshotDir);
    } finally {
      await manager.shutdown();
    }
  });

  it("validate analytics rules", async () => {
    let manager: TypesenseProcessManager;
    manager = new TypesenseProcessManager(v29SnapshotDir);
    try {
      await manager.startSingleNode("", V29_API_PORT, V29_PEERING_PORT, "v29-snapshot-server");
       const res = await fetchSingleNode("/analytics/rules", { method: "GET" }, V29_API_PORT);
      const data: any = await res.json();
      const expected_rules = [
        {
          collection: "products_1",
          event_type: "conversion",
          name: "products_conversion_event_products_1",
          params: {
            counter_field: "popularity",
            destination_collection: "products_1",
            weight: 2,
          },
          rule_tag: "product_clicks_1",
          type: "counter",
        }, {
          collection: "products",
          event_type: "conversion",
          name: "products_conversion_event_products",
          params: {
            counter_field: "popularity",
            destination_collection: "products_1",
            weight: 2,
          },
          rule_tag: "product_clicks_1",
          type: "counter",
        }, {
          collection: "products_1",
          event_type: "click",
          name: "products_click_event_1_products_1",
          params: {
            counter_field: "popularity",
            destination_collection: "products_1",
            weight: 1,
          },
          rule_tag: "product_clicks_1",
          type: "counter",
        }, {
          collection: "products",
          event_type: "click",
          name: "products_click_event_1_products",
          params: {
            counter_field: "popularity",
            destination_collection: "products_1",
            weight: 1,
          },
          rule_tag: "product_clicks_1",
          type: "counter",
        }, {
          collection: "products",
          event_type: "click",
          name: "products_click_event",
          params: {
            counter_field: "popularity",
            destination_collection: "products",
            weight: 1,
          },
          rule_tag: "product_clicks",
          type: "counter",
        }, {
          collection: "products_1",
          event_type: "search",
          name: "product_queries_aggregation_2_products_1",
          params: {
            capture_search_requests: true,
            destination_collection: "queries_1",
            expand_query: false,
            limit: 1000,
          },
          rule_tag: "product_queries_aggregation_2",
          type: "popular_queries",
        }, {
          collection: "products",
          event_type: "search",
          name: "product_queries_aggregation_2_products",
          params: {
            capture_search_requests: true,
            destination_collection: "queries_1",
            expand_query: false,
            limit: 1000,
          },
          rule_tag: "product_queries_aggregation_2",
          type: "popular_queries",
        }, {
          collection: "products",
          event_type: "search",
          name: "product_queries_aggregation",
          params: {
            capture_search_requests: true,
            destination_collection: "queries",
            expand_query: false,
            limit: 1000,
          },
          rule_tag: "product_queries_aggregation",
          type: "popular_queries",
        }, {
          collection: "products_1",
          event_type: "search",
          name: "product_no_hits_1_products_1",
          params: {
            capture_search_requests: true,
            destination_collection: "no_hits_queries_1",
            expand_query: false,
            limit: 1000,
          },
          rule_tag: "product_no_hits_1",
          type: "nohits_queries",
        }, {
          collection: "products",
          event_type: "search",
          name: "products_search_event",
          params: {
            capture_search_requests: false,
            destination_collection: "product_queries",
            expand_query: false,
            limit: 1000,
          },
          rule_tag: "product_queries_aggregation_1",
          type: "popular_queries",
        }, {
          collection: "products",
          event_type: "search",
          name: "product_no_hits_1_products",
          params: {
            capture_search_requests: true,
            destination_collection: "no_hits_queries_1",
            expand_query: false,
            limit: 1000,
          },
          rule_tag: "product_no_hits_1",
          type: "nohits_queries",
        }, {
          collection: "products",
          event_type: "search",
          name: "product_no_hits",
          params: {
            capture_search_requests: true,
            destination_collection: "no_hits_queries",
            expand_query: false,
            limit: 1000,
          },
          rule_tag: "product_no_hits",
          type: "nohits_queries",
        }
      ];

      for (const rule of expected_rules) {
        expect(data.find((r: any) => r.name === rule.name)).toBeDefined();
        expect(data.find((r: any) => r.name === rule.name).params).toEqual(rule.params);
        expect(data.find((r: any) => r.name === rule.name).rule_tag).toEqual(rule.rule_tag);
        expect(data.find((r: any) => r.name === rule.name).type).toEqual(rule.type);
        expect(data.find((r: any) => r.name === rule.name).event_type).toEqual(rule.event_type);
        expect(data.find((r: any) => r.name === rule.name).collection).toEqual(rule.collection);
      }
      const delete_res = await fetchSingleNode("/analytics/rules/products_conversion_event_products_1", { method: "DELETE" }, V29_API_PORT);
      expect(delete_res.ok).toBe(true);
      const put_res = await fetchSingleNode("/analytics/rules/products_conversion_event_products", { method: "PUT", body: JSON.stringify( {
        collection: "products",
        event_type: "conversion",
        name: "products_conversion_event_products",
        params: {
          counter_field: "num_employees",
          destination_collection: "products_1",
          weight: 2,
        },
        rule_tag: "product_clicks_1",
        type: "counter",
      }) }, V29_API_PORT);
      expect(put_res.ok).toBe(true);
    } finally {
      await manager.shutdown();
    }
  });

  it("validate migrated synonym sets", async () => {
    let manager: TypesenseProcessManager;
    manager = new TypesenseProcessManager(v29SnapshotDir);
    try {
    await manager.startSingleNode("", V29_API_PORT, V29_PEERING_PORT, "v29-snapshot-server");
    const res = await fetchSingleNode("/synonym_sets", { method: "GET" }, V29_API_PORT);
    const data: any = await res.json();
    expect(data.length).toEqual(1);
    expect(data[0].name).toEqual("products_synonyms_index");
    expect(data[0].items[0].id).toEqual("coat-synonyms");
    expect(data[0].items[0].synonyms).toEqual(["blazer", "coat", "jacket"]);
    expect(data[0].items[1].id).toEqual("smart-phone-synonyms");
    expect(data[0].items[1].synonyms).toEqual(["iphone", "android"]);
    expect(data[0].items[1].root).toEqual("smart phone");
    const delete_res = await fetchSingleNode("/synonym_sets/products_synonyms_index", { method: "DELETE" }, V29_API_PORT);
    expect(delete_res.ok).toBe(true);
    const put_res = await fetchSingleNode("/synonym_sets/products_synonyms_index", { method: "PUT", body: JSON.stringify({ items: [{ id: "coat-synonyms", synonyms: ["blazer", "coat"] }, { id: "smart-phone-synonyms", root: "smart phone", synonyms: ["iphone"] }] }) }, V29_API_PORT);
    expect(put_res.ok).toBe(true);
    } finally {
      await manager.shutdown();
    }
  });

  it("validate curation sets", async () => {
    let manager: TypesenseProcessManager;
    manager = new TypesenseProcessManager(v29SnapshotDir);
    try {
      await manager.startSingleNode("", V29_API_PORT, V29_PEERING_PORT, "v29-snapshot-server");
      const res = await fetchSingleNode("/curation_sets", { method: "GET" }, V29_API_PORT);
      const data: any = await res.json();
      expect(data.length).toEqual(1);
      expect(data[0].name).toEqual("products_curations_index");
      expect(data[0].items[0].id).toEqual("brand-filter");
      expect(data[0].items[0].rule.query).toEqual("{brand} phone");
      expect(data[0].items[0].rule.match).toEqual("contains");
      expect(data[0].items[0].filter_by).toEqual("brand:={brand}");
      expect(data[0].items[0].remove_matched_tokens).toEqual(true);
      expect(data[0].items[1].id).toEqual("customize-apple");
      expect(data[0].items[1].rule.query).toEqual("apple");
      expect(data[0].items[1].rule.match).toEqual("exact");
      expect(data[0].items[1].includes).toEqual([{ id: "422", position: 1 }, { id: "54", position: 2 }]);
      expect(data[0].items[1].excludes).toEqual([{ id: "287" }]);
      expect(data[0].items[2].id).toEqual("dynamic-sort");
      expect(data[0].items[2].rule.query).toEqual("{store}");
      expect(data[0].items[2].rule.match).toEqual("exact");
      expect(data[0].items[2].remove_matched_tokens).toEqual(true);
      expect(data[0].items[2].sort_by).toEqual("sales.{store}:desc, inventory.{store}:desc");

      const delete_res = await fetchSingleNode("/curation_sets/products_curations_index", { method: "DELETE" }, V29_API_PORT);
      expect(delete_res.ok).toBe(true);
      const put_res = await fetchSingleNode("/curation_sets/products_curations_index", { method: "PUT", body: JSON.stringify({ items: [{ id: "brand-filter", rule: { query: "{brand} phone", match: "contains" }, filter_by: "brand:={brand}", remove_matched_tokens: true }, { id: "customize-apple", rule: { query: "apple", match: "exact" }, includes: [{ id: "422", position: 1 }, { id: "54", position: 2 }], excludes: [{ id: "287" }] }, { id: "dynamic-sort", rule: { query: "{store}", match: "exact" }, remove_matched_tokens: true, sort_by: "sales.{store}:desc, inventory.{store}:desc" }] }) }, V29_API_PORT);
      expect(put_res.ok).toBe(true);
    } finally {
      await manager.shutdown();
    }
  });
});
