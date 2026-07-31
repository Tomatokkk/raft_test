#pragma once

#include <string_view>

namespace strongkv {

bool constant_time_password_equal(
    std::string_view supplied, std::string_view configured) noexcept;

}  // namespace strongkv
