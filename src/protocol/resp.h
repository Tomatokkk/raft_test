#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace strongkv {

class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& message)
        : std::runtime_error(message) {}
};

enum class RespType {
    kSimpleString,
    kError,
    kInteger,
    kBulkString,
    kNullBulkString,
    kArray,
};

struct RespValue {
    RespType type{RespType::kSimpleString};
    std::string text;
    std::int64_t integer{0};
    std::vector<RespValue> array;

    static RespValue simple(std::string text);
    static RespValue error(std::string text);
    static RespValue integer_value(std::int64_t value);
    static RespValue bulk(std::string text);
    static RespValue null_bulk();
    static RespValue array_value(std::vector<RespValue> values);
};

// Incremental RESP2 parser. feed() and next() may be called with arbitrary TCP
// packet boundaries; next() consumes exactly one complete value.
class RespParser {
public:
    explicit RespParser(std::size_t max_request_size,
                        std::size_t max_depth = 16);

    void feed(std::string_view data);
    std::optional<RespValue> next();
    std::size_t buffered_size() const noexcept { return buffer_.size(); }
    void clear() noexcept { buffer_.clear(); }

private:
    struct Parsed {
        RespValue value;
        std::size_t next_offset;
    };

    std::optional<Parsed> parse_value(
        std::size_t offset, std::size_t depth) const;
    std::optional<std::pair<std::string, std::size_t>> read_line(
        std::size_t offset) const;
    std::int64_t parse_number(
        const std::string& text, const char* context) const;

    std::size_t max_request_size_;
    std::size_t max_depth_;
    std::string buffer_;
};

std::string encode_resp(const RespValue& value);
std::string encode_simple_string(const std::string& value);
std::string encode_error(const std::string& value);
std::string encode_integer(std::int64_t value);
std::string encode_bulk_string(const std::string& value);
std::string encode_null_bulk_string();
std::string encode_array(const std::vector<std::string>& values);

std::vector<std::string> command_arguments(const RespValue& value);

}  // namespace strongkv
