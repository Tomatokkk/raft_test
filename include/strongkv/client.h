#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace strongkv {

class ClientError : public std::runtime_error {
public:
    explicit ClientError(const std::string& message)
        : std::runtime_error(message) {}
};

enum class ClientValueType {
    kSimpleString,
    kError,
    kInteger,
    kBulkString,
    kNull,
    kArray,
};

struct ClientValue {
    ClientValueType type{ClientValueType::kSimpleString};
    std::string text;
    std::int64_t integer{0};
    std::vector<ClientValue> array;
};

// Synchronous SDK for BCS-style integration. It discovers the leader from
// NOT_LEADER responses, re-authenticates after reconnecting, and preserves
// client_id/request_id across retries of the same write.
class Client final {
public:
    explicit Client(std::vector<std::string> seed_nodes);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void connect();
    void auth(const std::string& password);
    void close() noexcept;

    void set(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    std::int64_t del(const std::string& key);
    std::int64_t incr(const std::string& key);
    std::int64_t decr(const std::string& key);
    std::string ping();
    std::vector<std::string> role();
    std::string info();

    ClientValue command(const std::vector<std::string>& arguments);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace strongkv
