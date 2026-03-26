# Typesense Benchmark Results — Fork (v31-fork)

**Date:** 2026-03-26T16:35
**Build:** v31-fork (NuRaft runtime, WAL enabled, replay eliminated)
**Host:** DDEV Docker container
**Dataset:**
- products_se: 418,932 docs (130 fields)
- product_vehicle_fitments_se: 25,159,711 docs (3 fields, 2 JOINs)
- vehicles_se: 93,371 docs (40 fields)
- brands_se: 1,698 docs (13 fields)
- categories_se: 711 docs (41 fields)
- campaigns_se: 2 docs

**Startup time:** ~5.2 min (collection load from RocksDB, no replay)
**Method:** 5 runs per query, reporting min/median/max of `search_time_ms`

---

## 1. Basic Search (products_se — 418K docs)

| Query | Found | Min (ms) | Median (ms) | Max (ms) |
|-------|-------|----------|-------------|----------|
| q=* (match all) | 418,932 | 15 | 19 | 21 |
| q=* page 10 | 418,932 | 13 | 15 | 24 |
| q=oil filter | 348 | 5 | 5 | 8 |
| q=brake pad | 4,815 | 8 | 8 | 12 |
| q=chain kit | 6,214 | 5 | 6 | 9 |
| q=helmet (single word) | 2,068 | 2 | 3 | 6 |
| q=hf138 (part number) | 2 | 1 | 1 | 1 |

## 2. Filtered Search (products_se)

| Query | Found | Min (ms) | Median (ms) | Max (ms) |
|-------|-------|----------|-------------|----------|
| filter: image_exists:true | 396,666 | 15 | 16 | 17 |
| filter: brand_id=100 | 58 | 1 | 1 | 2 |
| filter: price_value:[100..500] | ~130K | 14 | 15 | 19 |
| filter: stock>0 && brand=100 | ~58 | 13 | 13 | 15 |
| filter: category_level1_ids:1 | 109,096 | 11 | 12 | 12 |
| filter: delivery_type:standard | 405,266 | 18 | 18 | 21 |
| text + filter (oil filter + stock>0) | 337 | 18 | 18 | 21 |

## 3. Sorted Search (products_se)

| Query | Found | Min (ms) | Median (ms) | Max (ms) |
|-------|-------|----------|-------------|----------|
| sort: price_value asc | 418,932 | 13 | 14 | 14 |
| sort: price_value desc | 418,932 | 13 | 13 | 15 |
| sort: stock_total desc + filter | 396,666 | 12 | 16 | 19 |
| sort: brand_name asc + price asc | 418,932 | 20 | 22 | 26 |

## 4. Faceted Search (products_se, ~396K matching docs)

| Query | Found | Min (ms) | Median (ms) | Max (ms) |
|-------|-------|----------|-------------|----------|
| 3 facets (brand, delivery, is_new) | 396,666 | 20 | 22 | 23 |
| 5 facets | 396,666 | 23 | 26 | 28 |
| 10 facets | 396,666 | 26 | 28 | 32 |
| 23 facets (full dashboard) | 396,666 | 398 | 419 | 424 |
| text + 5 facets (oil filter) | 348 | 6 | 6 | 6 |

## 5. Vehicles Search (vehicles_se — 93K docs)

| Query | Found | Min (ms) | Median (ms) | Max (ms) |
|-------|-------|----------|-------------|----------|
| q=* all vehicles | 93,371 | 3 | 3 | 4 |
| q=yamaha | 8,201 | 3 | 4 | 5 |
| q=honda cbr | 375 | 1 | 2 | 2 |
| filter: year>=2020 | 12,475 | 1 | 2 | 2 |
| facets: manufacturer, section, year | 93,371 | 3 | 4 | 4 |

## 6. JOIN: Products via Fitments (products for a vehicle)

Uses `$product_vehicle_fitments_se(vehicle_id:52018)` through 25M fitment docs.

| Query | Found | Min (ms) | Median (ms) | Max (ms) |
|-------|-------|----------|-------------|----------|
| JOIN: products for vehicle | 36 | 4 | 4 | 7 |
| JOIN: products for vehicle + text | 0 | 5 | 5 | 6 |
| JOIN: products for vehicle + facets | 36 | 4 | 4 | 5 |
| JOIN: products for vehicle + sort | 36 | 4 | 4 | 7 |

## 7. JOIN: Vehicles for a Product (reverse lookup)

Uses `$product_vehicle_fitments_se(variant_pid:2534810)` through 25M fitment docs.

| Query | Found | Min (ms) | Median (ms) | Max (ms) |
|-------|-------|----------|-------------|----------|
| JOIN: vehicles for product | 19 | 0 | 1 | 3 |
| JOIN: vehicles for product + facets | 19 | 0 | 1 | 1 |

## 8. Large Result Sets & Grouping (products_se)

| Query | Found | Min (ms) | Median (ms) | Max (ms) |
|-------|-------|----------|-------------|----------|
| per_page=100 | 396,666 | 30 | 35 | 39 |
| per_page=250 | 396,666 | 50 | 54 | 66 |
| group_by brand_name | 1,701 | 46 | 53 | 57 |
| group_by brand + stock>0 | 1,700 | 55 | 63 | 71 |

## 9. Fitments Direct Search (25M docs)

| Query | Found | Min (ms) | Median (ms) | Max (ms) |
|-------|-------|----------|-------------|----------|
| q=* all fitments | 25,159,711 | 2,334 | 2,370 | 2,430 |
| filter: vehicle_id=52018 | 74 | 0 | 0 | 0 |
| filter: variant_pid=2534810 | 19 | 0 | 0 | 2 |

## 10. Small Collections

| Query | Found | Min (ms) | Median (ms) | Max (ms) |
|-------|-------|----------|-------------|----------|
| categories: q=* | 711 | 1 | 1 | 1 |
| categories: text (motor) | 9 | 1 | 1 | 3 |
| brands: q=* | 1,698 | 0 | 0 | 2 |
| brands: text (bahco) | 1 | 0 | 0 | 0 |

---

## Summary

| Scenario | Typical Latency |
|----------|----------------|
| Simple text search | 1-8 ms |
| Filtered search (narrow) | 1-2 ms |
| Filtered search (broad, 400K match) | 12-18 ms |
| Sorted search | 13-22 ms |
| Faceted (3-10 facets) | 20-32 ms |
| Faceted (23 facets, dashboard) | ~420 ms |
| JOIN through 25M fitments | 1-7 ms |
| Reverse JOIN | 0-1 ms |
| Grouping | 50-63 ms |
| Large page (250 results) | ~54 ms |
| 25M collection q=* | ~2,370 ms |
| Small collection (<2K) | 0-1 ms |
