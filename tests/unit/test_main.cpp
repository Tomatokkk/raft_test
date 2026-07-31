#include "test.h"

#include <exception>
#include <iostream>

namespace strongkv::test {

std::vector<std::pair<std::string, TestFunction>>& registry() {
    static std::vector<std::pair<std::string, TestFunction>> tests;
    return tests;
}

Registrar::Registrar(std::string name, TestFunction function) {
    registry().emplace_back(std::move(name), std::move(function));
}

}  // namespace strongkv::test

int main() {
    std::size_t passed = 0;
    for (const auto& [name, function] : strongkv::test::registry()) {
        try {
            function();
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        } catch (...) {
            std::cerr << "[FAIL] " << name
                      << ": non-standard exception\n";
        }
    }
    std::cout << passed << '/' << strongkv::test::registry().size()
              << " tests passed\n";
    return passed == strongkv::test::registry().size() ? 0 : 1;
}
