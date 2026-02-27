import type { Params } from "k6/http";
import type { Options } from "k6/options";

import { sleep } from "k6";
import http from "k6/http";
import { Gauge } from "k6/metrics";

// System metrics from /metrics.json
const cpuActive = new Gauge("ts_cpu_active_pct");
const memoryUsed = new Gauge("ts_memory_used_bytes");
const memoryActive = new Gauge("ts_memory_active_bytes");
const memoryAllocated = new Gauge("ts_memory_allocated_bytes");
const memoryFragmentation = new Gauge("ts_memory_fragmentation_ratio");
const diskUsed = new Gauge("ts_disk_used_bytes");

// RocksDB metrics from /debug?rocksdb_stats=true
const rocksdbMemtableSize = new Gauge("rocksdb_memtable_size_bytes");
const rocksdbBlockCacheUsage = new Gauge("rocksdb_block_cache_usage_bytes");
const rocksdbBlockCacheCapacity = new Gauge("rocksdb_block_cache_capacity_bytes");
const rocksdbLiveDataSize = new Gauge("rocksdb_live_data_size_bytes");
const rocksdbRunningCompactions = new Gauge("rocksdb_running_compactions");
const rocksdbRunningFlushes = new Gauge("rocksdb_running_flushes");
const rocksdbCompactionPending = new Gauge("rocksdb_compaction_pending");
const rocksdbL0Files = new Gauge("rocksdb_l0_files");
const rocksdbWriteStopped = new Gauge("rocksdb_write_stopped");
const rocksdbDelayedWriteRate = new Gauge("rocksdb_delayed_write_rate");
const rocksdbEstimateNumKeys = new Gauge("rocksdb_estimate_num_keys");
const rocksdbTableReadersMem = new Gauge("rocksdb_table_readers_mem_bytes");

// Extended memory/system metrics from /metrics.json
const memoryMapped = new Gauge("ts_memory_mapped_bytes");
const memoryMetadata = new Gauge("ts_memory_metadata_bytes");
const memoryResident = new Gauge("ts_memory_resident_bytes");
const memoryRetained = new Gauge("ts_memory_retained_bytes");
const networkSent = new Gauge("ts_network_sent_bytes");
const networkReceived = new Gauge("ts_network_received_bytes");
const diskTotal = new Gauge("ts_disk_total_bytes");

// API stats from /stats.json
const importLatency = new Gauge("ts_import_latency_ms");
const importRps = new Gauge("ts_import_rps");
const searchLatencyP95 = new Gauge("ts_search_latency_p95_ms");
const searchLatencyP99 = new Gauge("ts_search_latency_p99_ms");
const writeLatencyP95 = new Gauge("ts_write_latency_p95_ms");
const writeLatencyP99 = new Gauge("ts_write_latency_p99_ms");
const importLatencyP95 = new Gauge("ts_import_latency_p95_ms");
const importLatencyP99 = new Gauge("ts_import_latency_p99_ms");
const totalRps = new Gauge("ts_total_rps");
const overloadedRps = new Gauge("ts_overloaded_rps");
const pendingWriteBatches = new Gauge("ts_pending_write_batches");

// Additional RocksDB metrics from /debug
const rocksdbBlockCachePinned = new Gauge("rocksdb_block_cache_pinned_usage_bytes");
const rocksdbMemTableFlushPending = new Gauge("rocksdb_mem_table_flush_pending");
const rocksdbBlockCacheHitCount = new Gauge("rocksdb_block_cache_hit_count");
const rocksdbBlockCacheMissCount = new Gauge("rocksdb_block_cache_miss_count");
const rocksdbBlockCacheHitRatio = new Gauge("rocksdb_block_cache_hit_ratio");
const rocksdbBloomFilterUsefulCount = new Gauge("rocksdb_bloom_filter_useful_count");
const rocksdbBloomFilterPositiveCount = new Gauge("rocksdb_bloom_filter_positive_count");
const rocksdbBloomFilterTruePositiveCount = new Gauge("rocksdb_bloom_filter_true_positive_count");
const rocksdbBloomFilterTruePositiveRatio = new Gauge("rocksdb_bloom_filter_true_positive_ratio");
const rocksdbStallMicros = new Gauge("rocksdb_stall_micros");
const rocksdbCompactionReadBytes = new Gauge("rocksdb_compaction_read_bytes");
const rocksdbCompactionWriteBytes = new Gauge("rocksdb_compaction_write_bytes");
const rocksdbCompactionWriteAmp = new Gauge("rocksdb_compaction_write_amplification");

// Collection interval
const INTERVAL_SECONDS = parseInt(__ENV.METRICS_INTERVAL ?? "2");

export const options: Options = {
  scenarios: {
    metrics_collection: {
      executor: "constant-vus",
      vus: 1,
      duration: __ENV.METRICS_DURATION ?? "5m",
    },
  },
  tags: {
    commitHash: __ENV.COMMIT_HASH ?? "unknown",
    testType: "metrics",
  },
};

const ROCKSDB_COUNTER_ALIASES = {
  blockCacheHit: ["rocksdb.block.cache.hit", "block_cache_hit"],
  blockCacheMiss: ["rocksdb.block.cache.miss", "block_cache_miss"],
  bloomFilterUseful: ["rocksdb.bloom.filter.useful", "bloom_filter_useful"],
  bloomFilterPositive: ["rocksdb.bloom.filter.full.positive", "bloom_filter_full_positive"],
  bloomFilterTruePositive: [
    "rocksdb.bloom.filter.full.true.positive",
    "bloom_filter_full_true_positive",
  ],
  stallMicros: ["rocksdb.stall.micros", "stall_micros"],
  compactReadBytes: ["rocksdb.compact.read.bytes", "compact_read_bytes"],
  compactWriteBytes: ["rocksdb.compact.write.bytes", "compact_write_bytes"],
} as const;

function toNumber(value: unknown): number | null {
  if (typeof value === "number" && Number.isFinite(value)) {
    return value;
  }

  if (typeof value === "string") {
    const parsed = parseFloat(value);
    if (Number.isFinite(parsed)) {
      return parsed;
    }
  }

  return null;
}

function getNestedNumber(record: Record<string, unknown>, key: string, nestedKey: string): number | null {
  const raw = record[key];
  if (raw && typeof raw === "object") {
    return toNumber((raw as Record<string, unknown>)[nestedKey]);
  }
  return null;
}

function extractTickerCount(statistics: string, aliases: readonly string[]): number | null {
  for (const line of statistics.split("\n")) {
    const lowerLine = line.toLowerCase();
    if (!lowerLine.includes("count")) {
      continue;
    }

    if (!aliases.some((alias) => lowerLine.includes(alias))) {
      continue;
    }

    const match = /:\s*([-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)/.exec(line);
    if (!match) {
      continue;
    }

    const value = parseFloat(match[1]);
    if (Number.isFinite(value)) {
      return value;
    }
  }

  return null;
}

export default function () {
  const baseUrl = `http://${__ENV.HOST}:${__ENV.PORT}`;
  const apiKey = __ENV.API_KEY ?? "xyz";

  const params: Params = {
    headers: {
      "X-TYPESENSE-API-KEY": apiKey,
    },
    timeout: "10000",
  };

  const tags = {
    commitHash: __ENV.COMMIT_HASH ?? "unknown",
  };

  // 1. Collect /metrics.json
  const metricsRes = http.get(`${baseUrl}/metrics.json`, params);
  if (metricsRes.status === 200 && typeof metricsRes.body === "string") {
    try {
      const m = JSON.parse(metricsRes.body) as Record<string, string>;
      const cpuValue = toNumber(m["system_cpu_active_percentage"]);
      if (cpuValue !== null) {
        cpuActive.add(cpuValue, tags);
      }

      const memoryUsedValue = toNumber(m["system_memory_used_bytes"]);
      if (memoryUsedValue !== null) {
        memoryUsed.add(memoryUsedValue, tags);
      }

      const memoryActiveValue = toNumber(m["typesense_memory_active_bytes"]);
      if (memoryActiveValue !== null) {
        memoryActive.add(memoryActiveValue, tags);
      }

      const memoryAllocatedValue = toNumber(m["typesense_memory_allocated_bytes"]);
      if (memoryAllocatedValue !== null) {
        memoryAllocated.add(memoryAllocatedValue, tags);
      }

      const memoryFragmentationValue = toNumber(m["typesense_memory_fragmentation_ratio"]);
      if (memoryFragmentationValue !== null) {
        memoryFragmentation.add(memoryFragmentationValue, tags);
      }

      const diskUsedValue = toNumber(m["system_disk_used_bytes"]);
      if (diskUsedValue !== null) {
        diskUsed.add(diskUsedValue, tags);
      }

      const memoryMappedValue = toNumber(m["typesense_memory_mapped_bytes"]);
      if (memoryMappedValue !== null) {
        memoryMapped.add(memoryMappedValue, tags);
      }

      const memoryMetadataValue = toNumber(m["typesense_memory_metadata_bytes"]);
      if (memoryMetadataValue !== null) {
        memoryMetadata.add(memoryMetadataValue, tags);
      }

      const memoryResidentValue = toNumber(m["typesense_memory_resident_bytes"]);
      if (memoryResidentValue !== null) {
        memoryResident.add(memoryResidentValue, tags);
      }

      const memoryRetainedValue = toNumber(m["typesense_memory_retained_bytes"]);
      if (memoryRetainedValue !== null) {
        memoryRetained.add(memoryRetainedValue, tags);
      }

      const networkSentValue = toNumber(m["system_network_sent_bytes"]);
      if (networkSentValue !== null) {
        networkSent.add(networkSentValue, tags);
      }

      const networkReceivedValue = toNumber(m["system_network_received_bytes"]);
      if (networkReceivedValue !== null) {
        networkReceived.add(networkReceivedValue, tags);
      }

      const diskTotalValue = toNumber(m["system_disk_total_bytes"]);
      if (diskTotalValue !== null) {
        diskTotal.add(diskTotalValue, tags);
      }
    } catch { /* ignore */ }
  }

  // 2. Collect /stats.json
  const statsRes = http.get(`${baseUrl}/stats.json`, params);
  if (statsRes.status === 200 && typeof statsRes.body === "string") {
    try {
      const s = JSON.parse(statsRes.body) as Record<string, unknown>;

      const latencyMs = s["latency_ms"] as Record<string, unknown> | undefined;
      const rps = s["requests_per_second"] as Record<string, unknown> | undefined;

      // Find import endpoint latency
      let importLatencyValue = toNumber(s["import_latency_ms"]);
      if (importLatencyValue === null && latencyMs) {
        for (const [endpoint, latency] of Object.entries(latencyMs)) {
          if (endpoint.includes("/documents/import")) {
            importLatencyValue = toNumber(latency);
            break;
          }
        }
      }
      if (importLatencyValue !== null) {
        importLatency.add(importLatencyValue, tags);
      }

      let importRpsValue = toNumber(s["import_requests_per_second"]);
      if (importRpsValue === null && rps) {
        for (const [endpoint, r] of Object.entries(rps)) {
          if (endpoint.includes("/documents/import")) {
            importRpsValue = toNumber(r);
            break;
          }
        }
      }
      if (importRpsValue !== null) {
        importRps.add(importRpsValue, tags);
      }

      // Latency percentiles (supports both flat and nested key formats)
      const searchP95 = toNumber(s["search_95Percentile_latency_ms"]) ??
        getNestedNumber(s, "search_latency_ms", "p95");
      if (searchP95 !== null) {
        searchLatencyP95.add(searchP95, tags);
      }

      const searchP99 = toNumber(s["search_99Percentile_latency_ms"]) ??
        getNestedNumber(s, "search_latency_ms", "p99");
      if (searchP99 !== null) {
        searchLatencyP99.add(searchP99, tags);
      }

      const writeP95 = toNumber(s["write_95Percentile_latency_ms"]) ??
        getNestedNumber(s, "write_latency_ms", "p95");
      if (writeP95 !== null) {
        writeLatencyP95.add(writeP95, tags);
      }

      const writeP99 = toNumber(s["write_99Percentile_latency_ms"]) ??
        getNestedNumber(s, "write_latency_ms", "p99");
      if (writeP99 !== null) {
        writeLatencyP99.add(writeP99, tags);
      }

      const importP95 = toNumber(s["import_95Percentile_latency_ms"]);
      if (importP95 !== null) {
        importLatencyP95.add(importP95, tags);
      }

      const importP99 = toNumber(s["import_99Percentile_latency_ms"]);
      if (importP99 !== null) {
        importLatencyP99.add(importP99, tags);
      }

      // Aggregate stats
      const totalRpsValue = toNumber(s["total_requests_per_second"]);
      if (totalRpsValue !== null) {
        totalRps.add(totalRpsValue, tags);
      }

      const overloadedRpsValue = toNumber(s["overloaded_requests_per_second"]);
      if (overloadedRpsValue !== null) {
        overloadedRps.add(overloadedRpsValue, tags);
      }

      const pendingBatchesValue = toNumber(s["pending_write_batches"]);
      if (pendingBatchesValue !== null) {
        pendingWriteBatches.add(pendingBatchesValue, tags);
      }
    } catch { /* ignore */ }
  }

  // 3. Collect /debug?rocksdb_stats=true
  const debugRes = http.get(`${baseUrl}/debug?rocksdb_stats=true`, params);
  if (debugRes.status === 200 && typeof debugRes.body === "string") {
    try {
      const d = JSON.parse(debugRes.body) as {
        rocksdb_properties?: Record<string, unknown>;
        rocksdb_statistics?: string;
      };

      const props = d.rocksdb_properties;
      if (props) {
        const memtableSize = toNumber(props["cur_size_all_mem_tables"]);
        if (memtableSize !== null) {
          rocksdbMemtableSize.add(memtableSize, tags);
        }

        const blockCacheUsage = toNumber(props["block_cache_usage"]);
        if (blockCacheUsage !== null) {
          rocksdbBlockCacheUsage.add(blockCacheUsage, tags);
        }

        const blockCacheCapacity = toNumber(props["block_cache_capacity"]);
        if (blockCacheCapacity !== null) {
          rocksdbBlockCacheCapacity.add(blockCacheCapacity, tags);
        }

        const liveDataSize = toNumber(props["estimate_live_data_size"]);
        if (liveDataSize !== null) {
          rocksdbLiveDataSize.add(liveDataSize, tags);
        }

        const runningCompactions = toNumber(props["num_running_compactions"]);
        if (runningCompactions !== null) {
          rocksdbRunningCompactions.add(runningCompactions, tags);
        }

        const runningFlushes = toNumber(props["num_running_flushes"]);
        if (runningFlushes !== null) {
          rocksdbRunningFlushes.add(runningFlushes, tags);
        }

        const compactionPending = toNumber(props["compaction_pending"]);
        if (compactionPending !== null) {
          rocksdbCompactionPending.add(compactionPending, tags);
        }

        const l0Files = toNumber(props["num_files_at_level0"]);
        if (l0Files !== null) {
          rocksdbL0Files.add(l0Files, tags);
        }

        const writeStopped = toNumber(props["is_write_stopped"]);
        if (writeStopped !== null) {
          rocksdbWriteStopped.add(writeStopped, tags);
        }

        const delayedWriteRate = toNumber(props["actual_delayed_write_rate"]);
        if (delayedWriteRate !== null) {
          rocksdbDelayedWriteRate.add(delayedWriteRate, tags);
        }

        const estimateNumKeys = toNumber(props["estimate_num_keys"]);
        if (estimateNumKeys !== null) {
          rocksdbEstimateNumKeys.add(estimateNumKeys, tags);
        }

        const tableReadersMem = toNumber(props["estimate_table_readers_mem"]);
        if (tableReadersMem !== null) {
          rocksdbTableReadersMem.add(tableReadersMem, tags);
        }

        const pinnedUsage = toNumber(props["block_cache_pinned_usage"]);
        if (pinnedUsage !== null) {
          rocksdbBlockCachePinned.add(pinnedUsage, tags);
        }

        const flushPending = toNumber(props["mem_table_flush_pending"]);
        if (flushPending !== null) {
          rocksdbMemTableFlushPending.add(flushPending, tags);
        }
      }

      if (typeof d.rocksdb_statistics === "string" && d.rocksdb_statistics.length > 0) {
        const statistics = d.rocksdb_statistics;

        const blockCacheHit = extractTickerCount(statistics, ROCKSDB_COUNTER_ALIASES.blockCacheHit);
        if (blockCacheHit !== null) {
          rocksdbBlockCacheHitCount.add(blockCacheHit, tags);
        }

        const blockCacheMiss = extractTickerCount(statistics, ROCKSDB_COUNTER_ALIASES.blockCacheMiss);
        if (blockCacheMiss !== null) {
          rocksdbBlockCacheMissCount.add(blockCacheMiss, tags);
        }

        if (blockCacheHit !== null && blockCacheMiss !== null) {
          const denominator = blockCacheHit + blockCacheMiss;
          if (denominator > 0) {
            rocksdbBlockCacheHitRatio.add(blockCacheHit / denominator, tags);
          }
        }

        const bloomUseful = extractTickerCount(statistics, ROCKSDB_COUNTER_ALIASES.bloomFilterUseful);
        if (bloomUseful !== null) {
          rocksdbBloomFilterUsefulCount.add(bloomUseful, tags);
        }

        const bloomPositive = extractTickerCount(statistics, ROCKSDB_COUNTER_ALIASES.bloomFilterPositive);
        if (bloomPositive !== null) {
          rocksdbBloomFilterPositiveCount.add(bloomPositive, tags);
        }

        const bloomTruePositive = extractTickerCount(statistics, ROCKSDB_COUNTER_ALIASES.bloomFilterTruePositive);
        if (bloomTruePositive !== null) {
          rocksdbBloomFilterTruePositiveCount.add(bloomTruePositive, tags);
        }

        if (bloomPositive !== null && bloomTruePositive !== null && bloomPositive > 0) {
          rocksdbBloomFilterTruePositiveRatio.add(bloomTruePositive / bloomPositive, tags);
        }

        const stallMicros = extractTickerCount(statistics, ROCKSDB_COUNTER_ALIASES.stallMicros);
        if (stallMicros !== null) {
          rocksdbStallMicros.add(stallMicros, tags);
        }

        const compactionReadBytes = extractTickerCount(statistics, ROCKSDB_COUNTER_ALIASES.compactReadBytes);
        if (compactionReadBytes !== null) {
          rocksdbCompactionReadBytes.add(compactionReadBytes, tags);
        }

        const compactionWriteBytes = extractTickerCount(statistics, ROCKSDB_COUNTER_ALIASES.compactWriteBytes);
        if (compactionWriteBytes !== null) {
          rocksdbCompactionWriteBytes.add(compactionWriteBytes, tags);
        }

        if (compactionReadBytes !== null && compactionWriteBytes !== null && compactionReadBytes > 0) {
          rocksdbCompactionWriteAmp.add(compactionWriteBytes / compactionReadBytes, tags);
        }
      }
    } catch { /* ignore */ }
  }

  sleep(INTERVAL_SECONDS);
}
