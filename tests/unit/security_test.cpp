#include "security/password.h"
#include "test.h"

SKV_TEST(password_comparison) {
    SKV_EXPECT_TRUE(strongkv::constant_time_password_equal(
        "unit-test-password", "unit-test-password"));
    SKV_EXPECT_TRUE(!strongkv::constant_time_password_equal(
        "wrong", "unit-test-password"));
    SKV_EXPECT_TRUE(!strongkv::constant_time_password_equal(
        "other-test-password", "unit-test-password"));
    SKV_EXPECT_TRUE(!strongkv::constant_time_password_equal(
        "unit-test-password-extra", "unit-test-password"));
}
