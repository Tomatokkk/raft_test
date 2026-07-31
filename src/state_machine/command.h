#pragma once

#include <libnuraft/nuraft.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace strongkv {

enum class CommandType : std::uint8_t {
    kSet = 1,
    kDel = 2,
    kIncr = 3,
    kDecr = 4,
    kReadBarrier = 5,
};

struct Command {
    std::uint16_t version{1};
    CommandType type{CommandType::kSet};
    std::string key;
    std::string value;
    std::uint64_t client_id{0};
    std::uint64_t request_id{0};
};

enum class ResultCode : std::uint8_t {
    kOk = 0,
    kNotFound = 1,
    kNotInteger = 2,
    kOverflow = 3,
    kStaleRequest = 4,
    kBadCommand = 5,
    kStorageError = 6,
};

enum class ResultKind : std::uint8_t {
    kNone = 0,
    kString = 1,
    kInteger = 2,
};

struct ApplyResult {
    ResultCode code{ResultCode::kOk};
    ResultKind kind{ResultKind::kNone};
    std::string value;
    std::int64_t integer{0};

    static ApplyResult ok();
    static ApplyResult string(std::string value);
    static ApplyResult integer_value(std::int64_t value);
    static ApplyResult error(ResultCode code, std::string message);

    bool operator==(const ApplyResult& other) const;
};

std::vector<std::uint8_t> encode_command_bytes(const Command& command);
Command decode_command_bytes(const void* data, std::size_t size,
                             std::size_t max_field_size);

nuraft::ptr<nuraft::buffer> encode_command(const Command& command);
Command decode_command(const nuraft::buffer& buffer,
                       std::size_t max_field_size);

std::vector<std::uint8_t> encode_result_bytes(const ApplyResult& result);
ApplyResult decode_result_bytes(const void* data, std::size_t size,
                                std::size_t max_field_size);
nuraft::ptr<nuraft::buffer> encode_result(const ApplyResult& result);
ApplyResult decode_result(const nuraft::buffer& buffer,
                          std::size_t max_field_size);

const char* result_code_name(ResultCode code) noexcept;

}  // namespace strongkv
