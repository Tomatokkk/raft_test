#include "server/command_service.h"

#include "logging/logger.h"
#include "protocol/resp.h"
#include "raft/raft_node.h"
#include "security/password.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace strongkv {
namespace {

std::string uppercase_ascii(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
    return value;
}

std::uint64_t parse_request_number(
        const std::string& text, const char* name) {
    if (text.empty()) {
        throw std::invalid_argument(
            std::string(name) + " must be a positive integer");
    }
    std::uint64_t value = 0;
    for (const unsigned char c : text) {
        if (c < '0' || c > '9') {
            throw std::invalid_argument(
                std::string(name) + " must be a positive integer");
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            throw std::invalid_argument(
                std::string(name) + " is too large");
        }
        value = value * 10U + digit;
    }
    if (value == 0) {
        throw std::invalid_argument(
            std::string(name) + " must be non-zero");
    }
    return value;
}

bool write_command(const std::string& name) {
    return name == "SET" || name == "DEL" ||
           name == "INCR" || name == "DECR";
}

CommandType command_type(const std::string& name) {
    if (name == "SET") {
        return CommandType::kSet;
    }
    if (name == "DEL") {
        return CommandType::kDel;
    }
    if (name == "INCR") {
        return CommandType::kIncr;
    }
    if (name == "DECR") {
        return CommandType::kDecr;
    }
    throw std::logic_error("not a write command");
}

std::string apply_error(const ApplyResult& result) {
    switch (result.code) {
    case ResultCode::kNotInteger:
        return encode_error(
            "ERR value is not an integer or out of range");
    case ResultCode::kOverflow:
        return encode_error("ERR increment or decrement would overflow");
    case ResultCode::kStaleRequest:
        return encode_error("STALE_REQUEST " + result.value);
    case ResultCode::kBadCommand:
        return encode_error("ERR " + result.value);
    case ResultCode::kStorageError:
        return encode_error("STORAGE_ERROR " + result.value);
    case ResultCode::kNotFound:
        return encode_null_bulk_string();
    case ResultCode::kOk:
        break;
    }
    return encode_error("ERR unexpected state-machine result");
}

}  // namespace

CommandService::CommandService(
        const Config& config, RaftNode& raft,
        nuraft::ptr<Logger> logger)
    : config_(config),
      raft_(raft),
      logger_(std::move(logger)),
      started_at_(std::chrono::steady_clock::now()) {}

std::string CommandService::execute(
        const std::vector<std::string>& arguments,
        ClientSession& session) {
    if (arguments.empty()) {
        return encode_error("ERR empty command");
    }
    const std::string name = uppercase_ascii(arguments.front());

    if (name == "PING") {
        if (arguments.size() == 1) {
            return encode_simple_string("PONG");
        }
        if (arguments.size() == 2) {
            return encode_bulk_string(arguments[1]);
        }
        return encode_error(
            "ERR wrong number of arguments for 'ping'");
    }

    if (name == "AUTH") {
        if (arguments.size() != 2) {
            return encode_error(
                "ERR wrong number of arguments for 'auth'");
        }
        session.authenticated = constant_time_password_equal(
            arguments[1], config_.requirepass);
        if (logger_) {
            logger_->log(
                session.authenticated ? LogLevel::kInfo : LogLevel::kWarn,
                std::string("AUTH ") +
                    (session.authenticated ? "success" : "failure"));
        }
        return session.authenticated
            ? encode_simple_string("OK")
            : encode_error(
                  "WRONGPASS invalid username-password pair");
    }

    if (!session.authenticated) {
        return encode_error("NOAUTH Authentication required");
    }
    return execute_authenticated(arguments);
}

std::string CommandService::execute_authenticated(
        const std::vector<std::string>& arguments) {
    const std::string name = uppercase_ascii(arguments.front());
    if (write_command(name)) {
        return execute_write(arguments);
    }
    if (name == "GET") {
        if (arguments.size() != 2) {
            return encode_error(
                "ERR wrong number of arguments for 'get'");
        }
        if (logger_) {
            logger_->debug("command=GET key=" + arguments[1]);
        }
        const auto result = raft_.linearizable_get(arguments[1]);
        if (result.status == RaftRequestStatus::kNotLeader) {
            return not_leader_error();
        }
        if (result.status != RaftRequestStatus::kOk) {
            return raft_failure_error(
                static_cast<int>(result.status), result.detail);
        }
        return result.value
            ? encode_bulk_string(*result.value)
            : encode_null_bulk_string();
    }
    if (name == "INFO") {
        if (arguments.size() != 1) {
            return encode_error(
                "ERR wrong number of arguments for 'info'");
        }
        return info_response();
    }
    if (name == "ROLE") {
        if (arguments.size() != 1) {
            return encode_error(
                "ERR wrong number of arguments for 'role'");
        }
        return role_response();
    }
    return encode_error("ERR unknown command '" + arguments.front() + "'");
}

std::string CommandService::execute_write(
        const std::vector<std::string>& arguments) {
    const std::string name = uppercase_ascii(arguments.front());
    const std::size_t base_size = name == "SET" ? 3U : 2U;
    if (arguments.size() != base_size &&
        arguments.size() != base_size + 4U) {
        return encode_error(
            "ERR wrong number of arguments for '" +
            uppercase_ascii(arguments.front()) + "'");
    }

    Command command;
    command.type = command_type(name);
    command.key = arguments[1];
    if (name == "SET") {
        command.value = arguments[2];
    }

    if (arguments.size() == base_size + 4U) {
        if (uppercase_ascii(arguments[base_size]) != "CLIENT" ||
            uppercase_ascii(arguments[base_size + 2U]) != "REQUEST") {
            return encode_error(
                "ERR expected CLIENT <id> REQUEST <id>");
        }
        try {
            command.client_id = parse_request_number(
                arguments[base_size + 1U], "client_id");
            command.request_id = parse_request_number(
                arguments[base_size + 3U], "request_id");
        } catch (const std::invalid_argument& error) {
            return encode_error(std::string("ERR ") + error.what());
        }
    }

    if (logger_) {
        logger_->debug("command=" + name + " key=" + command.key);
    }
    const auto result = raft_.submit(command);
    if (result.status == RaftRequestStatus::kNotLeader) {
        return not_leader_error();
    }
    if (result.status != RaftRequestStatus::kOk) {
        if (logger_) {
            logger_->warn(
                "Raft append failed command=" + name +
                " status=" + raft_request_status_name(result.status));
        }
        return raft_failure_error(
            static_cast<int>(result.status), result.detail);
    }
    if (result.result.code != ResultCode::kOk) {
        return apply_error(result.result);
    }
    if (name == "SET") {
        return encode_simple_string("OK");
    }
    if (result.result.kind != ResultKind::kInteger) {
        return encode_error("ERR state machine returned wrong result type");
    }
    return encode_integer(result.result.integer);
}

std::string CommandService::not_leader_error() const {
    const auto endpoint = raft_.leader_client_endpoint();
    if (!endpoint) {
        return encode_error("NOT_LEADER");
    }
    const auto separator = endpoint->rfind(':');
    if (separator == std::string::npos) {
        return encode_error("NOT_LEADER");
    }
    return encode_error(
        "NOT_LEADER " + endpoint->substr(0, separator) + " " +
        endpoint->substr(separator + 1));
}

std::string CommandService::raft_failure_error(
        int status, const std::string& detail) const {
    const auto typed = static_cast<RaftRequestStatus>(status);
    std::string message = raft_request_status_name(typed);
    if (!detail.empty()) {
        message += " " + detail;
    }
    return encode_error(message);
}

std::string CommandService::info_response() const {
    const auto raft = raft_.info();
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started_at_).count();
    std::ostringstream output;
    output << "node_id:" << raft.node_id << "\r\n"
           << "role:" << (raft.leader ? "leader" : "follower") << "\r\n"
           << "leader_id:" << raft.leader_id << "\r\n"
           << "term:" << raft.term << "\r\n"
           << "commit_index:" << raft.commit_index << "\r\n"
           << "last_log_index:" << raft.last_log_index << "\r\n"
           << "last_applied:" << raft.last_applied << "\r\n"
           << "snapshot_index:" << raft.snapshot_index << "\r\n"
           << "cluster_size:" << raft.cluster_size << "\r\n"
           << "proposal_batches:" << raft.proposal_batches << "\r\n"
           << "proposal_entries:" << raft.proposal_entries << "\r\n"
           << "read_batches:" << raft.read_batches << "\r\n"
           << "read_requests:" << raft.read_requests << "\r\n"
           << "client_connections:" << client_connections() << "\r\n"
           << "uptime_seconds:" << uptime << "\r\n";
    return encode_bulk_string(output.str());
}

std::string CommandService::role_response() const {
    if (raft_.is_leader()) {
        return encode_array({"leader"});
    }
    const auto endpoint = raft_.leader_client_endpoint();
    if (endpoint) {
        return encode_array({"follower", *endpoint});
    }
    return encode_array({"follower", ""});
}

void CommandService::client_connected() noexcept {
    client_connections_.fetch_add(1);
}

void CommandService::client_disconnected() noexcept {
    client_connections_.fetch_sub(1);
}

std::size_t CommandService::client_connections() const noexcept {
    return client_connections_.load();
}

}  // namespace strongkv
