#include <strongkv/client.h>

#include "protocol/resp.h"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

namespace strongkv {
namespace {

constexpr std::size_t kMaximumResponseSize = 16U * 1024U * 1024U;

struct Endpoint {
    std::string host;
    std::string port;

    std::string text() const { return host + ":" + port; }
};

Endpoint parse_endpoint(const std::string& text) {
    const auto separator = text.rfind(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= text.size()) {
        throw ClientError("invalid endpoint: " + text);
    }
    Endpoint result{text.substr(0, separator),
                    text.substr(separator + 1)};
    for (const unsigned char c : result.port) {
        if (c < '0' || c > '9') {
            throw ClientError("invalid endpoint port: " + text);
        }
    }
    const auto numeric = std::stoul(result.port);
    if (numeric == 0 || numeric > 65535) {
        throw ClientError("endpoint port out of range: " + text);
    }
    return result;
}

ClientValue convert_value(const RespValue& value) {
    ClientValue output;
    switch (value.type) {
    case RespType::kSimpleString:
        output.type = ClientValueType::kSimpleString;
        output.text = value.text;
        break;
    case RespType::kError:
        output.type = ClientValueType::kError;
        output.text = value.text;
        break;
    case RespType::kInteger:
        output.type = ClientValueType::kInteger;
        output.integer = value.integer;
        break;
    case RespType::kBulkString:
        output.type = ClientValueType::kBulkString;
        output.text = value.text;
        break;
    case RespType::kNullBulkString:
        output.type = ClientValueType::kNull;
        break;
    case RespType::kArray:
        output.type = ClientValueType::kArray;
        for (const auto& item : value.array) {
            output.array.push_back(convert_value(item));
        }
        break;
    }
    return output;
}

std::optional<Endpoint> redirect_from_error(
        const RespValue& value) {
    if (value.type != RespType::kError ||
        value.text.rfind("NOT_LEADER", 0) != 0) {
        return std::nullopt;
    }
    std::istringstream input(value.text);
    std::string marker;
    Endpoint endpoint;
    input >> marker >> endpoint.host >> endpoint.port;
    if (endpoint.host.empty() || endpoint.port.empty()) {
        return Endpoint{};
    }
    return parse_endpoint(endpoint.text());
}

void require_ok(const ClientValue& value, const char* operation) {
    if (value.type == ClientValueType::kError) {
        throw ClientError(value.text);
    }
    if (value.type != ClientValueType::kSimpleString ||
        value.text != "OK") {
        throw ClientError(
            std::string(operation) + " returned an unexpected value");
    }
}

std::int64_t require_integer(
        const ClientValue& value, const char* operation) {
    if (value.type == ClientValueType::kError) {
        throw ClientError(value.text);
    }
    if (value.type != ClientValueType::kInteger) {
        throw ClientError(
            std::string(operation) + " did not return an integer");
    }
    return value.integer;
}

std::uint64_t make_client_id() {
    std::random_device random;
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now()
            .time_since_epoch().count());
    const auto entropy =
        (static_cast<std::uint64_t>(random()) << 32U) ^
        static_cast<std::uint64_t>(random());
    const auto value = now ^ entropy;
    return value == 0 ? 1 : value;
}

}  // namespace

class Client::Impl {
public:
    explicit Impl(std::vector<std::string> seeds)
        : seeds_(std::move(seeds)),
          parser_(kMaximumResponseSize),
          client_id_(make_client_id()) {
        if (seeds_.empty()) {
            throw ClientError("at least one seed node is required");
        }
        for (const auto& seed : seeds_) {
            static_cast<void>(parse_endpoint(seed));
        }
    }

    void connect() {
        std::lock_guard<std::mutex> lock(mutex_);
        connect_any_locked();
    }

    void auth(const std::string& password) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!socket_ || !socket_->is_open()) {
            connect_any_locked();
        }
        auto result = request_once_locked({"AUTH", password});
        if (result.type == RespType::kError) {
            throw ClientError(result.text);
        }
        if (result.type != RespType::kSimpleString ||
            result.text != "OK") {
            throw ClientError("AUTH returned an unexpected response");
        }
        password_ = password;
    }

    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        close_locked();
    }

    ClientValue command(std::vector<std::string> arguments) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (arguments.empty()) {
            throw ClientError("empty command");
        }
        const bool write = is_write(arguments.front());
        if (write) {
            if (next_request_id_ ==
                std::numeric_limits<std::uint64_t>::max()) {
                throw ClientError("client request id exhausted");
            }
            ++next_request_id_;
            arguments.insert(
                arguments.end(),
                {"CLIENT", std::to_string(client_id_),
                 "REQUEST", std::to_string(next_request_id_)});
        }
        return convert_value(
            request_with_redirect_locked(arguments));
    }

private:
    static bool is_write(std::string name) {
        std::transform(
            name.begin(), name.end(), name.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
        return name == "SET" || name == "DEL" ||
               name == "INCR" || name == "DECR";
    }

    void connect_endpoint_locked(const Endpoint& endpoint) {
        close_locked();
        asio::ip::tcp::resolver resolver(io_context_);
        asio::error_code error;
        const auto addresses =
            resolver.resolve(endpoint.host, endpoint.port, error);
        if (error) {
            throw ClientError(
                "cannot resolve " + endpoint.text() +
                ": " + error.message());
        }
        socket_ =
            std::make_unique<asio::ip::tcp::socket>(io_context_);
        asio::connect(*socket_, addresses, error);
        if (error) {
            socket_.reset();
            throw ClientError(
                "cannot connect " + endpoint.text() +
                ": " + error.message());
        }
        current_ = endpoint.text();
        parser_.clear();
    }

    void connect_any_locked() {
        std::string failures;
        for (const auto& seed : seeds_) {
            try {
                connect_endpoint_locked(parse_endpoint(seed));
                if (!password_.empty()) {
                    authenticate_current_locked();
                }
                return;
            } catch (const ClientError& error) {
                failures += error.what();
                failures += "; ";
            }
        }
        throw ClientError("all seed nodes failed: " + failures);
    }

    void authenticate_current_locked() {
        const auto result =
            request_once_locked({"AUTH", password_});
        if (result.type == RespType::kError) {
            throw ClientError(result.text);
        }
        if (result.type != RespType::kSimpleString ||
            result.text != "OK") {
            throw ClientError("AUTH returned an unexpected response");
        }
    }

    RespValue request_with_redirect_locked(
            const std::vector<std::string>& arguments) {
        const std::size_t max_attempts = seeds_.size() + 3U;
        std::optional<Endpoint> directed;
        for (std::size_t attempt = 0;
             attempt < max_attempts; ++attempt) {
            try {
                if (!socket_ || !socket_->is_open()) {
                    if (directed && !directed->host.empty()) {
                        connect_endpoint_locked(*directed);
                        if (!password_.empty()) {
                            authenticate_current_locked();
                        }
                    } else {
                        connect_any_locked();
                    }
                }
                RespValue response =
                    request_once_locked(arguments);
                auto redirect = redirect_from_error(response);
                if (!redirect) {
                    return response;
                }
                close_locked();
                directed = redirect->host.empty()
                    ? std::optional<Endpoint>{}
                    : redirect;
            } catch (const ClientError&) {
                close_locked();
                directed.reset();
            }
            if (attempt + 1U < max_attempts) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }
        }
        throw ClientError(
            "unable to reach the current leader after retries");
    }

    RespValue request_once_locked(
            const std::vector<std::string>& arguments) {
        if (!socket_ || !socket_->is_open()) {
            throw ClientError("client is not connected");
        }
        const std::string request = encode_array(arguments);
        asio::error_code error;
        asio::write(*socket_, asio::buffer(request), error);
        if (error) {
            throw ClientError("write failed: " + error.message());
        }

        while (true) {
            try {
                if (auto value = parser_.next()) {
                    return std::move(*value);
                }
            } catch (const ProtocolError& protocol_error) {
                throw ClientError(
                    std::string("invalid server response: ") +
                    protocol_error.what());
            }
            std::array<char, 8192> data{};
            const auto count =
                socket_->read_some(asio::buffer(data), error);
            if (error) {
                throw ClientError("read failed: " + error.message());
            }
            try {
                parser_.feed(
                    std::string_view(data.data(), count));
            } catch (const ProtocolError& protocol_error) {
                throw ClientError(
                    std::string("invalid server response: ") +
                    protocol_error.what());
            }
        }
    }

    void close_locked() noexcept {
        if (socket_) {
            asio::error_code ignored;
            socket_->shutdown(
                asio::ip::tcp::socket::shutdown_both, ignored);
            socket_->close(ignored);
            socket_.reset();
        }
        parser_.clear();
        current_.clear();
    }

    std::vector<std::string> seeds_;
    asio::io_context io_context_;
    std::unique_ptr<asio::ip::tcp::socket> socket_;
    RespParser parser_;
    std::string current_;
    std::string password_;
    std::uint64_t client_id_;
    std::uint64_t next_request_id_{0};
    std::mutex mutex_;
};

Client::Client(std::vector<std::string> seed_nodes)
    : impl_(std::make_unique<Impl>(std::move(seed_nodes))) {}

Client::~Client() = default;

void Client::connect() {
    impl_->connect();
}

void Client::auth(const std::string& password) {
    impl_->auth(password);
}

void Client::close() noexcept {
    impl_->close();
}

void Client::set(const std::string& key, const std::string& value) {
    require_ok(command({"SET", key, value}), "SET");
}

std::optional<std::string> Client::get(const std::string& key) {
    const auto value = command({"GET", key});
    if (value.type == ClientValueType::kError) {
        throw ClientError(value.text);
    }
    if (value.type == ClientValueType::kNull) {
        return std::nullopt;
    }
    if (value.type != ClientValueType::kBulkString) {
        throw ClientError("GET returned an unexpected value");
    }
    return value.text;
}

std::int64_t Client::del(const std::string& key) {
    return require_integer(command({"DEL", key}), "DEL");
}

std::int64_t Client::incr(const std::string& key) {
    return require_integer(command({"INCR", key}), "INCR");
}

std::int64_t Client::decr(const std::string& key) {
    return require_integer(command({"DECR", key}), "DECR");
}

std::string Client::ping() {
    const auto value = command({"PING"});
    if (value.type == ClientValueType::kError) {
        throw ClientError(value.text);
    }
    if (value.type != ClientValueType::kSimpleString) {
        throw ClientError("PING returned an unexpected value");
    }
    return value.text;
}

std::vector<std::string> Client::role() {
    const auto value = command({"ROLE"});
    if (value.type == ClientValueType::kError) {
        throw ClientError(value.text);
    }
    if (value.type != ClientValueType::kArray) {
        throw ClientError("ROLE returned an unexpected value");
    }
    std::vector<std::string> output;
    for (const auto& item : value.array) {
        if (item.type != ClientValueType::kBulkString &&
            item.type != ClientValueType::kSimpleString) {
            throw ClientError("ROLE array contains a non-string value");
        }
        output.push_back(item.text);
    }
    return output;
}

std::string Client::info() {
    const auto value = command({"INFO"});
    if (value.type == ClientValueType::kError) {
        throw ClientError(value.text);
    }
    if (value.type != ClientValueType::kBulkString) {
        throw ClientError("INFO returned an unexpected value");
    }
    return value.text;
}

ClientValue Client::command(
        const std::vector<std::string>& arguments) {
    return impl_->command(arguments);
}

}  // namespace strongkv
