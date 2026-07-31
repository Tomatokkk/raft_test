#pragma once

#include "state_machine/command.h"

#include <libnuraft/nuraft.hxx>

#include <strongkv/config.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace strongkv {

class FileLogStore;
class FileStateManager;
class KvStateMachine;
class Logger;

enum class RaftRequestStatus {
    kOk,
    kNotLeader,
    kTimeout,
    kUnavailable,
    kRejected,
};

struct RaftCommandResult {
    RaftRequestStatus status{RaftRequestStatus::kUnavailable};
    ApplyResult result;
    std::string detail;
};

struct LinearizableGetResult {
    RaftRequestStatus status{RaftRequestStatus::kUnavailable};
    std::optional<std::string> value;
    std::string detail;
};

struct RaftNodeInfo {
    std::int32_t node_id{0};
    bool initialized{false};
    bool leader{false};
    std::int32_t leader_id{-1};
    std::uint64_t term{0};
    std::uint64_t commit_index{0};
    std::uint64_t last_log_index{0};
    std::uint64_t last_applied{0};
    std::uint64_t snapshot_index{0};
    std::size_t cluster_size{0};
    std::uint64_t proposal_batches{0};
    std::uint64_t proposal_entries{0};
    std::uint64_t read_batches{0};
    std::uint64_t read_requests{0};
};

// Owns the NuRaft launcher and exposes only operations with service-level
// consistency semantics. In particular, GET is guarded by a committed
// current-term barrier rather than by a local role check.
class RaftNode final {
public:
    RaftNode(Config config, nuraft::ptr<Logger> logger);
    ~RaftNode();

    RaftNode(const RaftNode&) = delete;
    RaftNode& operator=(const RaftNode&) = delete;

    void start();
    void stop();

    RaftCommandResult submit(const Command& command);
    LinearizableGetResult linearizable_get(const std::string& key);

    bool is_leader() const;
    std::int32_t leader_id() const;
    std::optional<std::string> leader_client_endpoint() const;
    RaftNodeInfo info() const;
    int fatal_storage_error() const noexcept;

private:
    struct PendingProposal {
        Command command;
        std::promise<RaftCommandResult> completion;
    };

    struct PendingRead {
        std::string key;
        std::promise<LinearizableGetResult> completion;
    };

    nuraft::cb_func::ReturnCode on_raft_event(
        nuraft::cb_func::Type type, nuraft::cb_func::Param* param);
    static RaftRequestStatus map_status(nuraft::cmd_result_code code);
    void start_workers();
    void stop_workers();
    void proposal_worker();
    void read_worker();
    void process_proposal_batch(
        const std::vector<std::shared_ptr<PendingProposal>>& batch);
    static RaftCommandResult unavailable_result(std::string detail);

    Config config_;
    nuraft::ptr<Logger> logger_;
    nuraft::ptr<FileLogStore> log_store_;
    nuraft::ptr<FileStateManager> state_manager_;
    nuraft::ptr<KvStateMachine> state_machine_;

    mutable std::mutex lifecycle_mutex_;
    std::unique_ptr<nuraft::raft_launcher> launcher_;
    nuraft::ptr<nuraft::raft_server> server_;
    std::atomic<bool> running_{false};

    std::mutex proposal_mutex_;
    std::condition_variable proposal_cv_;
    std::deque<std::shared_ptr<PendingProposal>> proposal_queue_;
    bool proposal_stopping_{true};
    std::thread proposal_thread_;

    std::mutex read_mutex_;
    std::condition_variable read_cv_;
    std::deque<std::shared_ptr<PendingRead>> read_queue_;
    bool read_stopping_{true};
    std::thread read_thread_;

    std::atomic<std::uint64_t> proposal_batches_{0};
    std::atomic<std::uint64_t> proposal_entries_{0};
    std::atomic<std::uint64_t> read_batches_{0};
    std::atomic<std::uint64_t> read_requests_{0};
};

const char* raft_request_status_name(RaftRequestStatus status) noexcept;

}  // namespace strongkv
