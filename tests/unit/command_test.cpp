#include "test.h"

#include "common/binary_codec.h"
#include "state_machine/command.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace strongkv;

SKV_TEST(command_round_trip_preserves_all_fields) {
    Command source;
    source.type = CommandType::kSet;
    source.key = std::string("a\0b", 3);
    source.value = std::string("x\0y", 3);
    source.client_id = 123456789;
    source.request_id = 987654321;

    const auto bytes = encode_command_bytes(source);
    const auto decoded =
        decode_command_bytes(bytes.data(), bytes.size(), 1024);

    SKV_EXPECT_EQ(decoded.version, source.version);
    SKV_EXPECT_EQ(decoded.type, source.type);
    SKV_EXPECT_EQ(decoded.key, source.key);
    SKV_EXPECT_EQ(decoded.value, source.value);
    SKV_EXPECT_EQ(decoded.client_id, source.client_id);
    SKV_EXPECT_EQ(decoded.request_id, source.request_id);
}

SKV_TEST(command_decode_rejects_truncation_and_trailing_bytes) {
    Command source;
    source.type = CommandType::kDel;
    source.key = "key";
    auto bytes = encode_command_bytes(source);

    SKV_EXPECT_THROW(
        decode_command_bytes(bytes.data(), bytes.size() - 1, 1024));
    bytes.push_back(0);
    SKV_EXPECT_THROW(
        decode_command_bytes(bytes.data(), bytes.size(), 1024));
}

SKV_TEST(command_decode_enforces_field_limit) {
    Command source;
    source.type = CommandType::kSet;
    source.key = std::string(32, 'k');
    const auto bytes = encode_command_bytes(source);
    SKV_EXPECT_THROW(
        decode_command_bytes(bytes.data(), bytes.size(), 16));
}

SKV_TEST(result_round_trip_supports_integer_and_error) {
    const auto integer = ApplyResult::integer_value(
        std::numeric_limits<std::int64_t>::min());
    const auto integer_bytes = encode_result_bytes(integer);
    SKV_EXPECT_EQ(
        decode_result_bytes(integer_bytes.data(), integer_bytes.size(), 1024),
        integer);

    const auto error =
        ApplyResult::error(ResultCode::kNotInteger, "not an integer");
    const auto error_bytes = encode_result_bytes(error);
    SKV_EXPECT_EQ(
        decode_result_bytes(error_bytes.data(), error_bytes.size(), 1024),
        error);
}

SKV_TEST(binary_codec_is_big_endian_and_detects_truncation) {
    BinaryWriter writer;
    writer.put_u16(0x1234);
    writer.put_u32(0x01020304);
    writer.put_u64(0x0102030405060708ULL);
    const auto bytes = writer.take();

    SKV_EXPECT_EQ(bytes[0], static_cast<std::uint8_t>(0x12));
    SKV_EXPECT_EQ(bytes[1], static_cast<std::uint8_t>(0x34));

    BinaryReader reader(bytes);
    SKV_EXPECT_EQ(reader.get_u16(), static_cast<std::uint16_t>(0x1234));
    SKV_EXPECT_EQ(reader.get_u32(), static_cast<std::uint32_t>(0x01020304));
    SKV_EXPECT_EQ(reader.get_u64(), 0x0102030405060708ULL);
    reader.require_end();
    SKV_EXPECT_THROW(reader.get_u8());
}
