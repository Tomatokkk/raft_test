#include "storage/file_state_manager.h"

#include "logging/logger.h"
#include "storage/file_log_store.h"
#include "storage/file_util.h"

#include <libnuraft/cluster_config.hxx>
#include <libnuraft/srv_config.hxx>
#include <libnuraft/srv_state.hxx>

#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace strongkv {
namespace {

constexpr FileMagic kConfigMagic{{'S', 'K', 'C', 'F'}};
constexpr FileMagic kStateMagic{{'S', 'K', 'R', 'S'}};
constexpr std::size_t kMaximumMetadataSize = 16 * 1024 * 1024;

std::vector<std::uint8_t> to_bytes(const nuraft::buffer& buffer) {
    const auto* begin = buffer.data_begin();
    return {begin, begin + buffer.size()};
}

nuraft::ptr<nuraft::buffer> to_buffer(
        const std::vector<std::uint8_t>& bytes) {
    auto buffer = nuraft::buffer::alloc(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(buffer->data_begin(), bytes.data(), bytes.size());
    }
    buffer->pos(0);
    return buffer;
}

}  // namespace

FileStateManager::FileStateManager(
        const Config& config,
        nuraft::ptr<FileLogStore> log_store,
        nuraft::ptr<Logger> logger)
    : config_(config),
      log_store_(std::move(log_store)),
      logger_(std::move(logger)),
      config_file_(config.raft_log_dir / "config.bin"),
      state_file_(config.raft_log_dir / "state.bin") {
    if (!log_store_) {
        throw std::invalid_argument("FileStateManager requires log store");
    }
}

nuraft::ptr<nuraft::cluster_config>
FileStateManager::initial_config() const {
    auto result = nuraft::cs_new<nuraft::cluster_config>();
    for (const auto& node : config_.cluster_nodes) {
        result->get_servers().push_back(
            nuraft::cs_new<nuraft::srv_config>(
                node.id, 0, node.raft_endpoint(), node.client_endpoint(),
                false, static_cast<nuraft::int32>(1)));
    }
    return result;
}

nuraft::ptr<nuraft::cluster_config>
FileStateManager::load_config() {
    std::lock_guard<std::mutex> lock(config_mutex_);
    if (!std::filesystem::exists(config_file_)) {
        auto created = initial_config();
        auto encoded = created->serialize();
        write_checked_record(config_file_, kConfigMagic, to_bytes(*encoded));
        return created;
    }
    auto bytes = read_checked_record(
        config_file_, kConfigMagic, kMaximumMetadataSize);
    auto buffer = to_buffer(bytes);
    return nuraft::cluster_config::deserialize(*buffer);
}

void FileStateManager::save_config(
        const nuraft::cluster_config& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    auto encoded = config.serialize();
    write_checked_record(config_file_, kConfigMagic, to_bytes(*encoded));
}

void FileStateManager::save_state(const nuraft::srv_state& state) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto encoded = state.serialize();
    write_checked_record(state_file_, kStateMagic, to_bytes(*encoded));
}

nuraft::ptr<nuraft::srv_state> FileStateManager::read_state() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!std::filesystem::exists(state_file_)) {
        return nullptr;
    }
    auto bytes = read_checked_record(
        state_file_, kStateMagic, kMaximumMetadataSize);
    auto buffer = to_buffer(bytes);
    return nuraft::srv_state::deserialize(*buffer);
}

nuraft::ptr<nuraft::log_store>
FileStateManager::load_log_store() {
    return log_store_;
}

nuraft::int32 FileStateManager::server_id() {
    return config_.node_id;
}

void FileStateManager::system_exit(int exit_code) {
    fatal_error_.store(exit_code);
    if (logger_) {
        logger_->err("NuRaft requested system exit, code=" +
                     std::to_string(exit_code));
    }
}

}  // namespace strongkv
