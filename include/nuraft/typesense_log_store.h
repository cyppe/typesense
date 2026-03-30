#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include <rocksdb/db.h>

#include <libnuraft/log_store.hxx>

// RocksDB-backed log store implementing NuRaft's nuraft::log_store interface.
//
// Key layout in RocksDB:
//   "entry/<20-digit-padded-index>" → serialized nuraft::log_entry
//   "meta/start_index"              → 8-byte big-endian uint64
//
// Thread-safe: all public methods are protected by a mutex.
class TypesenseLogStore : public nuraft::log_store {
public:
    explicit TypesenseLogStore(const std::string& db_path);
    ~TypesenseLogStore() override;

    // nuraft::log_store interface
    nuraft::ulong next_slot() const override;
    nuraft::ulong start_index() const override;
    nuraft::ptr<nuraft::log_entry> last_entry() const override;
    nuraft::ulong append(nuraft::ptr<nuraft::log_entry>& entry) override;
    void write_at(nuraft::ulong index, nuraft::ptr<nuraft::log_entry>& entry) override;
    nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>> log_entries(
        nuraft::ulong start, nuraft::ulong end) override;
    nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>> log_entries_ext(
        nuraft::ulong start, nuraft::ulong end,
        nuraft::int64 batch_size_hint_in_bytes = 0) override;
    nuraft::ptr<nuraft::log_entry> entry_at(nuraft::ulong index) override;
    nuraft::ulong term_at(nuraft::ulong index) override;
    nuraft::ptr<nuraft::buffer> pack(nuraft::ulong index, nuraft::int32 cnt) override;
    void apply_pack(nuraft::ulong index, nuraft::buffer& pack) override;
    bool compact(nuraft::ulong last_log_index) override;
    bool flush() override;

    void close();

private:
    static std::string entry_key(nuraft::ulong index);
    static nuraft::ulong parse_entry_key(const std::string& key);
    bool load_meta();
    void persist_start_index();
    nuraft::ptr<nuraft::log_entry> make_dummy_entry() const;

    std::string db_path_;
    std::shared_ptr<rocksdb::DB> db_;
    mutable std::shared_mutex lock_;
    nuraft::ulong start_index_;
    nuraft::ulong next_index_;
};
