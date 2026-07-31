#include "state_machine/kv_state_machine.h"

#include "common/binary_codec.h"
#include "logging/logger.h"
#include "storage/file_util.h"

#include <libnuraft/cluster_config.hxx>
#include <libnuraft/snapshot.hxx>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace strongkv {
namespace {

constexpr FileMagic kStateMagic{{'S', 'K', 'V', 'S'}};
constexpr FileMagic kSnapshotMagic{{'S', 'K', 'S', 'N'}};
constexpr std::uint16_t kStateVersion = 1;
constexpr std::uint16_t kSnapshotRecordVersion = 1;
constexpr std::uint64_t kMaximumItemCount = 100000000;
constexpr std::size_t kMaximumStateFile = 1024ULL * 1024 * 1024;
constexpr std::size_t kMaximumCachedResults = 65536;

nuraft::ptr<nuraft::buffer> to_buffer(
        const std::vector<std::uint8_t>& bytes) {
    auto output = nuraft::buffer::alloc(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(output->data_begin(), bytes.data(), bytes.size());
    }
    output->pos(0);
    return output;
}

std::vector<std::uint8_t> to_bytes(const nuraft::buffer& buffer) {
    const auto* begin = buffer.data_begin();
    return {begin, begin + buffer.size()};
}

bool is_write(CommandType type) noexcept {
    return type == CommandType::kSet || type == CommandType::kDel ||
           type == CommandType::kIncr || type == CommandType::kDecr;
}

const char* command_name(CommandType type) noexcept {
    switch (type) {
    case CommandType::kSet:
        return "SET";
    case CommandType::kDel:
        return "DEL";
    case CommandType::kIncr:
        return "INCR";
    case CommandType::kDecr:
        return "DECR";
    case CommandType::kReadBarrier:
        return "READ_BARRIER";
    }
    return "UNKNOWN";
}

}  // namespace

KvStateMachine::KvStateMachine(
        std::filesystem::path state_dir,
        std::filesystem::path snapshot_dir,
        std::size_t max_field_size,
        nuraft::ptr<Logger> logger)
    : state_file_(std::move(state_dir) / "kv-state.bin"),
      snapshot_file_(std::move(snapshot_dir) / "latest.bin"),
      max_field_size_(max_field_size),
      logger_(std::move(logger)) {
    if (max_field_size_ == 0) {
        throw std::invalid_argument("state machine field limit cannot be zero");
    }
    load_state();
    load_snapshot_metadata();
}

nuraft::ptr<nuraft::buffer> KvStateMachine::commit(
        nuraft::ulong log_index, nuraft::buffer& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    ApplyResult result;
    try {
        const Command command =
            decode_command(data, max_field_size_);
        result = apply_locked(command);
        if (logger_) {
            logger_->debug(
                "state-machine commit index=" + std::to_string(log_index) +
                " command=" + command_name(command.type) +
                (command.key.empty() ? std::string()
                                     : " key=" + command.key));
        }
    } catch (const std::exception& error) {
        result = ApplyResult::error(
            ResultCode::kBadCommand,
            std::string("invalid replicated command: ") + error.what());
        if (logger_) {
            logger_->err(
                "invalid command at committed index " +
                std::to_string(log_index) + ": " + error.what());
        }
    }

    state_.last_commit_index = log_index;
    recent_results_[log_index] = result;
    while (recent_results_.size() > kMaximumCachedResults) {
        recent_results_.erase(recent_results_.begin());
    }
    return encode_result(result);
}

void KvStateMachine::commit_config(
        nuraft::ulong log_index,
        nuraft::ptr<nuraft::cluster_config>& new_config) {
    static_cast<void>(new_config);
    std::lock_guard<std::mutex> lock(mutex_);
    state_.last_commit_index = log_index;
}

ApplyResult KvStateMachine::apply_locked(const Command& command) {
    if (!is_write(command.type)) {
        if (command.type == CommandType::kReadBarrier) {
            return ApplyResult::ok();
        }
        return ApplyResult::error(
            ResultCode::kBadCommand, "unknown command type");
    }

    if (command.client_id == 0) {
        return apply_new_request_locked(command);
    }
    if (command.request_id == 0) {
        return ApplyResult::error(
            ResultCode::kBadCommand,
            "request_id must be non-zero when client_id is set");
    }

    const auto found = state_.dedup.find(command.client_id);
    if (found != state_.dedup.end()) {
        if (command.request_id == found->second.request_id) {
            return found->second.result;
        }
        if (command.request_id < found->second.request_id) {
            return ApplyResult::error(
                ResultCode::kStaleRequest,
                "request_id is older than the last applied request");
        }
    }

    ApplyResult result = apply_new_request_locked(command);
    state_.dedup[command.client_id] =
        DedupRecord{command.request_id, result};
    return result;
}

ApplyResult KvStateMachine::apply_new_request_locked(
        const Command& command) {
    switch (command.type) {
    case CommandType::kSet:
        state_.kv[command.key] = command.value;
        return ApplyResult::ok();

    case CommandType::kDel: {
        const auto erased = state_.kv.erase(command.key);
        return ApplyResult::integer_value(
            static_cast<std::int64_t>(erased));
    }

    case CommandType::kIncr:
    case CommandType::kDecr: {
        std::int64_t current = 0;
        const auto found = state_.kv.find(command.key);
        if (found != state_.kv.end() &&
            !parse_int64(found->second, current)) {
            return ApplyResult::error(
                ResultCode::kNotInteger,
                "value is not a valid int64");
        }
        if (command.type == CommandType::kIncr) {
            if (current == std::numeric_limits<std::int64_t>::max()) {
                return ApplyResult::error(
                    ResultCode::kOverflow, "int64 overflow");
            }
            ++current;
        } else {
            if (current == std::numeric_limits<std::int64_t>::min()) {
                return ApplyResult::error(
                    ResultCode::kOverflow, "int64 underflow");
            }
            --current;
        }
        state_.kv[command.key] = std::to_string(current);
        return ApplyResult::integer_value(current);
    }

    case CommandType::kReadBarrier:
        return ApplyResult::ok();
    }
    return ApplyResult::error(ResultCode::kBadCommand,
                              "unknown command type");
}

bool KvStateMachine::parse_int64(
        const std::string& value, std::int64_t& output) noexcept {
    if (value.empty()) {
        return false;
    }
    std::size_t position = 0;
    bool negative = false;
    if (value[position] == '-' || value[position] == '+') {
        negative = value[position] == '-';
        if (++position == value.size()) {
            return false;
        }
    }

    const std::uint64_t limit = negative
        ? (static_cast<std::uint64_t>(
               std::numeric_limits<std::int64_t>::max()) + 1U)
        : static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max());
    std::uint64_t magnitude = 0;
    for (; position < value.size(); ++position) {
        const unsigned char c =
            static_cast<unsigned char>(value[position]);
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(c - '0');
        if (magnitude > (limit - digit) / 10U) {
            return false;
        }
        magnitude = magnitude * 10U + digit;
    }

    if (negative) {
        if (magnitude == limit) {
            output = std::numeric_limits<std::int64_t>::min();
        } else {
            output = -static_cast<std::int64_t>(magnitude);
        }
    } else {
        output = static_cast<std::int64_t>(magnitude);
    }
    return true;
}

std::optional<std::string> KvStateMachine::get(
        const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = state_.kv.find(key);
    if (found == state_.kv.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<std::optional<std::string>> KvStateMachine::get_many(
        const std::vector<std::string>& keys) const {
    std::vector<std::optional<std::string>> output;
    output.reserve(keys.size());
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& key : keys) {
        const auto found = state_.kv.find(key);
        if (found == state_.kv.end()) {
            output.emplace_back(std::nullopt);
        } else {
            output.emplace_back(found->second);
        }
    }
    return output;
}

std::optional<ApplyResult> KvStateMachine::take_result(
        nuraft::ulong log_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = recent_results_.find(log_index);
    if (found == recent_results_.end()) {
        return std::nullopt;
    }
    ApplyResult output = std::move(found->second);
    recent_results_.erase(found);
    return output;
}

void KvStateMachine::checkpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    persist_state_locked();
}

std::size_t KvStateMachine::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.kv.size();
}

nuraft::ulong KvStateMachine::last_commit_index() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.last_commit_index;
}

std::vector<std::uint8_t>
KvStateMachine::serialize_state_locked() const {
    BinaryWriter writer;
    writer.put_u16(kStateVersion);
    writer.put_u16(0);
    writer.put_u64(state_.last_commit_index);
    writer.put_u64(state_.kv.size());
    for (const auto& [key, value] : state_.kv) {
        writer.put_string(key);
        writer.put_string(value);
    }
    writer.put_u64(state_.dedup.size());
    for (const auto& [client_id, record] : state_.dedup) {
        const auto encoded_result = encode_result_bytes(record.result);
        writer.put_u64(client_id);
        writer.put_u64(record.request_id);
        writer.put_u32(static_cast<std::uint32_t>(
            encoded_result.size()));
        writer.put_raw(encoded_result.data(), encoded_result.size());
    }
    return writer.take();
}

KvStateMachine::StateData KvStateMachine::deserialize_state(
        const std::vector<std::uint8_t>& payload) const {
    BinaryReader reader(payload);
    if (reader.get_u16() != kStateVersion || reader.get_u16() != 0) {
        throw DecodeError("unsupported KV state version");
    }

    StateData output;
    output.last_commit_index = reader.get_u64();
    const std::uint64_t kv_count = reader.get_u64();
    if (kv_count > kMaximumItemCount) {
        throw DecodeError("KV item count is unreasonable");
    }
    for (std::uint64_t i = 0; i < kv_count; ++i) {
        std::string key = reader.get_string(max_field_size_);
        std::string value = reader.get_string(max_field_size_);
        if (!output.kv.emplace(std::move(key), std::move(value)).second) {
            throw DecodeError("duplicate key in durable KV state");
        }
    }

    const std::uint64_t dedup_count = reader.get_u64();
    if (dedup_count > kMaximumItemCount) {
        throw DecodeError("dedup record count is unreasonable");
    }
    for (std::uint64_t i = 0; i < dedup_count; ++i) {
        const std::uint64_t client_id = reader.get_u64();
        DedupRecord record;
        record.request_id = reader.get_u64();
        const std::uint32_t result_size = reader.get_u32();
        const auto result_bytes = reader.get_bytes(result_size);
        record.result = decode_result_bytes(
            result_bytes.data(), result_bytes.size(), max_field_size_);
        if (client_id == 0 || record.request_id == 0 ||
            !output.dedup.emplace(client_id, std::move(record)).second) {
            throw DecodeError("invalid duplicate dedup record");
        }
    }
    reader.require_end();
    return output;
}

void KvStateMachine::install_state_locked(StateData state) {
    state_ = std::move(state);
}

void KvStateMachine::persist_state_locked() const {
    write_checked_record(
        state_file_, kStateMagic, serialize_state_locked());
}

void KvStateMachine::load_state() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!std::filesystem::exists(state_file_)) {
        return;
    }
    const auto payload = read_checked_record(
        state_file_, kStateMagic, kMaximumStateFile);
    install_state_locked(deserialize_state(payload));
}

std::vector<std::uint8_t>
KvStateMachine::serialize_snapshot_record(
        nuraft::snapshot& metadata,
        const std::vector<std::uint8_t>& state_payload) const {
    auto metadata_buffer = metadata.serialize();
    if (metadata_buffer->size() >
            std::numeric_limits<std::uint32_t>::max() ||
        state_payload.size() >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("snapshot record exceeds format limit");
    }
    BinaryWriter writer;
    writer.put_u16(kSnapshotRecordVersion);
    writer.put_u16(0);
    writer.put_u32(static_cast<std::uint32_t>(metadata_buffer->size()));
    writer.put_raw(metadata_buffer->data_begin(), metadata_buffer->size());
    writer.put_u32(static_cast<std::uint32_t>(state_payload.size()));
    writer.put_raw(state_payload.data(), state_payload.size());
    return writer.take();
}

KvStateMachine::SnapshotData
KvStateMachine::deserialize_snapshot_record(
        const std::vector<std::uint8_t>& record) const {
    BinaryReader reader(record);
    if (reader.get_u16() != kSnapshotRecordVersion ||
        reader.get_u16() != 0) {
        throw DecodeError("unsupported snapshot record version");
    }
    const auto metadata_bytes = reader.get_bytes(reader.get_u32());
    const auto state_bytes = reader.get_bytes(reader.get_u32());
    reader.require_end();

    auto metadata_buffer = to_buffer(metadata_bytes);
    SnapshotData output;
    output.metadata =
        nuraft::snapshot::deserialize(*metadata_buffer);
    output.state_payload = state_bytes;
    const StateData decoded = deserialize_state(output.state_payload);
    if (decoded.last_commit_index !=
        output.metadata->get_last_log_idx()) {
        throw DecodeError(
            "snapshot metadata/state index mismatch");
    }
    return output;
}

KvStateMachine::SnapshotData
KvStateMachine::load_snapshot_file() const {
    const auto record = read_checked_record(
        snapshot_file_, kSnapshotMagic, kMaximumStateFile);
    return deserialize_snapshot_record(record);
}

void KvStateMachine::store_snapshot_file(
        nuraft::snapshot& metadata,
        const std::vector<std::uint8_t>& state_payload) const {
    write_checked_record(
        snapshot_file_, kSnapshotMagic,
        serialize_snapshot_record(metadata, state_payload));
}

void KvStateMachine::load_snapshot_metadata() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!std::filesystem::exists(snapshot_file_)) {
        return;
    }
    SnapshotData snapshot = load_snapshot_file();
    if (snapshot.metadata->get_last_log_idx() >
        state_.last_commit_index) {
        install_state_locked(
            deserialize_state(snapshot.state_payload));
    }
    last_snapshot_ = std::move(snapshot.metadata);
}

void KvStateMachine::create_snapshot(
        nuraft::snapshot& snapshot,
        nuraft::async_result<bool>::handler_type& when_done) {
    bool success = false;
    nuraft::ptr<std::exception> error = nullptr;
    try {
        std::vector<std::uint8_t> state_payload;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.last_commit_index !=
                snapshot.get_last_log_idx()) {
                throw std::runtime_error(
                    "snapshot index does not match applied index");
            }
            state_payload = serialize_state_locked();
        }

        store_snapshot_file(snapshot, state_payload);
        auto cloned_buffer = snapshot.serialize();
        auto cloned =
            nuraft::snapshot::deserialize(*cloned_buffer);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_snapshot_ = std::move(cloned);
        }
        success = true;
        if (logger_) {
            logger_->info(
                "snapshot completed index=" +
                std::to_string(snapshot.get_last_log_idx()));
        }
    } catch (const std::exception& exception) {
        error = nuraft::cs_new<std::runtime_error>(exception.what());
        if (logger_) {
            logger_->err(
                "snapshot failed index=" +
                std::to_string(snapshot.get_last_log_idx()) +
                " error=" + exception.what());
        }
    }
    when_done(success, error);
}

nuraft::ptr<nuraft::snapshot>
KvStateMachine::last_snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_snapshot_;
}

int KvStateMachine::read_logical_snp_obj(
        nuraft::snapshot& snapshot, void*& user_snapshot_context,
        nuraft::ulong object_id,
        nuraft::ptr<nuraft::buffer>& data_out,
        bool& is_last_object) {
    static_cast<void>(user_snapshot_context);
    if (object_id != 0) {
        data_out = nullptr;
        is_last_object = true;
        return -1;
    }
    try {
        const SnapshotData stored = load_snapshot_file();
        if (stored.metadata->get_last_log_idx() !=
            snapshot.get_last_log_idx()) {
            data_out = nullptr;
            is_last_object = true;
            return -1;
        }
        data_out = to_buffer(stored.state_payload);
        is_last_object = true;
        return 0;
    } catch (const std::exception& error) {
        if (logger_) {
            logger_->err(
                "snapshot read failed index=" +
                std::to_string(snapshot.get_last_log_idx()) +
                " error=" + error.what());
        }
        data_out = nullptr;
        is_last_object = true;
        return -1;
    }
}

void KvStateMachine::save_logical_snp_obj(
        nuraft::snapshot& snapshot, nuraft::ulong& object_id,
        nuraft::buffer& data, bool is_first_object,
        bool is_last_object) {
    if (object_id != 0 || !is_first_object || !is_last_object) {
        throw std::runtime_error(
            "StrongKV snapshot must be one logical object");
    }
    const auto payload = to_bytes(data);
    const StateData decoded = deserialize_state(payload);
    if (decoded.last_commit_index != snapshot.get_last_log_idx()) {
        throw std::runtime_error(
            "received snapshot state index mismatch");
    }
    store_snapshot_file(snapshot, payload);
    auto cloned_buffer = snapshot.serialize();
    auto cloned = nuraft::snapshot::deserialize(*cloned_buffer);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_snapshot_ = std::move(cloned);
    }
    ++object_id;
    if (logger_) {
        logger_->info(
            "snapshot object received index=" +
            std::to_string(snapshot.get_last_log_idx()));
    }
}

bool KvStateMachine::apply_snapshot(nuraft::snapshot& snapshot) {
    try {
        SnapshotData stored = load_snapshot_file();
        if (stored.metadata->get_last_log_idx() !=
            snapshot.get_last_log_idx()) {
            return false;
        }
        StateData replacement =
            deserialize_state(stored.state_payload);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            install_state_locked(std::move(replacement));
            last_snapshot_ = std::move(stored.metadata);
            recent_results_.clear();
        }
        if (logger_) {
            logger_->info(
                "snapshot applied index=" +
                std::to_string(snapshot.get_last_log_idx()));
        }
        return true;
    } catch (const std::exception& error) {
        if (logger_) {
            logger_->err(
                "snapshot apply failed index=" +
                std::to_string(snapshot.get_last_log_idx()) +
                " error=" + error.what());
        }
        return false;
    }
}

void KvStateMachine::free_user_snp_ctx(
        void*& user_snapshot_context) {
    user_snapshot_context = nullptr;
}

}  // namespace strongkv
