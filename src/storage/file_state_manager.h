#pragma once

#include <libnuraft/state_mgr.hxx>

#include <strongkv/config.h>

#include <atomic>
#include <filesystem>
#include <mutex>

namespace strongkv {

class FileLogStore;
class Logger;

// Persists NuRaft cluster configuration and server term/vote state.
class FileStateManager final : public nuraft::state_mgr {
public:
    FileStateManager(const Config& config,
                     nuraft::ptr<FileLogStore> log_store,
                     nuraft::ptr<Logger> logger);

    nuraft::ptr<nuraft::cluster_config> load_config() override;
    void save_config(const nuraft::cluster_config& config) override;
    void save_state(const nuraft::srv_state& state) override;
    nuraft::ptr<nuraft::srv_state> read_state() override;
    nuraft::ptr<nuraft::log_store> load_log_store() override;
    nuraft::int32 server_id() override;
    void system_exit(int exit_code) override;

    int fatal_error() const noexcept { return fatal_error_.load(); }

private:
    nuraft::ptr<nuraft::cluster_config> initial_config() const;

    Config config_;
    nuraft::ptr<FileLogStore> log_store_;
    nuraft::ptr<Logger> logger_;
    std::filesystem::path config_file_;
    std::filesystem::path state_file_;
    std::mutex config_mutex_;
    std::mutex state_mutex_;
    std::atomic<int> fatal_error_{0};
};

}  // namespace strongkv
