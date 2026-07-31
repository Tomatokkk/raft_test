#include "raft/raft_node.h"

#include "logging/logger.h"
#include "state_machine/kv_state_machine.h"
#include "storage/file_log_store.h"
#include "storage/file_state_manager.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace strongkv {
namespace {

constexpr auto kInitializationTimeout = std::chrono::seconds(10);
constexpr std::size_t kMaximumProposalBatch = 128;
constexpr auto kProposalBatchDelay =
    std::chrono::microseconds(150);
constexpr std::size_t kMaximumReadBatch = 4096;
constexpr auto kReadBatchDelay = std::chrono::microseconds(50);

std::string role_description(bool leader) {
    return leader ? "LEADER" : "FOLLOWER";
}

}  // namespace

RaftNode::RaftNode(Config config, nuraft::ptr<Logger> logger)
    : config_(std::move(config)),
      logger_(std::move(logger)) {
    if (!logger_) {
        throw std::invalid_argument("RaftNode requires a logger");
    }
}

RaftNode::~RaftNode() {
    stop();
}

void RaftNode::start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_.load()) {
        return;
    }

    std::filesystem::create_directories(config_.data_dir);
    std::filesystem::create_directories(config_.raft_log_dir);
    std::filesystem::create_directories(config_.snapshot_dir);

    log_store_ = nuraft::cs_new<FileLogStore>(
        config_.raft_log_dir / "raft-log.bin");
    state_manager_ = nuraft::cs_new<FileStateManager>(
        config_, log_store_, logger_);
    state_machine_ = nuraft::cs_new<KvStateMachine>(
        config_.data_dir, config_.snapshot_dir,
        config_.max_request_size, logger_);
    launcher_ = std::make_unique<nuraft::raft_launcher>();

    nuraft::asio_service::options asio_options;
    asio_options.thread_pool_size_ = 4;

    nuraft::raft_params params;
    params.heart_beat_interval_ = config_.heartbeat_ms;
    params.election_timeout_lower_bound_ =
        config_.election_timeout_lower_ms;
    params.election_timeout_upper_bound_ =
        config_.election_timeout_upper_ms;
    params.client_req_timeout_ = config_.client_request_timeout_ms;
    params.snapshot_distance_ = config_.snapshot_distance;
    params.reserved_log_items_ = config_.reserved_log_items;
    params.max_append_size_ = 256;
    params.use_bg_thread_for_urgent_commit_ = false;
    params.return_method_ = nuraft::raft_params::blocking;
    params.auto_forwarding_ = false;

    nuraft::raft_server::init_options init_options;
    init_options.raft_callback_ =
        [this](nuraft::cb_func::Type type,
               nuraft::cb_func::Param* param) {
            return on_raft_event(type, param);
        };

    logger_->info(
        "starting NuRaft node_id=" + std::to_string(config_.node_id) +
        " raft=" + config_.local_node().raft_endpoint() +
        " client=" + config_.local_node().client_endpoint());

    server_ = launcher_->init(
        state_machine_, state_manager_, logger_, config_.raft_port,
        asio_options, params, init_options);
    if (!server_) {
        launcher_.reset();
        state_machine_.reset();
        state_manager_.reset();
        log_store_.reset();
        throw std::runtime_error("NuRaft launcher initialization failed");
    }

    const auto deadline =
        std::chrono::steady_clock::now() + kInitializationTimeout;
    while (!server_->is_initialized() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!server_->is_initialized()) {
        launcher_->shutdown(5);
        server_.reset();
        launcher_.reset();
        state_machine_.reset();
        state_manager_.reset();
        log_store_.reset();
        throw std::runtime_error(
            "NuRaft did not initialize within 10 seconds");
    }

    running_.store(true);
    start_workers();
    logger_->info("NuRaft initialized node_id=" +
                  std::to_string(config_.node_id));
}

void RaftNode::stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!launcher_) {
        return;
    }
    running_.store(false);
    logger_->info("stopping NuRaft node_id=" +
                  std::to_string(config_.node_id));
    stop_workers();
    const bool graceful = launcher_->shutdown(10);
    if (log_store_) {
        log_store_->flush();
    }
    if (state_machine_) {
        state_machine_->checkpoint();
    }
    server_.reset();
    launcher_.reset();
    state_machine_.reset();
    state_manager_.reset();
    log_store_.reset();
    logger_->info(std::string("NuRaft stopped, graceful=") +
                  (graceful ? "true" : "false"));
}

RaftCommandResult RaftNode::submit(const Command& command) {
    auto server = server_;
    if (!running_.load() || !server || !server->is_initialized()) {
        return unavailable_result("Raft node is not ready");
    }
    if (!server->is_leader()) {
        RaftCommandResult output;
        output.status = RaftRequestStatus::kNotLeader;
        output.detail = "this node is not leader";
        return output;
    }

    auto pending = std::make_shared<PendingProposal>();
    pending->command = command;
    auto result = pending->completion.get_future();
    {
        std::lock_guard<std::mutex> lock(proposal_mutex_);
        if (proposal_stopping_ || !running_.load()) {
            return unavailable_result("Raft proposal worker is stopping");
        }
        proposal_queue_.push_back(pending);
    }
    proposal_cv_.notify_one();
    return result.get();
}

LinearizableGetResult RaftNode::linearizable_get(
        const std::string& key) {
    LinearizableGetResult output;
    auto server = server_;
    if (!running_.load() || !server || !server->is_initialized()) {
        output.detail = "Raft node is not ready";
        return output;
    }
    if (!server->is_leader()) {
        output.status = RaftRequestStatus::kNotLeader;
        output.detail = "this node is not leader";
        return output;
    }

    auto pending = std::make_shared<PendingRead>();
    pending->key = key;
    auto result = pending->completion.get_future();
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        if (read_stopping_ || !running_.load()) {
            output.detail = "linearizable read worker is stopping";
            return output;
        }
        read_queue_.push_back(pending);
    }
    read_cv_.notify_one();
    return result.get();
}

void RaftNode::start_workers() {
    {
        std::lock_guard<std::mutex> lock(proposal_mutex_);
        proposal_stopping_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        read_stopping_ = false;
    }
    proposal_thread_ =
        std::thread(&RaftNode::proposal_worker, this);
    read_thread_ = std::thread(&RaftNode::read_worker, this);
}

void RaftNode::stop_workers() {
    std::deque<std::shared_ptr<PendingRead>> rejected_reads;
    std::deque<std::shared_ptr<PendingProposal>> rejected_proposals;
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        read_stopping_ = true;
        rejected_reads.swap(read_queue_);
    }
    read_cv_.notify_all();
    for (auto& pending : rejected_reads) {
        LinearizableGetResult result;
        result.detail = "Raft node is stopping";
        pending->completion.set_value(std::move(result));
    }

    {
        std::lock_guard<std::mutex> lock(proposal_mutex_);
        proposal_stopping_ = true;
        rejected_proposals.swap(proposal_queue_);
    }
    proposal_cv_.notify_all();
    for (auto& pending : rejected_proposals) {
        pending->completion.set_value(
            unavailable_result("Raft node is stopping"));
    }

    if (read_thread_.joinable()) {
        read_thread_.join();
    }
    if (proposal_thread_.joinable()) {
        proposal_thread_.join();
    }
}

void RaftNode::proposal_worker() {
    for (;;) {
        std::vector<std::shared_ptr<PendingProposal>> batch;
        {
            std::unique_lock<std::mutex> lock(proposal_mutex_);
            proposal_cv_.wait(lock, [this] {
                return proposal_stopping_ || !proposal_queue_.empty();
            });
            if (proposal_stopping_ && proposal_queue_.empty()) {
                return;
            }

            const auto deadline =
                std::chrono::steady_clock::now() +
                kProposalBatchDelay;
            while (!proposal_stopping_ &&
                   proposal_queue_.size() < kMaximumProposalBatch &&
                   proposal_cv_.wait_until(lock, deadline) !=
                       std::cv_status::timeout) {
            }
            const auto count = std::min(
                proposal_queue_.size(), kMaximumProposalBatch);
            batch.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                batch.push_back(std::move(proposal_queue_.front()));
                proposal_queue_.pop_front();
            }
        }
        process_proposal_batch(batch);
    }
}

void RaftNode::process_proposal_batch(
        const std::vector<std::shared_ptr<PendingProposal>>& batch) {
    if (batch.empty()) {
        return;
    }
    proposal_batches_.fetch_add(1, std::memory_order_relaxed);
    proposal_entries_.fetch_add(
        static_cast<std::uint64_t>(batch.size()),
        std::memory_order_relaxed);
    auto fail_all = [&batch](RaftCommandResult failure) {
        for (const auto& pending : batch) {
            pending->completion.set_value(failure);
        }
    };

    auto server = server_;
    auto state_machine = state_machine_;
    if (!server || !state_machine || !server->is_initialized()) {
        fail_all(unavailable_result("Raft node is not ready"));
        return;
    }
    if (!server->is_leader()) {
        RaftCommandResult failure;
        failure.status = RaftRequestStatus::kNotLeader;
        failure.detail = "this node is not leader";
        fail_all(std::move(failure));
        return;
    }

    std::vector<nuraft::ptr<nuraft::buffer>> logs;
    logs.reserve(batch.size());
    try {
        for (const auto& pending : batch) {
            logs.push_back(encode_command(pending->command));
        }
    } catch (const std::exception& error) {
        RaftCommandResult failure;
        failure.status = RaftRequestStatus::kRejected;
        failure.detail =
            std::string("command encoding failed: ") + error.what();
        fail_all(std::move(failure));
        return;
    }

    std::vector<nuraft::ulong> log_indexes;
    log_indexes.reserve(batch.size());
    nuraft::raft_server::req_ext_params ext;
    ext.expected_term_ = server->get_term();
    ext.after_precommit_ =
        [&log_indexes](
            const nuraft::raft_server::req_ext_cb_params& params) {
            log_indexes.push_back(params.log_idx);
        };

    auto result = server->append_entries_ext(logs, ext);
    if (!result) {
        fail_all(unavailable_result(
            "NuRaft returned no command result"));
        return;
    }
    if (!result->get_accepted()) {
        RaftCommandResult failure;
        failure.status = map_status(result->get_result_code());
        failure.detail = result->get_result_str();
        fail_all(std::move(failure));
        return;
    }

    const auto committed = result->get();
    const auto code = result->get_result_code();
    if (code != nuraft::cmd_result_code::OK || !committed) {
        RaftCommandResult failure;
        failure.status = map_status(code);
        failure.detail = result->get_result_str();
        if (failure.detail.empty() && !committed) {
            failure.detail =
                "committed batch has no state-machine result";
        }
        fail_all(std::move(failure));
        return;
    }
    if (log_indexes.size() != batch.size()) {
        fail_all(unavailable_result(
            "NuRaft returned incomplete batch indexes"));
        return;
    }

    for (std::size_t index = 0; index < batch.size(); ++index) {
        RaftCommandResult output;
        auto applied = state_machine->take_result(log_indexes[index]);
        if (!applied) {
            output = unavailable_result(
                "state-machine result cache missed committed index " +
                std::to_string(log_indexes[index]));
        } else {
            output.status = RaftRequestStatus::kOk;
            output.result = std::move(*applied);
        }
        batch[index]->completion.set_value(std::move(output));
    }
}

void RaftNode::read_worker() {
    for (;;) {
        std::vector<std::shared_ptr<PendingRead>> batch;
        {
            std::unique_lock<std::mutex> lock(read_mutex_);
            read_cv_.wait(lock, [this] {
                return read_stopping_ || !read_queue_.empty();
            });
            if (read_stopping_ && read_queue_.empty()) {
                return;
            }
            const auto deadline =
                std::chrono::steady_clock::now() + kReadBatchDelay;
            while (!read_stopping_ &&
                   read_queue_.size() < kMaximumReadBatch &&
                   read_cv_.wait_until(lock, deadline) !=
                       std::cv_status::timeout) {
            }
            const auto count =
                std::min(read_queue_.size(), kMaximumReadBatch);
            batch.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                batch.push_back(std::move(read_queue_.front()));
                read_queue_.pop_front();
            }
        }

        Command barrier;
        barrier.type = CommandType::kReadBarrier;
        read_batches_.fetch_add(1, std::memory_order_relaxed);
        read_requests_.fetch_add(
            static_cast<std::uint64_t>(batch.size()),
            std::memory_order_relaxed);
        const auto barrier_result = submit(barrier);
        if (barrier_result.status != RaftRequestStatus::kOk) {
            for (const auto& pending : batch) {
                LinearizableGetResult output;
                output.status = barrier_result.status;
                output.detail = barrier_result.detail;
                pending->completion.set_value(std::move(output));
            }
            continue;
        }

        auto state_machine = state_machine_;
        if (!state_machine) {
            for (const auto& pending : batch) {
                LinearizableGetResult output;
                output.detail = "state machine is unavailable";
                pending->completion.set_value(std::move(output));
            }
            continue;
        }
        std::vector<std::string> keys;
        keys.reserve(batch.size());
        for (const auto& pending : batch) {
            keys.push_back(pending->key);
        }
        auto values = state_machine->get_many(keys);
        for (std::size_t index = 0; index < batch.size(); ++index) {
            LinearizableGetResult output;
            output.status = RaftRequestStatus::kOk;
            output.value = std::move(values[index]);
            batch[index]->completion.set_value(std::move(output));
        }
    }
}

RaftCommandResult RaftNode::unavailable_result(
        std::string detail) {
    RaftCommandResult output;
    output.detail = std::move(detail);
    return output;
}

bool RaftNode::is_leader() const {
    auto server = server_;
    return running_.load() && server && server->is_leader();
}

std::int32_t RaftNode::leader_id() const {
    auto server = server_;
    return server ? server->get_leader() : -1;
}

std::optional<std::string>
RaftNode::leader_client_endpoint() const {
    const auto id = leader_id();
    if (id < 0) {
        return std::nullopt;
    }
    const auto* node = config_.find_node(id);
    if (!node) {
        return std::nullopt;
    }
    return node->client_endpoint();
}

RaftNodeInfo RaftNode::info() const {
    RaftNodeInfo output;
    output.node_id = config_.node_id;
    auto server = server_;
    auto state_machine = state_machine_;
    if (!server) {
        return output;
    }
    output.initialized = server->is_initialized();
    output.leader = server->is_leader();
    output.leader_id = server->get_leader();
    output.term = server->get_term();
    output.commit_index = server->get_committed_log_idx();
    output.last_log_index = server->get_last_log_idx();
    output.last_applied =
        state_machine ? state_machine->last_commit_index() : 0;
    output.snapshot_index = server->get_last_snapshot_idx();
    std::vector<nuraft::ptr<nuraft::srv_config>> servers;
    server->get_srv_config_all(servers);
    output.cluster_size = servers.size();
    output.proposal_batches =
        proposal_batches_.load(std::memory_order_relaxed);
    output.proposal_entries =
        proposal_entries_.load(std::memory_order_relaxed);
    output.read_batches =
        read_batches_.load(std::memory_order_relaxed);
    output.read_requests =
        read_requests_.load(std::memory_order_relaxed);
    return output;
}

int RaftNode::fatal_storage_error() const noexcept {
    auto state_manager = state_manager_;
    return state_manager ? state_manager->fatal_error() : 0;
}

nuraft::cb_func::ReturnCode RaftNode::on_raft_event(
        nuraft::cb_func::Type type, nuraft::cb_func::Param* param) {
    if (!logger_) {
        return nuraft::cb_func::Ok;
    }
    const std::int32_t leader = param ? param->leaderId : -1;
    switch (type) {
    case nuraft::cb_func::BecomeLeader:
    case nuraft::cb_func::BecomeFollower: {
        const auto term =
            (param && param->ctx)
                ? *static_cast<nuraft::ulong*>(param->ctx)
                : 0;
        logger_->info(
            "role change role=" +
            role_description(type == nuraft::cb_func::BecomeLeader) +
            " leader_id=" + std::to_string(leader) +
            " term=" + std::to_string(term));
        break;
    }
    case nuraft::cb_func::SnapshotCreationBegin: {
        const auto index =
            (param && param->ctx)
                ? *static_cast<std::uint64_t*>(param->ctx)
                : 0;
        logger_->info("snapshot creation begin index=" +
                      std::to_string(index));
        break;
    }
    case nuraft::cb_func::NewConfig:
        logger_->info("Raft cluster configuration committed");
        break;
    default:
        break;
    }
    return nuraft::cb_func::Ok;
}

RaftRequestStatus RaftNode::map_status(
        nuraft::cmd_result_code code) {
    switch (code) {
    case nuraft::cmd_result_code::OK:
        return RaftRequestStatus::kOk;
    case nuraft::cmd_result_code::NOT_LEADER:
    case nuraft::cmd_result_code::TERM_MISMATCH:
        return RaftRequestStatus::kNotLeader;
    case nuraft::cmd_result_code::TIMEOUT:
        return RaftRequestStatus::kTimeout;
    case nuraft::cmd_result_code::CANCELLED:
    case nuraft::cmd_result_code::RESULT_NOT_EXIST_YET:
    case nuraft::cmd_result_code::FAILED:
        return RaftRequestStatus::kUnavailable;
    default:
        return RaftRequestStatus::kRejected;
    }
}

const char* raft_request_status_name(
        RaftRequestStatus status) noexcept {
    switch (status) {
    case RaftRequestStatus::kOk:
        return "OK";
    case RaftRequestStatus::kNotLeader:
        return "NOT_LEADER";
    case RaftRequestStatus::kTimeout:
        return "TIMEOUT";
    case RaftRequestStatus::kUnavailable:
        return "UNAVAILABLE";
    case RaftRequestStatus::kRejected:
        return "REJECTED";
    }
    return "UNKNOWN";
}

}  // namespace strongkv
