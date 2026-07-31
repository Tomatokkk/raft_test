#include "security/password.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace strongkv {

bool constant_time_password_equal(
        std::string_view supplied,
        std::string_view configured) noexcept {
    const std::size_t maximum =
        std::max(supplied.size(), configured.size());
    std::uint8_t difference =
        static_cast<std::uint8_t>(supplied.size() != configured.size());
    for (std::size_t i = 0; i < maximum; ++i) {
        const auto left = i < supplied.size()
            ? static_cast<std::uint8_t>(supplied[i])
            : std::uint8_t{0};
        const auto right = i < configured.size()
            ? static_cast<std::uint8_t>(configured[i])
            : std::uint8_t{0};
        difference = static_cast<std::uint8_t>(
            difference | static_cast<std::uint8_t>(left ^ right));
    }
    return difference == 0;
}

}  // namespace strongkv
