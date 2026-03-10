#include "nuraft/typesense_log_store.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <libnuraft/buffer.hxx>
#include <libnuraft/buffer_serializer.hxx>
#include <libnuraft/log_entry.hxx>

namespace {

constexpr const char* kEntryPrefix = "entry/";
constexpr size_t kEntryPrefixLen = 6;
constexpr const char* kStartIndexKey = "meta/start_index";
constexpr size_t kIndexPadWidth = 20;

std::string pad_index(nuraft::ulong index) {
    std::string s = std::to_string(index);
    if (s.size() < kIndexPadWidth) {
        s.insert(0, kIndexPadWidth - s.size(), '0');
    }
    return s;
}

void encode_uint64_be(uint8_t* buf, uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        buf[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }
}

uint64_t decode_uint64_be(const uint8_t* buf) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | buf[i];
    }
    return val;
}

}  // namespace

TypesenseLogStore::TypesenseLogStore(const std::string& db_path)
    : db_path_(db_path), start_index_(1), next_index_(1) {
    std::filesystem::create_directories(db_path);

    rocksdb::Options opts;
    opts.create_if_missing = true;
    opts.compression = rocksdb::kLZ4Compression;

    rocksdb::DB* raw_db = nullptr;
    auto status = rocksdb::DB::Open(opts, db_path, &raw_db);
    if (!status.ok()) {
        throw std::runtime_error("TypesenseLogStore: cannot open RocksDB at " +
                                 db_path + ": " + status.ToString());
    }
    db_.reset(raw_db);

    if (!load_meta()) {
        throw std::runtime_error("TypesenseLogStore: failed to load metadata");
    }
}

TypesenseLogStore::~TypesenseLogStore() {
    close();
}

void TypesenseLogStore::close() {
    std::lock_guard<std::mutex> guard(lock_);
    db_.reset();
}

std::string TypesenseLogStore::entry_key(nuraft::ulong index) {
    return std::string(kEntryPrefix) + pad_index(index);
}

nuraft::ulong TypesenseLogStore::parse_entry_key(const std::string& key) {
    return std::stoull(key.substr(kEntryPrefixLen));
}

bool TypesenseLogStore::load_meta() {
    // Load start_index from meta key.
    std::string val;
    auto s = db_->Get(rocksdb::ReadOptions(), kStartIndexKey, &val);
    if (s.ok() && val.size() == 8) {
        start_index_ = decode_uint64_be(reinterpret_cast<const uint8_t*>(val.data()));
    } else {
        start_index_ = 1;
    }

    // Find next_index by seeking to last entry key.
    next_index_ = start_index_;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
    // Seek to just past the last possible entry key.
    it->SeekForPrev(entry_key(UINT64_MAX));
    if (it->Valid()) {
        std::string key = it->key().ToString();
        if (key.substr(0, kEntryPrefixLen) == kEntryPrefix) {
            nuraft::ulong last_index = parse_entry_key(key);
            next_index_ = last_index + 1;
        }
    }

    return true;
}

void TypesenseLogStore::persist_start_index() {
    uint8_t buf[8];
    encode_uint64_be(buf, start_index_);
    db_->Put(rocksdb::WriteOptions(),
             kStartIndexKey,
             rocksdb::Slice(reinterpret_cast<const char*>(buf), 8));
}

nuraft::ptr<nuraft::log_entry> TypesenseLogStore::make_dummy_entry() const {
    return nuraft::cs_new<nuraft::log_entry>(0, nuraft::buffer::alloc(0));
}

nuraft::ulong TypesenseLogStore::next_slot() const {
    std::lock_guard<std::mutex> guard(lock_);
    return next_index_;
}

nuraft::ulong TypesenseLogStore::start_index() const {
    std::lock_guard<std::mutex> guard(lock_);
    return start_index_;
}

nuraft::ptr<nuraft::log_entry> TypesenseLogStore::last_entry() const {
    std::lock_guard<std::mutex> guard(lock_);
    if (next_index_ <= start_index_) {
        return make_dummy_entry();
    }
    std::string val;
    auto s = db_->Get(rocksdb::ReadOptions(), entry_key(next_index_ - 1), &val);
    if (!s.ok()) {
        return make_dummy_entry();
    }
    auto buf = nuraft::buffer::alloc(val.size());
    std::memcpy(buf->data(), val.data(), val.size());
    return nuraft::log_entry::deserialize(*buf);
}

nuraft::ulong TypesenseLogStore::append(nuraft::ptr<nuraft::log_entry>& entry) {
    std::lock_guard<std::mutex> guard(lock_);
    nuraft::ulong index = next_index_;
    auto serialized = entry->serialize();
    db_->Put(rocksdb::WriteOptions(),
             entry_key(index),
             rocksdb::Slice(reinterpret_cast<const char*>(serialized->data_begin()),
                            serialized->size()));
    next_index_ = index + 1;
    return index;
}

void TypesenseLogStore::write_at(nuraft::ulong index, nuraft::ptr<nuraft::log_entry>& entry) {
    std::lock_guard<std::mutex> guard(lock_);

    // Truncate everything from index onward.
    rocksdb::WriteBatch batch;
    for (nuraft::ulong i = index; i < next_index_; ++i) {
        batch.Delete(entry_key(i));
    }

    auto serialized = entry->serialize();
    batch.Put(entry_key(index),
              rocksdb::Slice(reinterpret_cast<const char*>(serialized->data_begin()),
                             serialized->size()));
    db_->Write(rocksdb::WriteOptions(), &batch);
    next_index_ = index + 1;
}

nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>>
TypesenseLogStore::log_entries(nuraft::ulong start, nuraft::ulong end) {
    return log_entries_ext(start, end, 0);
}

nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>>
TypesenseLogStore::log_entries_ext(nuraft::ulong start, nuraft::ulong end,
                                   nuraft::int64 batch_size_hint_in_bytes) {
    std::lock_guard<std::mutex> guard(lock_);
    auto result = nuraft::cs_new<std::vector<nuraft::ptr<nuraft::log_entry>>>();
    if (start >= end) {
        return result;
    }

    nuraft::int64 accum_size = 0;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
    it->Seek(entry_key(start));

    for (; it->Valid(); it->Next()) {
        std::string key = it->key().ToString();
        if (key.substr(0, kEntryPrefixLen) != kEntryPrefix) {
            break;
        }
        nuraft::ulong idx = parse_entry_key(key);
        if (idx >= end) {
            break;
        }

        auto val = it->value();
        auto buf = nuraft::buffer::alloc(val.size());
        std::memcpy(buf->data(), val.data(), val.size());
        result->push_back(nuraft::log_entry::deserialize(*buf));

        accum_size += static_cast<nuraft::int64>(val.size());
        if (batch_size_hint_in_bytes > 0 && accum_size >= batch_size_hint_in_bytes) {
            break;
        }
    }

    return result;
}

nuraft::ptr<nuraft::log_entry> TypesenseLogStore::entry_at(nuraft::ulong index) {
    std::lock_guard<std::mutex> guard(lock_);
    if (index < start_index_ || index >= next_index_) {
        return make_dummy_entry();
    }
    std::string val;
    auto s = db_->Get(rocksdb::ReadOptions(), entry_key(index), &val);
    if (!s.ok()) {
        return make_dummy_entry();
    }
    auto buf = nuraft::buffer::alloc(val.size());
    std::memcpy(buf->data(), val.data(), val.size());
    return nuraft::log_entry::deserialize(*buf);
}

nuraft::ulong TypesenseLogStore::term_at(nuraft::ulong index) {
    std::lock_guard<std::mutex> guard(lock_);
    if (index < start_index_ || index >= next_index_) {
        return 0;
    }
    std::string val;
    auto s = db_->Get(rocksdb::ReadOptions(), entry_key(index), &val);
    if (!s.ok()) {
        return 0;
    }
    auto buf = nuraft::buffer::alloc(val.size());
    std::memcpy(buf->data(), val.data(), val.size());
    return nuraft::log_entry::term_in_buffer(*buf);
}

nuraft::ptr<nuraft::buffer> TypesenseLogStore::pack(nuraft::ulong index, nuraft::int32 cnt) {
    std::lock_guard<std::mutex> guard(lock_);

    // Collect serialized entries.
    std::vector<nuraft::ptr<nuraft::buffer>> entries;
    entries.reserve(cnt);
    size_t total_size = sizeof(nuraft::int32);  // count header
    for (nuraft::int32 i = 0; i < cnt; ++i) {
        std::string val;
        auto s = db_->Get(rocksdb::ReadOptions(), entry_key(index + i), &val);
        if (!s.ok()) {
            break;
        }
        auto buf = nuraft::buffer::alloc(val.size());
        std::memcpy(buf->data(), val.data(), val.size());
        total_size += sizeof(nuraft::int32) + val.size();  // size + data
        entries.push_back(buf);
    }

    auto result = nuraft::buffer::alloc(total_size);
    result->put(static_cast<nuraft::int32>(entries.size()));
    for (auto& buf : entries) {
        result->put(static_cast<nuraft::int32>(buf->size()));
        result->put(*buf);
    }
    result->pos(0);
    return result;
}

void TypesenseLogStore::apply_pack(nuraft::ulong index, nuraft::buffer& pack) {
    std::lock_guard<std::mutex> guard(lock_);
    pack.pos(0);
    nuraft::int32 count = pack.get_int();

    rocksdb::WriteBatch batch;
    for (nuraft::int32 i = 0; i < count; ++i) {
        nuraft::int32 size = pack.get_int();
        auto raw = pack.get_raw(size);
        batch.Put(entry_key(index + i),
                  rocksdb::Slice(reinterpret_cast<const char*>(raw), size));
    }

    db_->Write(rocksdb::WriteOptions(), &batch);

    nuraft::ulong new_next = index + count;
    if (new_next > next_index_) {
        next_index_ = new_next;
    }
}

bool TypesenseLogStore::compact(nuraft::ulong last_log_index) {
    std::lock_guard<std::mutex> guard(lock_);
    if (last_log_index < start_index_) {
        return true;
    }

    // Delete entries [start_index_, last_log_index].
    std::string begin_key = entry_key(start_index_);
    // DeleteRange end is exclusive, so use last_log_index + 1.
    std::string end_key = entry_key(last_log_index + 1);
    db_->DeleteRange(rocksdb::WriteOptions(),
                     db_->DefaultColumnFamily(),
                     begin_key, end_key);

    start_index_ = last_log_index + 1;
    persist_start_index();
    return true;
}

bool TypesenseLogStore::flush() {
    std::lock_guard<std::mutex> guard(lock_);
    if (db_) {
        rocksdb::FlushOptions opts;
        opts.wait = true;
        db_->Flush(opts);
    }
    return true;
}
