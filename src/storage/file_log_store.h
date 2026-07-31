#pragma once

#include <libnuraft/log_store.hxx>

#include <filesystem>
#include <map>
#include <mutex>

namespace strongkv {

// Durable NuRaft log_store for the prototype. Mutations are persisted by an
// atomic whole-file rewrite. This favors simple crash semantics over QPS.
class FileLogStore final : public nuraft::log_store {
public:
    explicit FileLogStore(std::filesystem::path file);

    nuraft::ulong next_slot() const override;
    nuraft::ulong start_index() const override;
    nuraft::ptr<nuraft::log_entry> last_entry() const override;
    nuraft::ulong append(nuraft::ptr<nuraft::log_entry>& entry) override;
    void write_at(nuraft::ulong index,
                  nuraft::ptr<nuraft::log_entry>& entry) override;
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
    void persist_locked() const;

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    nuraft::ulong start_index_{1};
    std::map<nuraft::ulong, nuraft::ptr<nuraft::log_entry>> entries_;
};

}  // namespace strongkv
