#include "storage/file_log_store.h"

#include "common/binary_codec.h"
#include "storage/file_util.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace strongkv {
namespace {

constexpr FileMagic kLogMagic{{'S', 'K', 'L', 'G'}};
constexpr std::size_t kMaximumLogFile = 1024ULL * 1024 * 1024;
constexpr std::uint64_t kMaximumEntryCount = 100000000;

nuraft::ptr<nuraft::buffer> make_buffer(
        const std::vector<std::uint8_t>& bytes) {
    auto output = nuraft::buffer::alloc(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(output->data_begin(), bytes.data(), bytes.size());
    }
    output->pos(0);
    return output;
}

}  // namespace

FileLogStore::FileLogStore(std::filesystem::path file)
    : file_(std::move(file)) {
    load();
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
    persist_locked();
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
    entries_.erase(entries_.lower_bound(index), entries_.end());
    entries_[index] = clone_entry(entry);
    persist_locked();
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
            throw std::runtime_error("invalid packed Raft entry length");
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
    entries_.erase(entries_.lower_bound(index), entries_.end());
    for (std::size_t offset = 0; offset < decoded.size(); ++offset) {
        entries_[index + offset] = std::move(decoded[offset]);
    }
    if (!entries_.empty()) {
        start_index_ = entries_.begin()->first;
    } else {
        start_index_ = index;
    }
    persist_locked();
}

bool FileLogStore::compact(nuraft::ulong last_log_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(entries_.begin(),
                   entries_.upper_bound(last_log_index));
    if (start_index_ <= last_log_index) {
        start_index_ = last_log_index + 1;
    }
    persist_locked();
    return true;
}

bool FileLogStore::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    persist_locked();
    return true;
}

nuraft::ulong FileLogStore::last_durable_index() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_slot_locked() - 1;
}

void FileLogStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!std::filesystem::exists(file_)) {
        return;
    }
    const auto payload =
        read_checked_record(file_, kLogMagic, kMaximumLogFile);
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
            throw DecodeError("Raft log entries are not contiguous");
        }
        auto bytes = reader.get_bytes(size);
        auto buffer = make_buffer(bytes);
        entries_[index] = nuraft::log_entry::deserialize(*buffer);
    }
    reader.require_end();
}

void FileLogStore::persist_locked() const {
    BinaryWriter writer;
    writer.put_u64(start_index_);
    writer.put_u64(entries_.size());
    for (const auto& [index, entry] : entries_) {
        auto encoded = entry->serialize();
        if (encoded->size() >
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("Raft entry exceeds file format limit");
        }
        writer.put_u64(index);
        writer.put_u32(static_cast<std::uint32_t>(encoded->size()));
        writer.put_raw(encoded->data_begin(), encoded->size());
    }
    write_checked_record(file_, kLogMagic, writer.data());
}

}  // namespace strongkv
