#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rocksdb/db.h>

#include "nuraft_applied_request_store.h"

class NuRaftStateMachineSink {
public:
    virtual ~NuRaftStateMachineSink() = default;

    virtual bool apply_all(const std::vector<NuRaftAppliedRequest>& requests, std::string& error) = 0;
    virtual bool read_all(std::vector<NuRaftAppliedRequest>& requests, std::string& error) const = 0;
};

class NuRaftFileBackedStateMachineSink : public NuRaftStateMachineSink {
public:
    explicit NuRaftFileBackedStateMachineSink(NuRaftStateLayout layout);

    bool apply_all(const std::vector<NuRaftAppliedRequest>& requests, std::string& error) override;
    bool read_all(std::vector<NuRaftAppliedRequest>& requests, std::string& error) const override;

private:
    NuRaftAppliedRequestStore store_;
};

class NuRaftKvStateMachineSink : public NuRaftStateMachineSink {
public:
    explicit NuRaftKvStateMachineSink(NuRaftStateLayout layout);
    ~NuRaftKvStateMachineSink() override;

    bool apply_all(const std::vector<NuRaftAppliedRequest>& requests, std::string& error) override;
    bool read_all(std::vector<NuRaftAppliedRequest>& requests, std::string& error) const override;
    bool create_checkpoint(const std::string& checkpoint_path, std::string& error) const;
    bool read_materialized_value(const std::string& key,
                                 std::string& value,
                                 bool& found,
                                 std::string& error) const;
    bool read_materialized_prefix(const std::string& prefix,
                                  std::vector<std::pair<std::string, std::string>>& entries,
                                  std::string& error) const;
    bool count_materialized_prefix(const std::string& prefix, size_t& count, std::string& error) const;
    bool read_materialized_entries(std::vector<std::pair<std::string, std::string>>& entries,
                                   std::string& error) const;

private:
    bool initialize_db(std::string& error) const;

    NuRaftStateLayout layout_;
    mutable std::shared_ptr<rocksdb::DB> db_;
};
