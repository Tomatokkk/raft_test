#include "state_machine/command.h"

#include "common/binary_codec.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace strongkv {
namespace {

constexpr std::array<std::uint8_t, 4> kCommandMagic{{'S', 'K', 'V', 'C'}};
constexpr std::array<std::uint8_t, 4> kResultMagic{{'S', 'K', 'V', 'R'}};
constexpr std::uint16_t kCommandVersion = 1;
constexpr std::uint16_t kResultVersion = 1;

void put_magic(BinaryWriter& writer,
               const std::array<std::uint8_t, 4>& magic) {
    writer.put_raw(magic.data(), magic.size());
}

void expect_magic(BinaryReader& reader,
                  const std::array<std::uint8_t, 4>& expected) {
    const auto actual = reader.get_bytes(expected.size());
    if (!std::equal(actual.begin(), actual.end(), expected.begin())) {
        throw DecodeError("invalid binary envelope magic");
    }
}

bool valid_command_type(std::uint8_t type) {
    return type >= static_cast<std::uint8_t>(CommandType::kSet) &&
           type <= static_cast<std::uint8_t>(CommandType::kReadBarrier);
}

bool valid_result_code(std::uint8_t code) {
    return code <= static_cast<std::uint8_t>(ResultCode::kStorageError);
}

bool valid_result_kind(std::uint8_t kind) {
    return kind <= static_cast<std::uint8_t>(ResultKind::kInteger);
}

nuraft::ptr<nuraft::buffer> vector_to_buffer(
        const std::vector<std::uint8_t>& bytes) {
    auto output = nuraft::buffer::alloc(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(output->data_begin(), bytes.data(), bytes.size());
    }
    output->pos(0);
    return output;
}

}  // namespace

ApplyResult ApplyResult::ok() {
    return {};
}

ApplyResult ApplyResult::string(std::string value) {
    ApplyResult result;
    result.kind = ResultKind::kString;
    result.value = std::move(value);
    return result;
}

ApplyResult ApplyResult::integer_value(std::int64_t value) {
    ApplyResult result;
    result.kind = ResultKind::kInteger;
    result.integer = value;
    return result;
}

ApplyResult ApplyResult::error(ResultCode code, std::string message) {
    ApplyResult result;
    result.code = code;
    result.kind = ResultKind::kString;
    result.value = std::move(message);
    return result;
}

bool ApplyResult::operator==(const ApplyResult& other) const {
    return code == other.code && kind == other.kind &&
           value == other.value && integer == other.integer;
}

std::vector<std::uint8_t> encode_command_bytes(const Command& command) {
    if (command.version != kCommandVersion) {
        throw std::invalid_argument("unsupported command version");
    }
    BinaryWriter writer;
    put_magic(writer, kCommandMagic);
    writer.put_u16(command.version);
    writer.put_u8(static_cast<std::uint8_t>(command.type));
    writer.put_u8(0);
    writer.put_u64(command.client_id);
    writer.put_u64(command.request_id);
    writer.put_string(command.key);
    writer.put_string(command.value);
    return writer.take();
}

Command decode_command_bytes(const void* data, std::size_t size,
                             std::size_t max_field_size) {
    BinaryReader reader(data, size);
    expect_magic(reader, kCommandMagic);

    Command command;
    command.version = reader.get_u16();
    if (command.version != kCommandVersion) {
        throw DecodeError("unsupported command version");
    }
    const std::uint8_t raw_type = reader.get_u8();
    if (!valid_command_type(raw_type)) {
        throw DecodeError("unknown command type");
    }
    command.type = static_cast<CommandType>(raw_type);
    if (reader.get_u8() != 0) {
        throw DecodeError("non-zero reserved command byte");
    }
    command.client_id = reader.get_u64();
    command.request_id = reader.get_u64();
    command.key = reader.get_string(max_field_size);
    command.value = reader.get_string(max_field_size);
    reader.require_end();
    return command;
}

nuraft::ptr<nuraft::buffer> encode_command(const Command& command) {
    return vector_to_buffer(encode_command_bytes(command));
}

Command decode_command(const nuraft::buffer& buffer,
                       std::size_t max_field_size) {
    return decode_command_bytes(buffer.data_begin(), buffer.size(),
                                max_field_size);
}

std::vector<std::uint8_t> encode_result_bytes(const ApplyResult& result) {
    BinaryWriter writer;
    put_magic(writer, kResultMagic);
    writer.put_u16(kResultVersion);
    writer.put_u8(static_cast<std::uint8_t>(result.code));
    writer.put_u8(static_cast<std::uint8_t>(result.kind));
    writer.put_i64(result.integer);
    writer.put_string(result.value);
    return writer.take();
}

ApplyResult decode_result_bytes(const void* data, std::size_t size,
                                std::size_t max_field_size) {
    BinaryReader reader(data, size);
    expect_magic(reader, kResultMagic);
    if (reader.get_u16() != kResultVersion) {
        throw DecodeError("unsupported result version");
    }
    const std::uint8_t raw_code = reader.get_u8();
    const std::uint8_t raw_kind = reader.get_u8();
    if (!valid_result_code(raw_code) || !valid_result_kind(raw_kind)) {
        throw DecodeError("invalid result enum");
    }

    ApplyResult result;
    result.code = static_cast<ResultCode>(raw_code);
    result.kind = static_cast<ResultKind>(raw_kind);
    result.integer = reader.get_i64();
    result.value = reader.get_string(max_field_size);
    reader.require_end();

    if (result.kind == ResultKind::kNone && !result.value.empty()) {
        throw DecodeError("result with no payload contains string data");
    }
    return result;
}

nuraft::ptr<nuraft::buffer> encode_result(const ApplyResult& result) {
    return vector_to_buffer(encode_result_bytes(result));
}

ApplyResult decode_result(const nuraft::buffer& buffer,
                          std::size_t max_field_size) {
    return decode_result_bytes(buffer.data_begin(), buffer.size(),
                               max_field_size);
}

const char* result_code_name(ResultCode code) noexcept {
    switch (code) {
    case ResultCode::kOk:
        return "OK";
    case ResultCode::kNotFound:
        return "NOT_FOUND";
    case ResultCode::kNotInteger:
        return "NOT_INTEGER";
    case ResultCode::kOverflow:
        return "OVERFLOW";
    case ResultCode::kStaleRequest:
        return "STALE_REQUEST";
    case ResultCode::kBadCommand:
        return "BAD_COMMAND";
    case ResultCode::kStorageError:
        return "STORAGE_ERROR";
    }
    return "UNKNOWN";
}

}  // namespace strongkv
