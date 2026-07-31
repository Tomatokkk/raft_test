#include "protocol/resp.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

namespace strongkv {

RespValue RespValue::simple(std::string text) {
    RespValue value;
    value.type = RespType::kSimpleString;
    value.text = std::move(text);
    return value;
}

RespValue RespValue::error(std::string text) {
    RespValue value;
    value.type = RespType::kError;
    value.text = std::move(text);
    return value;
}

RespValue RespValue::integer_value(std::int64_t integer) {
    RespValue value;
    value.type = RespType::kInteger;
    value.integer = integer;
    return value;
}

RespValue RespValue::bulk(std::string text) {
    RespValue value;
    value.type = RespType::kBulkString;
    value.text = std::move(text);
    return value;
}

RespValue RespValue::null_bulk() {
    RespValue value;
    value.type = RespType::kNullBulkString;
    return value;
}

RespValue RespValue::array_value(std::vector<RespValue> values) {
    RespValue value;
    value.type = RespType::kArray;
    value.array = std::move(values);
    return value;
}

RespParser::RespParser(std::size_t max_request_size,
                       std::size_t max_depth)
    : max_request_size_(max_request_size), max_depth_(max_depth) {
    if (max_request_size_ < 16 || max_depth_ == 0) {
        throw std::invalid_argument("invalid RESP parser limits");
    }
}

void RespParser::feed(std::string_view data) {
    if (data.size() > max_request_size_ - buffer_.size()) {
        throw ProtocolError("request exceeds max_request_size");
    }
    buffer_.append(data.data(), data.size());
}

std::optional<RespValue> RespParser::next() {
    if (buffer_.empty()) {
        return std::nullopt;
    }
    auto parsed = parse_value(0, 0);
    if (!parsed) {
        return std::nullopt;
    }
    RespValue value = std::move(parsed->value);
    buffer_.erase(0, parsed->next_offset);
    return value;
}

std::optional<std::pair<std::string, std::size_t>>
RespParser::read_line(std::size_t offset) const {
    const auto end = buffer_.find("\r\n", offset);
    const auto bare_newline = buffer_.find('\n', offset);
    if (bare_newline != std::string::npos &&
        (end == std::string::npos || bare_newline < end + 1)) {
        throw ProtocolError("RESP line uses LF without CRLF");
    }
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return std::make_pair(
        buffer_.substr(offset, end - offset), end + 2);
}

std::int64_t RespParser::parse_number(
        const std::string& text, const char* context) const {
    if (text.empty()) {
        throw ProtocolError(std::string("empty ") + context);
    }
    std::size_t position = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        position = 1;
    } else if (text[0] == '+') {
        throw ProtocolError(std::string("plus sign in ") + context);
    }
    if (position == text.size()) {
        throw ProtocolError(std::string("invalid ") + context);
    }

    const std::uint64_t limit = negative
        ? static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) + 1U
        : static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max());
    std::uint64_t value = 0;
    for (; position < text.size(); ++position) {
        const unsigned char c =
            static_cast<unsigned char>(text[position]);
        if (c < '0' || c > '9') {
            throw ProtocolError(std::string("non-decimal ") + context);
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (limit - digit) / 10U) {
            throw ProtocolError(std::string("overflowing ") + context);
        }
        value = value * 10U + digit;
    }
    if (!negative) {
        return static_cast<std::int64_t>(value);
    }
    if (value == limit) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(value);
}

std::optional<RespParser::Parsed> RespParser::parse_value(
        std::size_t offset, std::size_t depth) const {
    if (depth > max_depth_) {
        throw ProtocolError("RESP nesting exceeds limit");
    }
    if (offset >= buffer_.size()) {
        return std::nullopt;
    }
    const char prefix = buffer_[offset++];

    if (prefix == '+' || prefix == '-' || prefix == ':') {
        const auto line = read_line(offset);
        if (!line) {
            return std::nullopt;
        }
        RespValue value;
        if (prefix == '+') {
            value = RespValue::simple(line->first);
        } else if (prefix == '-') {
            value = RespValue::error(line->first);
        } else {
            value = RespValue::integer_value(
                parse_number(line->first, "RESP integer"));
        }
        return Parsed{std::move(value), line->second};
    }

    if (prefix == '$') {
        const auto line = read_line(offset);
        if (!line) {
            return std::nullopt;
        }
        const std::int64_t signed_length =
            parse_number(line->first, "bulk length");
        if (signed_length == -1) {
            return Parsed{RespValue::null_bulk(), line->second};
        }
        if (signed_length < 0 ||
            static_cast<std::uint64_t>(signed_length) >
                max_request_size_) {
            throw ProtocolError("invalid bulk string length");
        }
        const auto length = static_cast<std::size_t>(signed_length);
        if (line->second > buffer_.size() ||
            length > buffer_.size() - line->second) {
            return std::nullopt;
        }
        const std::size_t data_end = line->second + length;
        if (buffer_.size() - data_end < 2) {
            return std::nullopt;
        }
        if (buffer_[data_end] != '\r' ||
            buffer_[data_end + 1] != '\n') {
            throw ProtocolError("bulk string lacks trailing CRLF");
        }
        return Parsed{
            RespValue::bulk(buffer_.substr(line->second, length)),
            data_end + 2};
    }

    if (prefix == '*') {
        const auto line = read_line(offset);
        if (!line) {
            return std::nullopt;
        }
        const std::int64_t signed_count =
            parse_number(line->first, "array length");
        if (signed_count < 0 ||
            static_cast<std::uint64_t>(signed_count) >
                max_request_size_ / 3U) {
            throw ProtocolError("invalid RESP array length");
        }

        std::vector<RespValue> values;
        values.reserve(static_cast<std::size_t>(signed_count));
        std::size_t next_offset = line->second;
        for (std::int64_t i = 0; i < signed_count; ++i) {
            auto child = parse_value(next_offset, depth + 1);
            if (!child) {
                return std::nullopt;
            }
            values.push_back(std::move(child->value));
            next_offset = child->next_offset;
        }
        return Parsed{
            RespValue::array_value(std::move(values)), next_offset};
    }

    throw ProtocolError("unknown RESP type prefix");
}

std::string encode_resp(const RespValue& value) {
    switch (value.type) {
    case RespType::kSimpleString:
        return encode_simple_string(value.text);
    case RespType::kError:
        return encode_error(value.text);
    case RespType::kInteger:
        return encode_integer(value.integer);
    case RespType::kBulkString:
        return encode_bulk_string(value.text);
    case RespType::kNullBulkString:
        return encode_null_bulk_string();
    case RespType::kArray: {
        std::string output =
            "*" + std::to_string(value.array.size()) + "\r\n";
        for (const auto& child : value.array) {
            output += encode_resp(child);
        }
        return output;
    }
    }
    throw std::logic_error("unknown RESP value type");
}

std::string encode_simple_string(const std::string& value) {
    if (value.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument("simple string contains newline");
    }
    return "+" + value + "\r\n";
}

std::string encode_error(const std::string& value) {
    if (value.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument("RESP error contains newline");
    }
    return "-" + value + "\r\n";
}

std::string encode_integer(std::int64_t value) {
    return ":" + std::to_string(value) + "\r\n";
}

std::string encode_bulk_string(const std::string& value) {
    return "$" + std::to_string(value.size()) + "\r\n" +
           value + "\r\n";
}

std::string encode_null_bulk_string() {
    return "$-1\r\n";
}

std::string encode_array(const std::vector<std::string>& values) {
    std::string output = "*" + std::to_string(values.size()) + "\r\n";
    for (const auto& value : values) {
        output += encode_bulk_string(value);
    }
    return output;
}

std::vector<std::string> command_arguments(const RespValue& value) {
    if (value.type != RespType::kArray || value.array.empty()) {
        throw ProtocolError("command must be a non-empty RESP array");
    }
    std::vector<std::string> output;
    output.reserve(value.array.size());
    for (const auto& item : value.array) {
        if (item.type != RespType::kBulkString &&
            item.type != RespType::kSimpleString) {
            throw ProtocolError(
                "command arguments must be bulk or simple strings");
        }
        output.push_back(item.text);
    }
    return output;
}

}  // namespace strongkv
