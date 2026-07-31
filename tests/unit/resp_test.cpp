#include "test.h"

#include "protocol/resp.h"
#include "security/password.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace strongkv;

SKV_TEST(resp_parser_handles_partial_and_coalesced_requests) {
    RespParser parser(1024);
    const std::string first = encode_array({"SET", "foo", "bar"});
    const std::string second = encode_array({"GET", "foo"});

    parser.feed(first.substr(0, 7));
    SKV_EXPECT_TRUE(!parser.next().has_value());
    parser.feed(first.substr(7) + second);

    const auto set = command_arguments(parser.next().value());
    SKV_EXPECT_EQ(set.size(), std::size_t{3});
    SKV_EXPECT_EQ(set[0], std::string("SET"));
    SKV_EXPECT_EQ(set[2], std::string("bar"));

    const auto get = command_arguments(parser.next().value());
    SKV_EXPECT_EQ(get.size(), std::size_t{2});
    SKV_EXPECT_EQ(get[0], std::string("GET"));
    SKV_EXPECT_TRUE(!parser.next().has_value());
}

SKV_TEST(resp_parser_supports_all_required_response_types) {
    RespValue root = RespValue::array_value({
        RespValue::simple("OK"),
        RespValue::error("ERR bad"),
        RespValue::integer_value(-42),
        RespValue::bulk(std::string("a\0b", 3)),
        RespValue::null_bulk(),
    });
    const std::string encoded = encode_resp(root);
    RespParser parser(1024);
    parser.feed(encoded);
    const auto decoded = parser.next().value();
    SKV_EXPECT_EQ(decoded.type, RespType::kArray);
    SKV_EXPECT_EQ(decoded.array.size(), std::size_t{5});
    SKV_EXPECT_EQ(decoded.array[2].integer, std::int64_t{-42});
    SKV_EXPECT_EQ(decoded.array[3].text, std::string("a\0b", 3));
    SKV_EXPECT_EQ(decoded.array[4].type, RespType::kNullBulkString);
}

SKV_TEST(resp_parser_rejects_invalid_and_oversized_frames) {
    RespParser parser(64);
    SKV_EXPECT_THROW(parser.feed(std::string(65, 'x')));

    RespParser invalid_length(64);
    invalid_length.feed("$-2\r\n");
    SKV_EXPECT_THROW(invalid_length.next());

    RespParser missing_cr(64);
    missing_cr.feed("+OK\n");
    SKV_EXPECT_THROW(missing_cr.next());

    RespParser huge_declaration(64);
    huge_declaration.feed("$1000\r\n");
    SKV_EXPECT_THROW(huge_declaration.next());
}

SKV_TEST(password_comparison_handles_equal_and_different_lengths) {
    SKV_EXPECT_TRUE(constant_time_password_equal(
        "unit-test-password", "unit-test-password"));
    SKV_EXPECT_TRUE(!constant_time_password_equal(
        "short-test-password", "unit-test-password"));
    SKV_EXPECT_TRUE(!constant_time_password_equal(
        "other-test-password", "unit-test-password"));
}
