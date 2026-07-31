#pragma once

#include "state_machine/command.h"

#include <libnuraft/state_machine.hxx>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace strongkv {

class Logger;

// Applies only committed NuRaft entries. The complete applied state and dedup
// table are atomically durable at the index returned by last_commit_index().
class KvStateMachine final : public nuraft::state_machine {
public:
    KvStateMachine(std::filesystem::path state_dir,
                   std::filesystem::path snapshot_dir,
                   std::size_t max_field_size,
                   nuraft::ptr<Logger> logger = nullptr);

    nuraft::ptr<nuraft::buffer> commit(
        nuraft::ulong log_index, nuraft::buffer& data) override;
    void commit_config(
        nuraft::ulong log_index,
        nuraft::ptr<nuraft::cluster_config>& new_config) override;

    bool apply_snapshot(nuraft::snapshot& snapshot) override;
    int read_logical_snp_obj(
        nuraft::snapshot& snapshot, void*& user_snapshot_context,
        nuraft::ulong object_id,
        nuraft::ptr<nuraft::buffer>& data_out,
        bool& is_last_object) override;
    void save_logical_snp_obj(
        nuraft::snapshot& snapshot, nuraft::ulong& object_id,
        nuraft::buffer& data, bool is_first_object,
        bool is_last_object) override;
    void free_user_snp_ctx(void*& user_snapshot_context) override;
    nuraft::ptr<nuraft::snapshot> last_snapshot() override;
    nuraft::ulong last_commit_index() override;
    void create_snapshot(
        nuraft::snapshot& snapshot,
        nuraft::async_result<bool>::handler_type& when_done) override;

    std::optional<std::string> get(const std::string& key) const;
    std::size_t size() const;

private:
    struct DedupRecord {
        std::uint64_t request_id{0};
        ApplyResult result;
    };

    struct StateData {
        nuraft::ulong last_commit_index{0};
        std::map<std::string, std::string> kv;
        std::map<std::uint64_t, DedupRecord> dedup;
    };

    struct SnapshotData {
        nuraft::ptr<nuraft::snapshot> metadata;
        std::vector<std::uint8_t> state_payload;
    };

    ApplyResult apply_locked(const Command& command);
    ApplyResult apply_new_request_locked(const Command& command);
    static bool parse_int64(const std::string& value,
                            std::int64_t& output) noexcept;

    std::vector<std::uint8_t> serialize_state_locked() const;
    StateData deserialize_state(
        const std::vector<std::uint8_t>& payload) const;
    void install_state_locked(StateData state);
    void persist_state_locked() const;
    void load_state();

    std::vector<std::uint8_t> serialize_snapshot_record(
        nuraft::snapshot& metadata,
        const std::vector<std::uint8_t>& state_payload) const;
    SnapshotData deserialize_snapshot_record(
        const std::vector<std::uint8_t>& record) const;
    SnapshotData load_snapshot_file() const;
    void store_snapshot_file(
        nuraft::snapshot& metadata,
        const std::vector<std::uint8_t>& state_payload) const;
    void load_snapshot_metadata();

    std::filesystem::path state_file_;
    std::filesystem::path snapshot_file_;
    std::size_t max_field_size_;
    nuraft::ptr<Logger> logger_;

    mutable std::mutex mutex_;
    StateData state_;
    nuraft::ptr<nuraft::snapshot> last_snapshot_;
};

}  // namespace strongkv
