#pragma once

#include <libnuraft/log_store.hxx>

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

namespace strongkv {

// Durable append-only NuRaft log store. Normal appends are encoded in memory
// and persisted once at end_of_append_batch(), so one Raft batch needs one
// fdatasync instead of rewriting and fsyncing the complete log per entry.
// Suffix overwrite and snapshot compaction remain cold-path atomic rewrites.
class FileLogStore final : public nuraft::log_store {
public:
    explicit FileLogStore(std::filesystem::path file);
    ~FileLogStore() override;

    nuraft::ulong next_slot() const override;
    nuraft::ulong start_index() const override;
    nuraft::ptr<nuraft::log_entry> last_entry() const override;
    nuraft::ulong append(nuraft::ptr<nuraft::log_entry>& entry) override;
    void write_at(nuraft::ulong index,
                  nuraft::ptr<nuraft::log_entry>& entry) override;
    void end_of_append_batch(nuraft::ulong start,
                             nuraft::ulong count) override;
    nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>> log_entries(
        nuraft::ulong start, nuraft::ulong end) override;
    nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>> log_entries_ext(
        nuraft::ulong start, nuraft::ulong end,
        nuraft::int64 batch_size_hint_in_bytes) override;
    nuraft::ptr<nuraft::log_entry> entry_at(
        nuraft::ulong index) override;
    nuraft::ulong term_at(nuraft::ulong index) override;
    nuraft::ptr<nuraft::buffer> pack(nuraft::ulong index,
                                      nuraft::int32 count) override;
    void apply_pack(nuraft::ulong index, nuraft::buffer& pack) override;
    bool compact(nuraft::ulong last_log_index) override;
    bool flush() override;
    nuraft::ulong last_durable_index() override;

private:
    static nuraft::ptr<nuraft::log_entry> clone_entry(
        const nuraft::ptr<nuraft::log_entry>& entry);
    static nuraft::ptr<nuraft::log_entry> dummy_entry();

    nuraft::ulong next_slot_locked() const;
    void load();
    void load_legacy_locked();
    void load_wal_locked(const std::vector<std::uint8_t>& bytes);
    std::vector<std::uint8_t> encode_header_locked() const;
    std::vector<std::uint8_t> encode_entry_locked(
        nuraft::ulong index,
        const nuraft::ptr<nuraft::log_entry>& entry) const;
    std::vector<std::uint8_t> encode_all_locked() const;
    void queue_entry_locked(
        nuraft::ulong index,
        const nuraft::ptr<nuraft::log_entry>& entry);
    void sync_pending_locked();
    void rewrite_locked();
    void open_locked();
    void close_locked() noexcept;
    void truncate_locked(std::uint64_t size);

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    nuraft::ulong start_index_{1};
    std::map<nuraft::ulong, nuraft::ptr<nuraft::log_entry>> entries_;
    std::map<nuraft::ulong, std::uint64_t> offsets_;
    std::vector<std::uint8_t> pending_;
    nuraft::ulong pending_start_index_{0};
    nuraft::ulong pending_count_{0};
    nuraft::ulong durable_index_{0};
    std::uint64_t file_size_{0};
    int fd_{-1};
};

}  // namespace strongkv
