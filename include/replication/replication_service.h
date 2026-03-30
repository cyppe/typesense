#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "../json.hpp"

struct http_req;
struct http_res;

class ReplicationService {
public:
    virtual ~ReplicationService() = default;

    virtual void write(const std::shared_ptr<http_req>& request,
                       const std::shared_ptr<http_res>& response) = 0;
    virtual bool should_replicate_write(uint64_t route_hash) const = 0;
    virtual bool is_read_caught_up() const = 0;
    virtual bool is_write_caught_up() const = 0;
    virtual bool is_alive() const = 0;
    virtual uint64_t node_state() const = 0;
    virtual nlohmann::json get_status() = 0;
    virtual void do_snapshot(const std::string& snapshot_path,
                             const std::shared_ptr<http_req>& req,
                             const std::shared_ptr<http_res>& res) = 0;
    virtual bool trigger_vote() = 0;
    virtual bool reset_peers() = 0;
    virtual void persist_applying_index() = 0;
    virtual int64_t get_num_queued_writes() = 0;
    virtual bool is_leader() = 0;
    virtual std::string get_leader_url() const = 0;
    virtual void decr_pending_writes() = 0;
};
