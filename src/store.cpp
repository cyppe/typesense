#include "include/store.h"
#include "logger.h"
#include <rocksdb/statistics.h>

Store::Store(const std::string & state_dir_path, const size_t wal_ttl_secs, const size_t wal_size_mb, bool disable_wal,
             int32_t ttl, size_t write_buffer_size, size_t max_write_buffer_number, size_t max_log_file_size,
             size_t keep_log_file_num, size_t block_cache_size,
             int64_t rate_limit_bytes_per_sec, bool level_compaction_dynamic_level_bytes,
             uint32_t block_size, uint32_t format_version, bool enable_statistics,
             uint32_t compression_parallel_threads, uint64_t bytes_per_sync,
             uint64_t max_manifest_file_size, bool enable_async_io,
             const std::string& offpeak_time_utc,
             bool unordered_write, uint32_t max_subcompactions,
             uint32_t max_background_jobs, bool use_direct_reads,
             bool use_direct_io_for_flush_and_compaction,
             uint64_t compaction_readahead_size,
             bool optimize_filters_for_hits,
             bool paranoid_memory_checks):
             state_dir_path(state_dir_path), async_io_enabled(enable_async_io) {
    // Optimize RocksDB
    if(max_background_jobs > 0) {
        options.IncreaseParallelism(static_cast<int>(max_background_jobs));
        options.max_background_jobs = static_cast<int>(max_background_jobs);
    } else {
        options.IncreaseParallelism();
    }
    // Pass total memtable budget so OptimizeLevelStyleCompaction sizes buffers correctly.
    options.OptimizeLevelStyleCompaction(write_buffer_size * max_write_buffer_number);
    options.create_if_missing = true;
    options.write_buffer_size = write_buffer_size;
    options.max_write_buffer_number = max_write_buffer_number;

    // Automatically size levels based on actual data volume — reduces write amplification
    options.level_compaction_dynamic_level_bytes = level_compaction_dynamic_level_bytes;
    options.merge_operator.reset(new UInt64AddOperator);
    // Per-level compression: L0-L1 uncompressed (max write speed),
    // L2-L4 LZ4 (fast decompression ~4.5 GB/s), L5+ ZSTD (best ratio for cold data)
    options.compression = rocksdb::CompressionType::kLZ4Compression;
    options.compression_per_level = {
        rocksdb::CompressionType::kNoCompression,     // L0: memtable flush, keep fast
        rocksdb::CompressionType::kNoCompression,     // L1: hot data, no compression overhead
        rocksdb::CompressionType::kLZ4Compression,    // L2: warm data, fast decompression
        rocksdb::CompressionType::kLZ4Compression,    // L3
        rocksdb::CompressionType::kLZ4Compression,    // L4
        rocksdb::CompressionType::kZSTD,              // L5: cold data, best ratio
        rocksdb::CompressionType::kZSTD,              // L6: coldest data
    };
    options.bottommost_compression = rocksdb::CompressionType::kZSTD;

    options.max_log_file_size = max_log_file_size;
    options.keep_log_file_num = keep_log_file_num;

    // Block-based table options with cache, bloom filters, and best practices
    rocksdb::BlockBasedTableOptions table_options;

    // HyperClockCache — production-ready in 10.7+, preferred over LRUCache
    auto cache = rocksdb::HyperClockCacheOptions(
        block_cache_size,
        0 /* estimated_entry_charge=0 for auto */
    ).MakeSharedCache();
    table_options.block_cache = cache;

    // Ribbon filters: 30% less memory than Bloom at same FP rate, Bloom for L0 (faster build)
    table_options.filter_policy.reset(rocksdb::NewRibbonFilterPolicy(10, 0));
    table_options.optimize_filters_for_memory = true;

    // Cache index and filter blocks in block cache for better memory management
    table_options.cache_index_and_filter_blocks = true;
    table_options.pin_l0_filter_and_index_blocks_in_cache = true;
    table_options.cache_index_and_filter_blocks_with_high_priority = true;

    // Partitioned index/filters: breaks large index/filter blocks into partitions
    // for better cache efficiency when working set exceeds cache (up to 11x throughput)
    table_options.index_type = rocksdb::BlockBasedTableOptions::kTwoLevelIndexSearch;
    table_options.partition_filters = true;
    table_options.metadata_block_size = 4096;
    table_options.pin_top_level_index_and_filter = true;

    // Warm block cache on memtable flush — newly written data is immediately cache-hot
    table_options.prepopulate_block_cache =
        rocksdb::BlockBasedTableOptions::PrepopulateBlockCache::kFlushOnly;

    table_options.format_version = format_version;
    table_options.block_size = block_size;

    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
    options.optimize_filters_for_hits = optimize_filters_for_hits;

    // Per-key-value integrity checking for in-memory blocks (moved to ColumnFamilyOptions in v10)
    options.block_protection_bytes_per_key = 1;

    // Rate limiter to prevent compaction I/O from starving reads
    if (rate_limit_bytes_per_sec > 0) {
        options.rate_limiter.reset(rocksdb::NewGenericRateLimiter(
            rate_limit_bytes_per_sec,
            100 * 1000,  // refill period 100ms
            10,          // fairness
            rocksdb::RateLimiter::Mode::kWritesOnly
        ));
    }

    options.bytes_per_sync = bytes_per_sync;
    options.max_manifest_file_size = max_manifest_file_size;

    // RocksDB statistics counters (kExceptTimers avoids timer overhead, counters are ~free)
    if (enable_statistics) {
        options.statistics = rocksdb::CreateDBStatistics();
        options.statistics->set_stats_level(rocksdb::StatsLevel::kExceptTimers);
    }

    // Parallel compression threads
    rocksdb::CompressionOptions compression_opts;
    compression_opts.parallel_threads = compression_parallel_threads;
    options.compression_opts = compression_opts;

    // Per-read memory validation to catch corruption early (v9.6+, low overhead)
    options.paranoid_memory_checks = paranoid_memory_checks;

    options.use_direct_reads = use_direct_reads;
    options.use_direct_io_for_flush_and_compaction = use_direct_io_for_flush_and_compaction;
    options.compaction_readahead_size = compaction_readahead_size;

    // Offpeak compaction scheduling — reduces compaction impact during peak hours (v8.8+)
    if (!offpeak_time_utc.empty()) {
        options.daily_offpeak_time_utc = offpeak_time_utc;
    }

    // Unordered writes: +34-131% write throughput by relaxing snapshot immutability.
    // Safe when WAL is disabled (Raft provides ordering) and app doesn't rely on
    // consistent-point-in-time snapshots during concurrent writes.
    if (unordered_write && disable_wal) {
        options.unordered_write = true;
    }

    // Parallel sub-compactions for L0→L1 (the most critical compaction level)
    if (max_subcompactions > 1) {
        options.max_subcompactions = max_subcompactions;
    }

    // Compact on deletion to reclaim space from bulk deletes
    options.table_properties_collector_factories.emplace_back(
        rocksdb::NewCompactOnDeletionCollectorFactory(10000, 7500, 0.5));

    // these need to be high for replication scenarios
    options.WAL_ttl_seconds = wal_ttl_secs;
    options.WAL_size_limit_MB = wal_size_mb;

    // Disable WAL for master writes (Raft's WAL is used)
    // The replica uses native WAL, though.
    write_options.disableWAL = disable_wal;

    // open DB
    init_db(ttl);
}

Store::~Store() {
    close();
}

rocksdb::Status Store::init_db(int32_t ttl) {
    TS_LOG(INFO) << "Initializing DB by opening state dir: " << state_dir_path;

    rocksdb::Status s;

    if(ttl > 0) {
        rocksdb::DBWithTTL* dbWithTtl = nullptr;
        s = rocksdb::DBWithTTL::Open(options, state_dir_path,
                                     &dbWithTtl, ttl, false);
        db.reset(dbWithTtl);
    } else {
        rocksdb::DB* dbRaw = nullptr;
        s = rocksdb::DB::Open(options, state_dir_path, &dbRaw);
        db.reset(dbRaw);
    }

    if(!s.ok()) {
        TS_LOG(ERROR) << "Error while initializing store: " << s.ToString();
        if(s.code() == rocksdb::Status::Code::kIOError) {
            TS_LOG(ERROR) << "It seems like the data directory " << state_dir_path << " is already being used by "
                       << "another Typesense server. ";
            TS_LOG(ERROR) << "If you are SURE that this is not the case, delete the LOCK file "
                       << "in the data db directory and try again.";
        }
    }

    assert(s.ok());
    return s;
}

bool Store::insert(const std::string& key, const std::string& value) {
    std::shared_lock lock(mutex);
    rocksdb::Status status = db->Put(write_options, key, value);
    return status.ok();
}

bool Store::batch_write(rocksdb::WriteBatch& batch) {
    std::shared_lock lock(mutex);
    rocksdb::Status status = db->Write(write_options, &batch);
    return status.ok();
}

bool Store::contains(const std::string& key) const {
    std::shared_lock lock(mutex);

    std::string value;
    bool value_found;
    bool key_may_exist = db->KeyMayExist(rocksdb::ReadOptions(), key, &value, &value_found);

    // returns false when key definitely does not exist
    if(!key_may_exist) {
        return false;
    }

    if(value_found) {
        return true;
    }

    // otherwise, we have try getting the value
    rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok() && !status.IsNotFound();
}

StoreStatus Store::get(const std::string& key, std::string& value, bool fill_cache) const {
    std::shared_lock lock(mutex);
    rocksdb::ReadOptions read_options;
    read_options.fill_cache = fill_cache;
    rocksdb::Status status = db->Get(read_options, key, &value);

    if(status.ok()) {
        return StoreStatus::FOUND;
    }

    if(status.IsNotFound()) {
        return StoreStatus::NOT_FOUND;
    }

    TS_LOG(ERROR) << "Error while fetching the key: " << key << " - status is: " << status.ToString();
    return StoreStatus::ERROR;
}

void Store::multi_get(const std::vector<std::string>& keys, std::vector<StoreStatus>& statuses,
                      std::vector<std::string>& values, bool fill_cache) const {
    statuses.clear();
    values.clear();

    if(keys.empty()) {
        return;
    }

    std::vector<rocksdb::Slice> slices;
    slices.reserve(keys.size());
    for(const auto& key: keys) {
        slices.emplace_back(key);
    }

    values.resize(keys.size());

    rocksdb::ReadOptions read_options;
    read_options.fill_cache = fill_cache;

    std::shared_lock lock(mutex);
    const std::vector<rocksdb::Status> rocks_statuses = db->MultiGet(read_options, slices, &values);

    statuses.reserve(rocks_statuses.size());
    for(const auto& status: rocks_statuses) {
        if(status.ok()) {
            statuses.push_back(StoreStatus::FOUND);
        } else if(status.IsNotFound()) {
            statuses.push_back(StoreStatus::NOT_FOUND);
        } else {
            statuses.push_back(StoreStatus::ERROR);
        }
    }
}

bool Store::remove(const std::string& key) {
    std::shared_lock lock(mutex);
    rocksdb::Status status = db->Delete(write_options, key);
    return status.ok();
}

rocksdb::Iterator* Store::scan(const std::string & prefix, const rocksdb::Slice* iterate_upper_bound) {
    std::shared_lock lock(mutex);
    rocksdb::ReadOptions read_opts;
    read_opts.async_io = async_io_enabled;
    if(iterate_upper_bound) {
        read_opts.iterate_upper_bound = iterate_upper_bound;
    }
    rocksdb::Iterator *iter = db->NewIterator(read_opts);
    iter->Seek(prefix);
    return iter;
}

rocksdb::Iterator* Store::get_iterator() {
    std::shared_lock lock(mutex);
    rocksdb::ReadOptions read_opts;
    read_opts.async_io = async_io_enabled;
    rocksdb::Iterator* it = db->NewIterator(read_opts);
    return it;
}

void Store::scan_fill(const std::string& prefix_start, const std::string& prefix_end, std::vector<std::string> & values) {
    rocksdb::ReadOptions read_opts;
    read_opts.async_io = async_io_enabled;
    rocksdb::Slice upper_bound(prefix_end);
    read_opts.iterate_upper_bound = &upper_bound;

    std::shared_lock lock(mutex);
    rocksdb::Iterator *iter = db->NewIterator(read_opts);
    for (iter->Seek(prefix_start); iter->Valid() && iter->key().starts_with(prefix_start); iter->Next()) {
        values.push_back(iter->value().ToString());
    }

    delete iter;
}

void Store::increment(const std::string & key, uint32_t value) {
    std::shared_lock lock(mutex);
    db->Merge(write_options, key, StringUtils::serialize_uint32_t(value));
}

uint64_t Store::get_latest_seq_number() const {
    std::shared_lock lock(mutex);
    return db->GetLatestSequenceNumber();
}

Option<std::vector<std::string>*> Store::get_updates_since(const uint64_t seq_number_org, const uint64_t max_updates) const {
    std::shared_lock lock(mutex);
    const uint64_t local_latest_seq_num = db->GetLatestSequenceNumber();

    // Since GetUpdatesSince(0) == GetUpdatesSince(1)
    const uint64_t seq_number = (seq_number_org == 0) ? 1 : seq_number_org;

    if(seq_number == local_latest_seq_num+1) {
        // replica has caught up, send an empty list as result
        std::vector<std::string>* updates = new std::vector<std::string>();
        return Option<std::vector<std::string>*>(updates);
    }

    std::unique_ptr<rocksdb::TransactionLogIterator> iter;
    rocksdb::Status status = db->GetUpdatesSince(seq_number, &iter);

    if(!status.ok()) {
        TS_LOG(ERROR) << "Error while fetching updates for replication: " << status.ToString();

        std::ostringstream error;
        error << "Unable to fetch updates. " << "Master's latest sequence number is " << local_latest_seq_num
              << " but requested sequence number is " << seq_number;
        TS_LOG(ERROR) << error.str();

        return Option<std::vector<std::string>*>(400, error.str());
    }

    if(!iter->Valid()) {
        std::ostringstream error;
        error << "Invalid iterator. Master's latest sequence number is " << local_latest_seq_num << " but "
              << "updates are requested from sequence number " << seq_number << ". "
              << "The master's WAL entries might have expired (they are kept only for 24 hours).";
        TS_LOG(ERROR) << error.str();
        return Option<std::vector<std::string>*>(400, error.str());
    }

    uint64_t num_updates = 0;
    std::vector<std::string>* updates = new std::vector<std::string>();

    bool first_iteration = true;

    while(iter->Valid() && num_updates < max_updates) {
        const rocksdb::BatchResult & batch = iter->GetBatch();
        if(first_iteration) {
            first_iteration = false;
            if(batch.sequence != seq_number) {
                std::ostringstream error;
                error << "Invalid iterator. Requested sequence number is " << seq_number << " but "
                      << "updates are available only from sequence number " << batch.sequence << ". "
                      << "The master's WAL entries might have expired (they are kept only for 24 hours).";
                TS_LOG(ERROR) << error.str();
                return Option<std::vector<std::string>*>(400, error.str());
            }
        }

        const std::string & write_batch_serialized = batch.writeBatchPtr->Data();
        updates->push_back(write_batch_serialized);
        num_updates += 1;
        iter->Next();
    }

    return Option<std::vector<std::string>*>(updates);
}

void Store::close() {
    std::unique_lock lock(mutex);
    db.reset();
}

int Store::reload(bool clear_state_dir, const std::string& snapshot_path, int32_t ttl) {
    std::unique_lock lock(mutex);

    // we don't use close() to avoid nested lock and because lock is required until db is re-initialized
    db.reset();

    if(clear_state_dir) {
        if (!delete_path(state_dir_path, true)) {
            TS_LOG(WARNING) << "rm " << state_dir_path << " failed";
            return -1;
        }

        TS_LOG(INFO) << "rm " << state_dir_path << " success";
    }

    if(!snapshot_path.empty()) {
        // tries to use link if possible, or else copies
        if (!copy_dir(snapshot_path, state_dir_path)) {
            TS_LOG(WARNING) << "copy snapshot " << snapshot_path << " to " << state_dir_path << " failed";
            return -1;
        }

        TS_LOG(INFO) << "copy snapshot " << snapshot_path << " to " << state_dir_path << " success";
    }

    if (!create_directory(state_dir_path)) {
        TS_LOG(WARNING) << "CreateDirectory " << state_dir_path << " failed";
        return -1;
    }

    const rocksdb::Status& status = init_db(ttl);
    if (!status.ok()) {
        TS_LOG(WARNING) << "Open DB " << state_dir_path << " failed, msg: " << status.ToString();
        return -1;
    }

    TS_LOG(INFO) << "DB open success!";

    return 0;
}

void Store::flush() {
    std::shared_lock lock(mutex);
    rocksdb::FlushOptions options;
    db->Flush(options);
}

rocksdb::Status Store::compact_all() {
    std::shared_lock lock(mutex);
    return db->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
}

rocksdb::Status Store::create_check_point(rocksdb::Checkpoint** checkpoint_ptr, const std::string& db_snapshot_path) {
    std::shared_lock lock(mutex);
    rocksdb::Status status = rocksdb::Checkpoint::Create(db.get(), checkpoint_ptr);
    if(!status.ok()) {
        TS_LOG(ERROR) << "Checkpoint Create failed, msg:" << status.ToString();
        return status;
    }

    status = (*checkpoint_ptr)->CreateCheckpoint(db_snapshot_path);

    if(!status.ok()) {
        TS_LOG(WARNING) << "Checkpoint CreateCheckpoint failed at snapshot path: "
                     << db_snapshot_path << ", msg:" << status.ToString();
    }

    return status;
}

rocksdb::Status Store::delete_range(const std::string& begin_key, const std::string& end_key) {
    std::unique_lock lock(mutex);
    return db->DeleteRange(rocksdb::WriteOptions(), db->DefaultColumnFamily(), begin_key, end_key);
}

rocksdb::Status Store::compact_range(const rocksdb::Slice& begin_key, const rocksdb::Slice& end_key) {
    std::shared_lock lock(mutex);
    return db->CompactRange(rocksdb::CompactRangeOptions(), &begin_key, &end_key);
}

rocksdb::DB* Store::_get_db_unsafe() const {
    return db.get();
}

const std::string& Store::get_state_dir_path() const {
    return state_dir_path;
}

const rocksdb::Options& Store::get_db_options() const {
    return options;
}

void Store::print_memory_usage() {
    std::string val;

    db->GetProperty("rocksdb.estimate-table-readers-mem", &val);
    TS_LOG(INFO) << "rocksdb.estimate-table-readers-mem: " << val;

    db->GetProperty("rocksdb.cur-size-all-mem-tables", &val);
    TS_LOG(INFO) << "rocksdb.cur-size-all-mem-tables: " << val;

    db->GetProperty("rocksdb.block-cache-usage", &val);
    TS_LOG(INFO) << "rocksdb.block-cache-usage: " << val;

    db->GetProperty("rocksdb.block-cache-capacity", &val);
    TS_LOG(INFO) << "rocksdb.block-cache-capacity: " << val;

    db->GetProperty("rocksdb.estimate-live-data-size", &val);
    TS_LOG(INFO) << "rocksdb.estimate-live-data-size: " << val;

    db->GetProperty("rocksdb.num-running-compactions", &val);
    TS_LOG(INFO) << "rocksdb.num-running-compactions: " << val;

    db->GetProperty("rocksdb.is-write-stopped", &val);
    TS_LOG(INFO) << "rocksdb.is-write-stopped: " << val;

    db->GetProperty("rocksdb.actual-delayed-write-rate", &val);
    TS_LOG(INFO) << "rocksdb.actual-delayed-write-rate: " << val;

    db->GetProperty(rocksdb::DB::Properties::kBlockCacheEntryStats, &val);
    TS_LOG(INFO) << "rocksdb.block-cache-entry-stats: " << val;
}

std::string Store::get_statistics() const {
    if (options.statistics) {
        return options.statistics->ToString();
    }
    return "";
}

void Store::get_last_N_values(const std::string& userid_prefix, uint32_t N, std::vector<std::string>& values) {
    std::shared_lock lock(mutex);

    rocksdb::ReadOptions read_opts;
    read_opts.async_io = async_io_enabled;
    rocksdb::Iterator* iter = db->NewIterator(read_opts);
    auto prefix_key = userid_prefix + "~";
    iter->SeekForPrev(prefix_key);

    while(iter->Valid() && N) {
        auto key = iter->key().ToString();
        if(!StringUtils::begins_with(key, userid_prefix)) {
            break;
        }

        values.push_back(iter->value().ToString());
        N--;
        iter->Prev();
    }

    delete iter;
}
