#include "storage/file_log_store.h"

#include "common/binary_codec.h"
#include "storage/file_util.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace strongkv {
namespace {

constexpr FileMagic kLegacyLogMagic{{'S', 'K', 'L', 'G'}};
constexpr std::uint8_t kWalMagic[4]{'S', 'K', 'W', '2'};
constexpr std::uint16_t kWalVersion = 2;
constexpr std::uint16_t kEntryVersion = 1;
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kMaximumLogFile = 4ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t kMaximumEntryCount = 100000000;
constexpr std::uint32_t kMaximumEntrySize = 64U * 1024 * 1024;

std::runtime_error io_error(
        const char* operation, const std::filesystem::path& path) {
    return std::runtime_error(
        std::string(operation) + " failed for " + path.string() +
        ": " + std::system_error(errno, std::generic_category()).what());
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

void sync_data(int fd, const std::filesystem::path& path) {
#ifdef _WIN32
    if (_commit(fd) != 0) {
        throw io_error("flush", path);
    }
#else
    if (::fdatasync(fd) != 0) {
        throw io_error("fdatasync", path);
    }
#endif
}

nuraft::ptr<nuraft::buffer> make_buffer(
        const std::vector<std::uint8_t>& bytes) {
    auto output = nuraft::buffer::alloc(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(output->data_begin(), bytes.data(), bytes.size());
    }
    output->pos(0);
    return output;
}

bool starts_with(
        const std::vector<std::uint8_t>& bytes,
        const std::uint8_t* prefix, std::size_t size) {
    return bytes.size() >= size &&
           std::memcmp(bytes.data(), prefix, size) == 0;
}

}  // namespace

FileLogStore::FileLogStore(std::filesystem::path file)
    : file_(std::move(file)) {
    load();
}

FileLogStore::~FileLogStore() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        sync_pending_locked();
    } catch (...) {
        // Destructors cannot report storage failure. NuRaft calls flush()
        // during orderly shutdown, where the error remains observable.
    }
    close_locked();
}

nuraft::ptr<nuraft::log_entry> FileLogStore::clone_entry(
        const nuraft::ptr<nuraft::log_entry>& entry) {
    return nuraft::cs_new<nuraft::log_entry>(
        entry->get_term(), nuraft::buffer::clone(entry->get_buf()),
        entry->get_val_type(), entry->get_timestamp(),
        entry->has_crc32(), entry->get_crc32(), false);
}

nuraft::ptr<nuraft::log_entry> FileLogStore::dummy_entry() {
    auto buffer = nuraft::buffer::alloc(sizeof(nuraft::ulong));
    return nuraft::cs_new<nuraft::log_entry>(0, buffer);
}

nuraft::ulong FileLogStore::next_slot_locked() const {
    if (entries_.empty()) {
        return start_index_;
    }
    return entries_.rbegin()->first + 1;
}

nuraft::ulong FileLogStore::next_slot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_slot_locked();
}

nuraft::ulong FileLogStore::start_index() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return start_index_;
}

nuraft::ptr<nuraft::log_entry> FileLogStore::last_entry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty() ? dummy_entry()
                            : clone_entry(entries_.rbegin()->second);
}

nuraft::ulong FileLogStore::append(
        nuraft::ptr<nuraft::log_entry>& entry) {
    if (!entry) {
        throw std::invalid_argument("cannot append a null Raft entry");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const nuraft::ulong index = next_slot_locked();
    entries_[index] = clone_entry(entry);
    queue_entry_locked(index, entries_[index]);
    return index;
}

void FileLogStore::write_at(
        nuraft::ulong index, nuraft::ptr<nuraft::log_entry>& entry) {
    if (!entry || index < start_index_) {
        throw std::invalid_argument("invalid Raft log overwrite index");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (index > next_slot_locked()) {
        throw std::invalid_argument("Raft log overwrite creates a gap");
    }

    // NuRaft finishes each append batch before beginning another. Flush any
    // unexpected pending batch first so suffix truncation never discards it.
    sync_pending_locked();
    const auto offset = offsets_.find(index);
    const std::uint64_t truncate_at =
        offset == offsets_.end() ? file_size_ : offset->second;
    truncate_locked(truncate_at);
    entries_.erase(entries_.lower_bound(index), entries_.end());
    offsets_.erase(offsets_.lower_bound(index), offsets_.end());
    durable_index_ = index > 0 ? index - 1 : 0;
    entries_[index] = clone_entry(entry);
    queue_entry_locked(index, entries_[index]);
}

void FileLogStore::end_of_append_batch(
        nuraft::ulong start, nuraft::ulong count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count == 0 || pending_count_ == 0) {
        return;
    }
    const nuraft::ulong requested_end = start + count;
    const nuraft::ulong pending_end =
        pending_start_index_ + pending_count_;
    // Followers may receive a retry whose prefix already exists. NuRaft
    // reports the complete network batch here, while log_store::append was
    // invoked only for the missing suffix.
    if (pending_start_index_ < start || pending_end > requested_end) {
        throw std::runtime_error(
            "queued Raft WAL entries fall outside append batch");
    }
    sync_pending_locked();
}

nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>>
FileLogStore::log_entries(nuraft::ulong start, nuraft::ulong end) {
    return log_entries_ext(start, end, 0);
}

nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>>
FileLogStore::log_entries_ext(nuraft::ulong start, nuraft::ulong end,
                              nuraft::int64 batch_size_hint_in_bytes) {
    auto output = nuraft::cs_new<
        std::vector<nuraft::ptr<nuraft::log_entry>>>();
    if (batch_size_hint_in_bytes < 0) {
        return output;
    }
    if (end < start) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t accumulated = 0;
    for (nuraft::ulong index = start; index < end; ++index) {
        const auto found = entries_.find(index);
        if (found == entries_.end()) {
            return nullptr;
        }
        output->push_back(clone_entry(found->second));
        accumulated += found->second->get_buf().size();
        if (batch_size_hint_in_bytes > 0 &&
            accumulated >=
                static_cast<std::size_t>(batch_size_hint_in_bytes)) {
            break;
        }
    }
    return output;
}

nuraft::ptr<nuraft::log_entry> FileLogStore::entry_at(
        nuraft::ulong index) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(index);
    return found == entries_.end() ? dummy_entry()
                                   : clone_entry(found->second);
}

nuraft::ulong FileLogStore::term_at(nuraft::ulong index) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(index);
    return found == entries_.end() ? 0 : found->second->get_term();
}

nuraft::ptr<nuraft::buffer> FileLogStore::pack(
        nuraft::ulong index, nuraft::int32 count) {
    if (count < 0) {
        throw std::invalid_argument("negative Raft pack count");
    }
    std::vector<nuraft::ptr<nuraft::buffer>> serialized;
    std::size_t total = sizeof(nuraft::int32);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (nuraft::int32 offset = 0; offset < count; ++offset) {
            const auto found =
                entries_.find(index + static_cast<nuraft::ulong>(offset));
            if (found == entries_.end()) {
                throw std::runtime_error("Raft pack requested missing entry");
            }
            auto encoded = found->second->serialize();
            total += sizeof(nuraft::int32) + encoded->size();
            serialized.push_back(std::move(encoded));
        }
    }
    auto output = nuraft::buffer::alloc(total);
    output->put(count);
    for (auto& encoded : serialized) {
        if (encoded->size() >
            static_cast<std::size_t>(
                std::numeric_limits<nuraft::int32>::max())) {
            throw std::length_error("Raft entry is too large to pack");
        }
        output->put(static_cast<nuraft::int32>(encoded->size()));
        output->put(*encoded);
    }
    output->pos(0);
    return output;
}

void FileLogStore::apply_pack(nuraft::ulong index, nuraft::buffer& pack) {
    pack.pos(0);
    const nuraft::int32 count = pack.get_int();
    if (count < 0) {
        throw std::runtime_error("negative Raft pack count");
    }
    std::vector<nuraft::ptr<nuraft::log_entry>> decoded;
    decoded.reserve(static_cast<std::size_t>(count));
    for (nuraft::int32 offset = 0; offset < count; ++offset) {
        const nuraft::int32 raw_size = pack.get_int();
        if (raw_size <= 0 ||
            pack.pos() > pack.size() ||
            static_cast<std::size_t>(raw_size) >
                pack.size() - pack.pos()) {
            throw std::runtime_error("invalid packed Raft log length");
        }
        auto raw = nuraft::buffer::alloc(
            static_cast<std::size_t>(raw_size));
        pack.get(raw);
        raw->pos(0);
        decoded.push_back(nuraft::log_entry::deserialize(*raw));
    }
    if (pack.pos() != pack.size()) {
        throw std::runtime_error("trailing bytes in Raft log pack");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    sync_pending_locked();
    entries_.erase(entries_.lower_bound(index), entries_.end());
    for (std::size_t offset = 0; offset < decoded.size(); ++offset) {
        entries_[index + offset] = std::move(decoded[offset]);
    }
    start_index_ = entries_.empty() ? index : entries_.begin()->first;
    rewrite_locked();
}

bool FileLogStore::compact(nuraft::ulong last_log_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    sync_pending_locked();
    entries_.erase(entries_.begin(),
                   entries_.upper_bound(last_log_index));
    if (start_index_ <= last_log_index) {
        start_index_ = last_log_index + 1;
    }
    rewrite_locked();
    return true;
}

bool FileLogStore::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    sync_pending_locked();
    if (fd_ >= 0) {
        sync_data(fd_, file_);
    }
    return true;
}

nuraft::ulong FileLogStore::last_durable_index() {
    std::lock_guard<std::mutex> lock(mutex_);
    return durable_index_;
}

void FileLogStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::filesystem::create_directories(
        file_.has_parent_path() ? file_.parent_path()
                                : std::filesystem::path("."));
    if (!std::filesystem::exists(file_)) {
        atomic_write_file(file_, encode_header_locked());
        file_size_ = kHeaderSize;
        durable_index_ = start_index_ - 1;
        open_locked();
        return;
    }

    const auto bytes = read_file(file_, kMaximumLogFile);
    if (starts_with(bytes, kWalMagic, sizeof(kWalMagic))) {
        load_wal_locked(bytes);
    } else {
        load_legacy_locked();
        rewrite_locked();
        return;
    }
    open_locked();
}

void FileLogStore::load_legacy_locked() {
    const auto payload =
        read_checked_record(file_, kLegacyLogMagic, kMaximumLogFile);
    BinaryReader reader(payload);
    start_index_ = reader.get_u64();
    if (start_index_ == 0) {
        throw DecodeError("Raft log start index cannot be zero");
    }
    const std::uint64_t count = reader.get_u64();
    if (count > kMaximumEntryCount) {
        throw DecodeError("Raft log entry count is unreasonable");
    }
    nuraft::ulong expected = start_index_;
    for (std::uint64_t i = 0; i < count; ++i) {
        const nuraft::ulong index = reader.get_u64();
        const std::uint32_t size = reader.get_u32();
        if (index != expected++ || size == 0) {
            throw DecodeError("legacy Raft log is not contiguous");
        }
        auto bytes = reader.get_bytes(size);
        auto buffer = make_buffer(bytes);
        entries_[index] = nuraft::log_entry::deserialize(*buffer);
    }
    reader.require_end();
    durable_index_ = next_slot_locked() - 1;
}

void FileLogStore::load_wal_locked(
        const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kHeaderSize) {
        throw DecodeError("truncated Raft WAL header");
    }
    BinaryReader header(bytes.data() + 4, kHeaderSize - 4);
    if (header.get_u16() != kWalVersion || header.get_u16() != 0) {
        throw DecodeError("unsupported Raft WAL version");
    }
    start_index_ = header.get_u64();
    const std::uint64_t stored_checksum = header.get_u64();
    if (start_index_ == 0 ||
        stored_checksum != fnv1a64(bytes.data(), kHeaderSize - 8)) {
        throw DecodeError("invalid Raft WAL header");
    }

    std::size_t position = kHeaderSize;
    nuraft::ulong expected = start_index_;
    std::size_t valid_size = kHeaderSize;
    while (position < bytes.size()) {
        const std::size_t frame_offset = position;
        if (bytes.size() - position < sizeof(std::uint32_t)) {
            break;
        }
        BinaryReader frame_prefix(bytes.data() + position,
                                  bytes.size() - position);
        const std::uint32_t body_size = frame_prefix.get_u32();
        position += sizeof(std::uint32_t);
        if (body_size < sizeof(std::uint16_t) * 2 +
                            sizeof(std::uint64_t) +
                            sizeof(std::uint32_t) ||
            body_size > kMaximumEntrySize + 32U ||
            bytes.size() - position <
                static_cast<std::size_t>(body_size) +
                    sizeof(std::uint64_t)) {
            break;
        }
        const auto* body = bytes.data() + position;
        position += body_size;
        BinaryReader checksum_reader(
            bytes.data() + position, sizeof(std::uint64_t));
        const std::uint64_t stored = checksum_reader.get_u64();
        position += sizeof(std::uint64_t);
        if (stored != fnv1a64(body, body_size)) {
            if (position != bytes.size()) {
                throw DecodeError("Raft WAL checksum mismatch before tail");
            }
            break;
        }

        BinaryReader record(body, body_size);
        if (record.get_u16() != kEntryVersion ||
            record.get_u16() != 0) {
            throw DecodeError("unsupported Raft WAL entry version");
        }
        const nuraft::ulong index = record.get_u64();
        const std::uint32_t entry_size = record.get_u32();
        if (index != expected++ || entry_size == 0 ||
            entry_size > kMaximumEntrySize) {
            throw DecodeError("invalid Raft WAL entry sequence");
        }
        auto serialized = record.get_bytes(entry_size);
        record.require_end();
        auto buffer = make_buffer(serialized);
        entries_[index] = nuraft::log_entry::deserialize(*buffer);
        offsets_[index] = frame_offset;
        valid_size = position;
    }

    file_size_ = valid_size;
    durable_index_ = next_slot_locked() - 1;
    if (valid_size != bytes.size()) {
        open_locked();
        truncate_locked(valid_size);
        sync_data(fd_, file_);
        close_locked();
    }
}

std::vector<std::uint8_t> FileLogStore::encode_header_locked() const {
    BinaryWriter writer;
    writer.put_raw(kWalMagic, sizeof(kWalMagic));
    writer.put_u16(kWalVersion);
    writer.put_u16(0);
    writer.put_u64(start_index_);
    const auto& prefix = writer.data();
    writer.put_u64(fnv1a64(prefix.data(), prefix.size()));
    return writer.take();
}

std::vector<std::uint8_t> FileLogStore::encode_entry_locked(
        nuraft::ulong index,
        const nuraft::ptr<nuraft::log_entry>& entry) const {
    auto serialized = entry->serialize();
    if (serialized->size() == 0 ||
        serialized->size() > kMaximumEntrySize) {
        throw std::length_error("Raft WAL entry exceeds size limit");
    }
    BinaryWriter body;
    body.put_u16(kEntryVersion);
    body.put_u16(0);
    body.put_u64(index);
    body.put_u32(static_cast<std::uint32_t>(serialized->size()));
    body.put_raw(serialized->data_begin(), serialized->size());

    BinaryWriter frame;
    frame.put_u32(static_cast<std::uint32_t>(body.data().size()));
    frame.put_raw(body.data().data(), body.data().size());
    frame.put_u64(fnv1a64(body.data().data(), body.data().size()));
    return frame.take();
}

std::vector<std::uint8_t> FileLogStore::encode_all_locked() const {
    BinaryWriter output;
    const auto header = encode_header_locked();
    output.put_raw(header.data(), header.size());
    for (const auto& [index, entry] : entries_) {
        const auto frame = encode_entry_locked(index, entry);
        output.put_raw(frame.data(), frame.size());
    }
    return output.take();
}

void FileLogStore::queue_entry_locked(
        nuraft::ulong index,
        const nuraft::ptr<nuraft::log_entry>& entry) {
    if (pending_count_ == 0) {
        pending_start_index_ = index;
    } else if (index != pending_start_index_ + pending_count_) {
        throw std::runtime_error("non-contiguous pending Raft WAL batch");
    }
    const auto frame = encode_entry_locked(index, entry);
    offsets_[index] = file_size_ + pending_.size();
    pending_.insert(pending_.end(), frame.begin(), frame.end());
    ++pending_count_;
}

void FileLogStore::sync_pending_locked() {
    if (pending_.empty()) {
        return;
    }
    if (fd_ < 0) {
        open_locked();
    }
    write_all(fd_, pending_.data(), pending_.size(), file_);
    sync_data(fd_, file_);
    file_size_ += pending_.size();
    durable_index_ =
        pending_start_index_ + pending_count_ - 1;
    pending_.clear();
    pending_start_index_ = 0;
    pending_count_ = 0;
}

void FileLogStore::rewrite_locked() {
    pending_.clear();
    pending_start_index_ = 0;
    pending_count_ = 0;
    close_locked();
    const auto bytes = encode_all_locked();
    atomic_write_file(file_, bytes);

    offsets_.clear();
    std::uint64_t offset = kHeaderSize;
    for (const auto& [index, entry] : entries_) {
        offsets_[index] = offset;
        offset += encode_entry_locked(index, entry).size();
    }
    file_size_ = bytes.size();
    durable_index_ = next_slot_locked() - 1;
    open_locked();
}

void FileLogStore::open_locked() {
    if (fd_ >= 0) {
        return;
    }
#ifdef _WIN32
    fd_ = _open(file_.string().c_str(),
                _O_RDWR | _O_APPEND | _O_BINARY,
                _S_IREAD | _S_IWRITE);
#else
    fd_ = ::open(file_.c_str(), O_RDWR | O_APPEND | O_CLOEXEC);
#endif
    if (fd_ < 0) {
        throw io_error("open", file_);
    }
}

void FileLogStore::close_locked() noexcept {
    if (fd_ < 0) {
        return;
    }
#ifdef _WIN32
    _close(fd_);
#else
    ::close(fd_);
#endif
    fd_ = -1;
}

void FileLogStore::truncate_locked(std::uint64_t size) {
    if (fd_ < 0) {
        open_locked();
    }
#ifdef _WIN32
    if (_chsize_s(fd_, size) != 0) {
        throw io_error("truncate", file_);
    }
#else
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        throw io_error("truncate", file_);
    }
#endif
    file_size_ = size;
}

}  // namespace strongkv
