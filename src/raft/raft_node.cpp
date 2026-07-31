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
    const bool graceful = launcher_->shutdown(10);
    if (log_store_) {
        log_store_->flush();
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
    RaftCommandResult output;
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

    const auto term = server->get_term();
    nuraft::raft_server::req_ext_params ext;
    ext.expected_term_ = term;
    std::vector<nuraft::ptr<nuraft::buffer>> logs{
        encode_command(command)};
    auto result = server->append_entries_ext(logs, ext);
    if (!result) {
        output.status = RaftRequestStatus::kUnavailable;
        output.detail = "NuRaft returned no command result";
        return output;
    }
    if (!result->get_accepted()) {
        output.status = map_status(result->get_result_code());
        output.detail = result->get_result_str();
        return output;
    }

    const auto code = result->get_result_code();
    output.status = map_status(code);
    if (code != nuraft::cmd_result_code::OK) {
        output.detail = result->get_result_str();
        return output;
    }
    auto committed = result->get();
    if (!committed) {
        output.status = RaftRequestStatus::kUnavailable;
        output.detail = "committed entry has no state-machine result";
        return output;
    }

    try {
        output.result =
            decode_result(*committed, config_.max_request_size);
        output.status = RaftRequestStatus::kOk;
    } catch (const std::exception& error) {
        output.status = RaftRequestStatus::kUnavailable;
        output.detail =
            std::string("invalid state-machine result: ") + error.what();
        logger_->err(output.detail);
    }
    return output;
}

LinearizableGetResult RaftNode::linearizable_get(
        const std::string& key) {
    LinearizableGetResult output;
    Command barrier;
    barrier.type = CommandType::kReadBarrier;
    const auto barrier_result = submit(barrier);
    output.status = barrier_result.status;
    output.detail = barrier_result.detail;
    if (barrier_result.status != RaftRequestStatus::kOk) {
        return output;
    }

    // The barrier was committed by a majority in the current term before
    // this local read. An isolated former leader cannot complete it.
    auto state_machine = state_machine_;
    if (!state_machine) {
        output.status = RaftRequestStatus::kUnavailable;
        output.detail = "state machine is unavailable";
        return output;
    }
    output.value = state_machine->get(key);
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
