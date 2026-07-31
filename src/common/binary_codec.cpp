#include "common/binary_codec.h"

#include <cstring>
#include <limits>

namespace strongkv {

void BinaryWriter::put_u8(std::uint8_t value) {
    data_.push_back(value);
}

void BinaryWriter::put_u16(std::uint16_t value) {
    data_.push_back(static_cast<std::uint8_t>(value >> 8U));
    data_.push_back(static_cast<std::uint8_t>(value));
}

void BinaryWriter::put_u32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        data_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void BinaryWriter::put_u64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        data_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void BinaryWriter::put_i64(std::int64_t value) {
    put_u64(static_cast<std::uint64_t>(value));
}

void BinaryWriter::put_raw(const void* data, std::size_t size) {
    if (size == 0) {
        return;
    }
    if (data == nullptr) {
        throw std::invalid_argument("BinaryWriter::put_raw received null data");
    }
    const auto* begin = static_cast<const std::uint8_t*>(data);
    data_.insert(data_.end(), begin, begin + size);
}

void BinaryWriter::put_string(const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("string is too large for binary encoding");
    }
    put_u32(static_cast<std::uint32_t>(value.size()));
    put_raw(value.data(), value.size());
}

BinaryReader::BinaryReader(const void* data, std::size_t size)
    : data_(static_cast<const std::uint8_t*>(data)), size_(size) {
    if (data_ == nullptr && size_ != 0) {
        throw std::invalid_argument("BinaryReader received null data");
    }
}

BinaryReader::BinaryReader(const std::vector<std::uint8_t>& data)
    : BinaryReader(data.data(), data.size()) {}

void BinaryReader::require(std::size_t count) const {
    if (count > remaining()) {
        throw DecodeError("truncated binary payload");
    }
}

std::uint8_t BinaryReader::get_u8() {
    require(1);
    return data_[pos_++];
}

std::uint16_t BinaryReader::get_u16() {
    require(2);
    const auto value = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(data_[pos_]) << 8U) |
        static_cast<std::uint32_t>(data_[pos_ + 1]));
    pos_ += 2;
    return value;
}

std::uint32_t BinaryReader::get_u32() {
    require(4);
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8U) | data_[pos_++];
    }
    return value;
}

std::uint64_t BinaryReader::get_u64() {
    require(8);
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | data_[pos_++];
    }
    return value;
}

std::int64_t BinaryReader::get_i64() {
    return static_cast<std::int64_t>(get_u64());
}

std::string BinaryReader::get_string(std::size_t max_size) {
    const std::uint32_t length = get_u32();
    if (length > max_size) {
        throw DecodeError("binary string exceeds configured limit");
    }
    require(length);
    const auto* begin = reinterpret_cast<const char*>(data_ + pos_);
    std::string value(begin, begin + length);
    pos_ += length;
    return value;
}

std::vector<std::uint8_t> BinaryReader::get_bytes(std::size_t size) {
    require(size);
    std::vector<std::uint8_t> value(data_ + pos_, data_ + pos_ + size);
    pos_ += size;
    return value;
}

void BinaryReader::require_end() const {
    if (remaining() != 0) {
        throw DecodeError("trailing bytes in binary payload");
    }
}

std::uint64_t fnv1a64(const void* data, std::size_t size) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t value = offset;
    for (std::size_t i = 0; i < size; ++i) {
        value ^= bytes[i];
        value *= prime;
    }
    return value;
}

}  // namespace strongkv
