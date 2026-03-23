#pragma once

#include <stdint.h>
#include <cstdlib>
#include <string>
#include <sstream>
#include <memory>
#include <mutex>
#include <thread>
#include <shared_mutex>
#include <vector>
#include <option.h>
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/options.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/transaction_log.h>
#include <rocksdb/cache.h>
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/rate_limiter.h>
#include <rocksdb/statistics.h>
#include <mutex>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/utilities/table_properties_collectors.h>
#include "string_utils.h"
#include "logger.h"
#include "file_utils.h"
#include <rocksdb/utilities/db_ttl.h>

#define FOURWEEKS_SECS 2419200

class UInt64AddOperator : public rocksdb::AssociativeMergeOperator {
public:
    virtual bool Merge(const rocksdb::Slice& key, const rocksdb::Slice* existing_value, const rocksdb::Slice& value,
                       std::string* new_value, rocksdb::Logger* logger) const override {
        uint64_t existing = 0;
        if (existing_value) {
            existing = StringUtils::deserialize_uint32_t(existing_value->ToString());
        }
        *new_value = StringUtils::serialize_uint32_t(existing + StringUtils::deserialize_uint32_t(value.ToString()));
        return true;
    }

    virtual const char* Name() const override {
        return "UInt64AddOperator";
    }
};

enum StoreStatus {
    FOUND,
    NOT_FOUND,
    ERROR
};

/*
 *  Abstraction for underlying KV store (RocksDB)
 */
class Store {
private:

    const std::string state_dir_path;
    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Options options;
    rocksdb::WriteOptions write_options;
    bool async_io_enabled;

    // Used to protect assignment to DB handle, which is otherwise thread safe
    // So we use unique lock only for assignment, but shared locks for all other operations on DB
    mutable std::shared_mutex mutex;

    rocksdb::Status init_db(int32_t ttl);

public:

    Store() = delete;

    Store(const std::string & state_dir_path,
          const size_t wal_ttl_secs = 24*60*60,
          const size_t wal_size_mb = 1024,
          bool disable_wal = true,
          int32_t ttl=0,
          size_t write_buffer_size = 4*1048576,
          size_t max_write_buffer_number = 2,
          size_t max_log_file_size = 4*1048576,
          size_t keep_log_file_num = 5,
          size_t block_cache_size = 256*1048576,
          int64_t rate_limit_bytes_per_sec = 0,
          bool level_compaction_dynamic_level_bytes = true,
          uint32_t block_size = 16*1024,
          uint32_t format_version = 7,
          bool enable_statistics = true,
          uint32_t compression_parallel_threads = 4,
          uint64_t bytes_per_sync = 1048576,
          uint64_t max_manifest_file_size = 1048576,
          bool enable_async_io = true,
          const std::string& offpeak_time_utc = "02:00-06:00",
          bool unordered_write = true,
          uint32_t max_subcompactions = 2,
          uint32_t max_background_jobs = 0,
          bool use_direct_reads = false,
          bool use_direct_io_for_flush_and_compaction = false,
          uint64_t compaction_readahead_size = 0,
          bool optimize_filters_for_hits = false,
          bool paranoid_memory_checks = true);

    ~Store();

    bool insert(const std::string& key, const std::string& value);

    bool batch_write(rocksdb::WriteBatch& batch);

    bool contains(const std::string& key) const;

    StoreStatus get(const std::string& key, std::string& value, bool fill_cache = true) const;

    void multi_get(const std::vector<std::string>& keys, std::vector<StoreStatus>& statuses,
                   std::vector<std::string>& values, bool fill_cache = true) const;

    void multi_get_pinned(const std::vector<std::string>& keys, std::vector<StoreStatus>& statuses,
                          std::vector<rocksdb::PinnableSlice>& values, bool fill_cache = true,
                          bool sorted_input = false) const;

    bool remove(const std::string& key);

    rocksdb::Iterator* scan(const std::string & prefix, const rocksdb::Slice* iterate_upper_bound);

    rocksdb::Iterator* get_iterator();

    void scan_fill(const std::string& prefix_start, const std::string& prefix_end, std::vector<std::string> & values);

    void increment(const std::string & key, uint32_t value);

    uint64_t get_latest_seq_number() const;

    Option<std::vector<std::string>*> get_updates_since(const uint64_t seq_number_org, const uint64_t max_updates) const;

    void close();

    int reload(bool clear_state_dir, const std::string& snapshot_path, int32_t ttl = 0);

    void flush();

    rocksdb::Status compact_all();

    rocksdb::Status create_check_point(rocksdb::Checkpoint** checkpoint_ptr, const std::string& db_snapshot_path);

    rocksdb::Status delete_range(const std::string& begin_key, const std::string& end_key);

    rocksdb::Status compact_range(const rocksdb::Slice& begin_key, const rocksdb::Slice& end_key);

    // Only for internal tests
    rocksdb::DB* _get_db_unsafe() const;

    const std::string& get_state_dir_path() const;

    const rocksdb::Options &get_db_options() const;

    void print_memory_usage();

    std::string get_statistics() const;

    void get_last_N_values(const std::string& userid_prefix, uint32_t N, std::vector<std::string>& values);
};
