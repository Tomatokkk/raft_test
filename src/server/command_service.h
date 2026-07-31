#pragma once

#include <libnuraft/ptr.hxx>

#include <strongkv/config.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace strongkv {

class Logger;
class RaftNode;

struct ClientSession {
    bool authenticated{false};
};

// Stateless command semantics plus connection-scoped AUTH state. The return
// value is always a complete RESP2 frame.
class CommandService final {
public:
    CommandService(const Config& config, RaftNode& raft,
                   nuraft::ptr<Logger> logger);

    std::string execute(const std::vector<std::string>& arguments,
                        ClientSession& session);

    void client_connected() noexcept;
    void client_disconnected() noexcept;
    std::size_t client_connections() const noexcept;

private:
    std::string execute_authenticated(
        const std::vector<std::string>& arguments);
    std::string execute_write(
        const std::vector<std::string>& arguments);
    std::string not_leader_error() const;
    std::string raft_failure_error(int status,
                                   const std::string& detail) const;
    std::string info_response() const;
    std::string role_response() const;

    const Config& config_;
    RaftNode& raft_;
    nuraft::ptr<Logger> logger_;
    std::chrono::steady_clock::time_point started_at_;
    std::atomic<std::size_t> client_connections_{0};
};

}  // namespace strongkv
