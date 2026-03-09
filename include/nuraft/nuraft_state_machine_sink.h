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
    bool read_materialized_entries(std::vector<std::pair<std::string, std::string>>& entries,
                                   std::string& error) const;

private:
    bool initialize_db(std::string& error) const;

    NuRaftStateLayout layout_;
    mutable std::unique_ptr<rocksdb::DB> db_;
};
