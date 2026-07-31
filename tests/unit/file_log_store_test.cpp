#include "test.h"

#include "storage/file_log_store.h"

#include <libnuraft/nuraft.hxx>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace strongkv;

namespace {

std::filesystem::path temporary_log_file() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("strongkv-log-" + std::to_string(stamp)) / "log.bin";
}

nuraft::ptr<nuraft::log_entry> entry(nuraft::ulong term,
                                      const std::string& value) {
    auto buffer = nuraft::buffer::alloc(value.size());
    if (!value.empty()) {
        std::memcpy(buffer->data_begin(), value.data(), value.size());
    }
    return nuraft::cs_new<nuraft::log_entry>(term, buffer);
}

std::string value_of(const nuraft::ptr<nuraft::log_entry>& value) {
    const auto& buffer = value->get_buf();
    return std::string(
        reinterpret_cast<const char*>(buffer.data_begin()), buffer.size());
}

}  // namespace

SKV_TEST(file_log_store_persists_append_and_overwrite) {
    const auto path = temporary_log_file();
    {
        FileLogStore store(path);
        auto first = entry(1, "one");
        auto second = entry(1, "two");
        SKV_EXPECT_EQ(store.append(first), nuraft::ulong{1});
        SKV_EXPECT_EQ(store.append(second), nuraft::ulong{2});
        store.end_of_append_batch(1, 2);
        SKV_EXPECT_EQ(store.last_durable_index(), nuraft::ulong{2});
        auto replacement = entry(2, "replacement");
        store.write_at(2, replacement);
        store.end_of_append_batch(2, 1);
    }
    {
        FileLogStore reopened(path);
        SKV_EXPECT_EQ(reopened.start_index(), nuraft::ulong{1});
        SKV_EXPECT_EQ(reopened.next_slot(), nuraft::ulong{3});
        SKV_EXPECT_EQ(reopened.term_at(2), nuraft::ulong{2});
        SKV_EXPECT_EQ(value_of(reopened.entry_at(2)),
                      std::string("replacement"));
    }
    std::filesystem::remove_all(path.parent_path());
}

SKV_TEST(file_log_store_compaction_survives_restart) {
    const auto path = temporary_log_file();
    {
        FileLogStore store(path);
        for (int i = 0; i < 5; ++i) {
            auto value = entry(1, std::to_string(i));
            store.append(value);
        }
        store.end_of_append_batch(1, 5);
        SKV_EXPECT_TRUE(store.compact(3));
        SKV_EXPECT_EQ(store.start_index(), nuraft::ulong{4});
    }
    {
        FileLogStore reopened(path);
        SKV_EXPECT_EQ(reopened.start_index(), nuraft::ulong{4});
        SKV_EXPECT_EQ(reopened.next_slot(), nuraft::ulong{6});
        SKV_EXPECT_EQ(reopened.term_at(3), nuraft::ulong{0});
        SKV_EXPECT_EQ(value_of(reopened.entry_at(4)), std::string("3"));
    }
    std::filesystem::remove_all(path.parent_path());
}

SKV_TEST(file_log_store_truncates_torn_tail_on_restart) {
    const auto path = temporary_log_file();
    {
        FileLogStore store(path);
        auto first = entry(1, "one");
        auto second = entry(1, "two");
        store.append(first);
        store.append(second);
        store.end_of_append_batch(1, 2);
    }
    const auto valid_size = std::filesystem::file_size(path);
    {
        std::ofstream output(
            path, std::ios::binary | std::ios::app);
        const char torn_tail[3]{'\x20', '\x00', '\x01'};
        output.write(torn_tail, sizeof(torn_tail));
    }
    SKV_EXPECT_TRUE(std::filesystem::file_size(path) > valid_size);
    {
        FileLogStore reopened(path);
        SKV_EXPECT_EQ(reopened.next_slot(), nuraft::ulong{3});
        SKV_EXPECT_EQ(value_of(reopened.entry_at(2)), std::string("two"));
    }
    SKV_EXPECT_EQ(std::filesystem::file_size(path), valid_size);
    std::filesystem::remove_all(path.parent_path());
}
