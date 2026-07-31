#include "storage/file_util.h"

#include "common/binary_codec.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace strongkv {
namespace {

std::runtime_error io_error(const std::string& operation,
                            const std::filesystem::path& path) {
    return std::runtime_error(operation + " " + path.string() + ": " +
                              std::strerror(errno));
}

void write_all(int fd, const std::uint8_t* data, std::size_t size,
               const std::filesystem::path& path) {
    std::size_t written = 0;
    while (written < size) {
#ifdef _WIN32
        const auto chunk = static_cast<unsigned int>(
            std::min<std::size_t>(
                size - written,
                static_cast<std::size_t>(
                    std::numeric_limits<unsigned int>::max())));
        const int result = _write(fd, data + written, chunk);
#else
        const ssize_t result = ::write(fd, data + written, size - written);
#endif
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw io_error("write", path);
        }
        if (result == 0) {
            throw std::runtime_error("short write to " + path.string());
        }
        written += static_cast<std::size_t>(result);
    }
}

void sync_fd(int fd, const std::filesystem::path& path) {
#ifdef _WIN32
    if (_commit(fd) != 0) {
        throw io_error("flush", path);
    }
#else
    if (::fsync(fd) != 0) {
        throw io_error("fsync", path);
    }
#endif
}

void close_fd(int fd) noexcept {
#ifdef _WIN32
    _close(fd);
#else
    ::close(fd);
#endif
}

#ifndef _WIN32
void sync_directory(const std::filesystem::path& directory) {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        throw io_error("open directory", directory);
    }
    const int result = ::fsync(fd);
    const int saved_errno = errno;
    close_fd(fd);
    if (result != 0) {
        errno = saved_errno;
        throw io_error("fsync directory", directory);
    }
}
#endif

}  // namespace

void atomic_write_file(const std::filesystem::path& path,
                       const std::vector<std::uint8_t>& data) {
    const auto directory =
        path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    std::filesystem::create_directories(directory);
    const auto temporary = path.string() + ".tmp";

#ifdef _WIN32
    const int fd = _open(temporary.c_str(),
                         _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                         _S_IREAD | _S_IWRITE);
#else
    const int fd = ::open(temporary.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
#endif
    if (fd < 0) {
        throw io_error("open", temporary);
    }

    try {
        write_all(fd, data.data(), data.size(), temporary);
        sync_fd(fd, temporary);
        close_fd(fd);
    } catch (...) {
        close_fd(fd);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }

#ifdef _WIN32
    if (!MoveFileExA(temporary.c_str(), path.string().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error(
            "atomic replace failed for " + path.string() +
            ", Windows error " + std::to_string(GetLastError()));
    }
#else
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const auto error = io_error("rename", path);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw error;
    }
    sync_directory(directory);
#endif
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path,
                                    std::size_t max_size) {
    const auto raw_size = std::filesystem::file_size(path);
    if (raw_size > max_size) {
        throw std::runtime_error("file exceeds configured limit: " +
                                 path.string());
    }
    const auto size = static_cast<std::size_t>(raw_size);
    std::vector<std::uint8_t> data(size);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    if (size != 0) {
        input.read(reinterpret_cast<char*>(data.data()),
                   static_cast<std::streamsize>(size));
        if (!input || input.gcount() != static_cast<std::streamsize>(size)) {
            throw std::runtime_error("short read from file: " + path.string());
        }
    }
    return data;
}

void write_checked_record(const std::filesystem::path& path,
                          const FileMagic& magic,
                          const std::vector<std::uint8_t>& payload) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("record payload is too large");
    }
    BinaryWriter writer;
    writer.put_raw(magic.data(), magic.size());
    writer.put_u16(1);
    writer.put_u16(0);
    writer.put_u32(static_cast<std::uint32_t>(payload.size()));
    writer.put_raw(payload.data(), payload.size());
    const auto& without_checksum = writer.data();
    writer.put_u64(fnv1a64(without_checksum.data(), without_checksum.size()));
    atomic_write_file(path, writer.data());
}

std::vector<std::uint8_t> read_checked_record(
        const std::filesystem::path& path,
        const FileMagic& expected_magic,
        std::size_t max_payload_size) {
    constexpr std::size_t overhead = 4 + 2 + 2 + 4 + 8;
    const auto data = read_file(path, max_payload_size + overhead);
    if (data.size() < overhead) {
        throw DecodeError("checked record is truncated: " + path.string());
    }

    BinaryReader reader(data);
    const auto magic = reader.get_bytes(expected_magic.size());
    if (!std::equal(magic.begin(), magic.end(), expected_magic.begin())) {
        throw DecodeError("checked record magic mismatch: " + path.string());
    }
    if (reader.get_u16() != 1 || reader.get_u16() != 0) {
        throw DecodeError("unsupported checked record version: " +
                          path.string());
    }
    const std::uint32_t payload_size = reader.get_u32();
    if (payload_size > max_payload_size ||
        reader.remaining() != static_cast<std::size_t>(payload_size) + 8) {
        throw DecodeError("invalid checked record length: " + path.string());
    }
    const auto payload = reader.get_bytes(payload_size);
    const std::uint64_t stored_checksum = reader.get_u64();
    reader.require_end();
    const std::size_t checked_size = data.size() - sizeof(std::uint64_t);
    if (fnv1a64(data.data(), checked_size) != stored_checksum) {
        throw DecodeError("checked record checksum mismatch: " +
                          path.string());
    }
    return payload;
}

}  // namespace strongkv
