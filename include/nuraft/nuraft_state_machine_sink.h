#pragma once

#include <string>
#include <vector>

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
