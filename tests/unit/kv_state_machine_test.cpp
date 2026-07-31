#include "test.h"

#include "state_machine/command.h"
#include "state_machine/kv_state_machine.h"

#include <chrono>
#include <filesystem>
#include <limits>
#include <string>

using namespace strongkv;

namespace {

struct StateFixture {
    StateFixture() {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            ("strongkv-state-" + std::to_string(stamp));
    }
    ~StateFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::filesystem::path root;
};

ApplyResult commit(KvStateMachine& machine, nuraft::ulong index,
                   Command command) {
    auto encoded = encode_command(command);
    auto result = machine.commit(index, *encoded);
    return decode_result(*result, 1024 * 1024);
}

Command command(CommandType type, std::string key = {},
                std::string value = {}, std::uint64_t client = 0,
                std::uint64_t request = 0) {
    Command output;
    output.type = type;
    output.key = std::move(key);
    output.value = std::move(value);
    output.client_id = client;
    output.request_id = request;
    return output;
}

}  // namespace

SKV_TEST(state_machine_applies_basic_commands_in_order) {
    StateFixture fixture;
    KvStateMachine machine(
        fixture.root / "state", fixture.root / "snapshot", 1024 * 1024);

    SKV_EXPECT_EQ(
        commit(machine, 1, command(CommandType::kSet, "foo", "bar")).code,
        ResultCode::kOk);
    SKV_EXPECT_EQ(machine.get("foo").value(), std::string("bar"));

    const auto deleted =
        commit(machine, 2, command(CommandType::kDel, "foo"));
    SKV_EXPECT_EQ(deleted.integer, std::int64_t{1});
    SKV_EXPECT_TRUE(!machine.get("foo").has_value());
    SKV_EXPECT_EQ(machine.last_commit_index(), nuraft::ulong{2});
}

SKV_TEST(state_machine_increment_handles_boundaries_and_non_integer) {
    StateFixture fixture;
    KvStateMachine machine(
        fixture.root / "state", fixture.root / "snapshot", 1024 * 1024);

    auto first =
        commit(machine, 1, command(CommandType::kIncr, "counter"));
    SKV_EXPECT_EQ(first.integer, std::int64_t{1});
    auto second =
        commit(machine, 2, command(CommandType::kDecr, "counter"));
    SKV_EXPECT_EQ(second.integer, std::int64_t{0});

    commit(machine, 3, command(
        CommandType::kSet, "counter",
        std::to_string(std::numeric_limits<std::int64_t>::max())));
    const auto overflow =
        commit(machine, 4, command(CommandType::kIncr, "counter"));
    SKV_EXPECT_EQ(overflow.code, ResultCode::kOverflow);
    SKV_EXPECT_EQ(
        machine.get("counter").value(),
        std::to_string(std::numeric_limits<std::int64_t>::max()));

    commit(machine, 5,
           command(CommandType::kSet, "counter", "12x"));
    const auto invalid =
        commit(machine, 6, command(CommandType::kDecr, "counter"));
    SKV_EXPECT_EQ(invalid.code, ResultCode::kNotInteger);
    SKV_EXPECT_EQ(machine.get("counter").value(), std::string("12x"));
}

SKV_TEST(state_machine_deduplicates_retried_increment) {
    StateFixture fixture;
    KvStateMachine machine(
        fixture.root / "state", fixture.root / "snapshot", 1024 * 1024);

    const auto request =
        command(CommandType::kIncr, "counter", {}, 42, 7);
    const auto first = commit(machine, 1, request);
    const auto retry = commit(machine, 2, request);
    SKV_EXPECT_EQ(first, retry);
    SKV_EXPECT_EQ(machine.get("counter").value(), std::string("1"));

    const auto stale = commit(
        machine, 3,
        command(CommandType::kIncr, "counter", {}, 42, 6));
    SKV_EXPECT_EQ(stale.code, ResultCode::kStaleRequest);
    SKV_EXPECT_EQ(machine.get("counter").value(), std::string("1"));
}

SKV_TEST(state_machine_recovers_kv_and_dedup_from_disk) {
    StateFixture fixture;
    {
        KvStateMachine machine(
            fixture.root / "state", fixture.root / "snapshot",
            1024 * 1024);
        commit(machine, 1,
               command(CommandType::kSet, "key", "value", 9, 1));
        commit(machine, 2,
               command(CommandType::kIncr, "counter", {}, 9, 2));
        machine.checkpoint();
    }
    {
        KvStateMachine reopened(
            fixture.root / "state", fixture.root / "snapshot",
            1024 * 1024);
        SKV_EXPECT_EQ(reopened.last_commit_index(), nuraft::ulong{2});
        SKV_EXPECT_EQ(reopened.get("key").value(), std::string("value"));
        const auto retry = commit(
            reopened, 3,
            command(CommandType::kIncr, "counter", {}, 9, 2));
        SKV_EXPECT_EQ(retry.integer, std::int64_t{1});
        SKV_EXPECT_EQ(reopened.get("counter").value(), std::string("1"));
    }
}

SKV_TEST(state_machine_exposes_each_batched_commit_result_once) {
    StateFixture fixture;
    KvStateMachine machine(
        fixture.root / "state", fixture.root / "snapshot", 1024 * 1024);

    commit(machine, 7, command(CommandType::kSet, "key", "value"));
    commit(machine, 8, command(CommandType::kDel, "missing"));

    const auto set_result = machine.take_result(7);
    const auto del_result = machine.take_result(8);
    SKV_EXPECT_TRUE(set_result.has_value());
    SKV_EXPECT_TRUE(del_result.has_value());
    SKV_EXPECT_EQ(set_result->code, ResultCode::kOk);
    SKV_EXPECT_EQ(del_result->integer, std::int64_t{0});
    SKV_EXPECT_TRUE(!machine.take_result(7).has_value());
}
