#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace strongkv {

class DecodeError : public std::runtime_error {
public:
    explicit DecodeError(const std::string& message)
        : std::runtime_error(message) {}
};

class BinaryWriter {
public:
    void put_u8(std::uint8_t value);
    void put_u16(std::uint16_t value);
    void put_u32(std::uint32_t value);
    void put_u64(std::uint64_t value);
    void put_i64(std::int64_t value);
    void put_raw(const void* data, std::size_t size);
    void put_string(const std::string& value);

    const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    std::vector<std::uint8_t> take() noexcept { return std::move(data_); }

private:
    std::vector<std::uint8_t> data_;
};

class BinaryReader {
public:
    BinaryReader(const void* data, std::size_t size);
    explicit BinaryReader(const std::vector<std::uint8_t>& data);

    std::uint8_t get_u8();
    std::uint16_t get_u16();
    std::uint32_t get_u32();
    std::uint64_t get_u64();
    std::int64_t get_i64();
    std::string get_string(std::size_t max_size);
    std::vector<std::uint8_t> get_bytes(std::size_t size);
    void require_end() const;

    std::size_t remaining() const noexcept { return size_ - pos_; }

private:
    void require(std::size_t count) const;

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_{0};
};

std::uint64_t fnv1a64(const void* data, std::size_t size) noexcept;

}  // namespace strongkv
