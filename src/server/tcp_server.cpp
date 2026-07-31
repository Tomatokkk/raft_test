#include "server/tcp_server.h"

#include "logging/logger.h"
#include "protocol/resp.h"
#include "server/command_service.h"

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>

namespace strongkv {

TcpServer::TcpServer(
        const Config& config, CommandService& commands,
        nuraft::ptr<Logger> logger)
    : config_(config),
      commands_(commands),
      logger_(std::move(logger)) {}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::start() {
    if (running_.exchange(true)) {
        return;
    }
    try {
        const auto address =
            asio::ip::make_address(config_.bind_address);
        const asio::ip::tcp::endpoint endpoint(
            address, config_.client_port);
        acceptor_ =
            std::make_unique<asio::ip::tcp::acceptor>(io_context_);
        acceptor_->open(endpoint.protocol());
        acceptor_->set_option(
            asio::socket_base::reuse_address(true));
        acceptor_->bind(endpoint);
        acceptor_->listen(
            asio::socket_base::max_listen_connections);
        acceptor_->non_blocking(true);
        accept_thread_ =
            std::thread([this] { accept_loop(); });
        if (logger_) {
            logger_->info(
                "client server listening on " +
                config_.bind_address + ":" +
                std::to_string(config_.client_port));
        }
    } catch (...) {
        running_.store(false);
        acceptor_.reset();
        throw;
    }
}

void TcpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const auto& entry : sockets_) {
            close_socket(entry.second);
        }
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    asio::error_code ignored;
    if (acceptor_) {
        acceptor_->close(ignored);
    }
    {
        std::unique_lock<std::mutex> lock(sessions_mutex_);
        sessions_cv_.wait(
            lock, [this] { return sockets_.empty(); });
    }
    acceptor_.reset();
    if (logger_) {
        logger_->info("client server stopped");
    }
}

void TcpServer::accept_loop() {
    while (running_.load()) {
        auto socket = std::make_shared<Socket>(io_context_);
        asio::error_code error;
        acceptor_->accept(*socket, error);
        if (error) {
            if (error == asio::error::would_block ||
                error == asio::error::try_again) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
                continue;
            }
            if (running_.load() && logger_) {
                logger_->warn(
                    "client accept failed: " + error.message());
            }
            continue;
        }

        const auto session_id = next_session_id_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            if (sockets_.size() >= config_.max_connections) {
                asio::write(
                    *socket,
                    asio::buffer(encode_error(
                        "ERR max number of clients reached")),
                    error);
                close_socket(socket);
                continue;
            }
            sockets_.emplace(session_id, socket);
            try {
                std::thread(
                    [this, session_id, socket] {
                        session_loop(session_id, socket);
                    }).detach();
            } catch (const std::exception& exception) {
                sockets_.erase(session_id);
                close_socket(socket);
                if (logger_) {
                    logger_->err(
                        "cannot start client session thread: " +
                        std::string(exception.what()));
                }
            }
        }
    }
}

void TcpServer::session_loop(
        std::uint64_t session_id,
        std::shared_ptr<Socket> socket) {
    commands_.client_connected();
    ClientSession session;
    RespParser parser(config_.max_request_size);
    std::array<char, 8192> buffer{};
    asio::error_code error;

    std::string peer = "unknown";
    const auto remote = socket->remote_endpoint(error);
    if (!error) {
        peer = remote.address().to_string() + ":" +
               std::to_string(remote.port());
    }
    if (logger_) {
        logger_->info(
            "client connected session=" +
            std::to_string(session_id) + " peer=" + peer);
    }

    try {
        while (running_.load()) {
            const std::size_t count =
                socket->read_some(asio::buffer(buffer), error);
            if (error) {
                break;
            }
            parser.feed(std::string_view(buffer.data(), count));
            while (auto value = parser.next()) {
                std::string response;
                try {
                    response = commands_.execute(
                        command_arguments(*value), session);
                } catch (const ProtocolError& protocol_error) {
                    response = encode_error(
                        std::string("ERR Protocol error: ") +
                        protocol_error.what());
                }
                asio::write(*socket, asio::buffer(response), error);
                if (error) {
                    break;
                }
            }
            if (error) {
                break;
            }
        }
    } catch (const ProtocolError& protocol_error) {
        const std::string response = encode_error(
            std::string("ERR Protocol error: ") +
            protocol_error.what());
        asio::write(*socket, asio::buffer(response), error);
    } catch (const std::exception& exception) {
        if (logger_) {
            logger_->err(
                "client session error session=" +
                std::to_string(session_id) +
                " error=" + exception.what());
        }
    }

    close_socket(socket);
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sockets_.erase(session_id);
    }
    sessions_cv_.notify_all();
    commands_.client_disconnected();
    if (logger_) {
        logger_->info(
            "client disconnected session=" +
            std::to_string(session_id));
    }
}

void TcpServer::close_socket(
        const std::shared_ptr<Socket>& socket) noexcept {
    if (!socket) {
        return;
    }
    asio::error_code ignored;
    socket->shutdown(
        asio::ip::tcp::socket::shutdown_both, ignored);
    socket->close(ignored);
}

}  // namespace strongkv
