#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace strongkv {

using FileMagic = std::array<std::uint8_t, 4>;

void atomic_write_file(const std::filesystem::path& path,
                       const std::vector<std::uint8_t>& data);

std::vector<std::uint8_t> read_file(
    const std::filesystem::path& path,
    std::size_t max_size = 1024ULL * 1024 * 1024);

void write_checked_record(const std::filesystem::path& path,
                          const FileMagic& magic,
                          const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> read_checked_record(
    const std::filesystem::path& path,
    const FileMagic& expected_magic,
    std::size_t max_payload_size = 1024ULL * 1024 * 1024);

}  // namespace strongkv
