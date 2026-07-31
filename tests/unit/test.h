#pragma once

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace strongkv::test {

using TestFunction = std::function<void()>;

std::vector<std::pair<std::string, TestFunction>>& registry();

class Registrar {
public:
    Registrar(std::string name, TestFunction function);
};

template <typename Left, typename Right>
void expect_equal(const Left& left, const Right& right,
                  const char* left_text, const char* right_text,
                  const char* file, int line) {
    if (!(left == right)) {
        std::ostringstream message;
        message << file << ':' << line << ": expected " << left_text
                << " == " << right_text;
        throw std::runtime_error(message.str());
    }
}

inline void expect_true(bool value, const char* text,
                        const char* file, int line) {
    if (!value) {
        std::ostringstream message;
        message << file << ':' << line << ": expected true: " << text;
        throw std::runtime_error(message.str());
    }
}

template <typename Function>
void expect_throw(Function&& function, const char* text,
                  const char* file, int line) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    std::ostringstream message;
    message << file << ':' << line << ": expected exception: " << text;
    throw std::runtime_error(message.str());
}

}  // namespace strongkv::test

#define SKV_TEST(name)                                                     \
    static void name();                                                    \
    static ::strongkv::test::Registrar name##_registrar(#name, &name);     \
    static void name()

#define SKV_EXPECT_EQ(left, right)                                         \
    ::strongkv::test::expect_equal((left), (right), #left, #right,         \
                                   __FILE__, __LINE__)

#define SKV_EXPECT_TRUE(value)                                             \
    ::strongkv::test::expect_true(static_cast<bool>(value), #value,        \
                                  __FILE__, __LINE__)

#define SKV_EXPECT_THROW(expression)                                       \
    ::strongkv::test::expect_throw([&] { expression; }, #expression,       \
                                   __FILE__, __LINE__)
