#pragma once

#include <asio.hpp>

#include <libnuraft/ptr.hxx>

#include <strongkv/config.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace strongkv {

class CommandService;
class Logger;

// Blocking-per-connection prototype server. Session threads are owned and
// joined during shutdown; all sockets are closed to unblock reads first.
class TcpServer final {
public:
    TcpServer(const Config& config, CommandService& commands,
              nuraft::ptr<Logger> logger);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void start();
    void stop();
    bool running() const noexcept { return running_.load(); }

private:
    using Socket = asio::ip::tcp::socket;

    void accept_loop();
    void session_loop(std::uint64_t session_id,
                      std::shared_ptr<Socket> socket);
    void close_socket(const std::shared_ptr<Socket>& socket) noexcept;

    const Config& config_;
    CommandService& commands_;
    nuraft::ptr<Logger> logger_;

    asio::io_context io_context_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::thread accept_thread_;
    std::mutex sessions_mutex_;
    std::condition_variable sessions_cv_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Socket>> sockets_;
    std::atomic<std::uint64_t> next_session_id_{1};
    std::atomic<bool> running_{false};
};

}  // namespace strongkv
