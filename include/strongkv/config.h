#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace strongkv {

struct ClusterNode {
    std::int32_t id{0};
    std::string host;
    std::uint16_t raft_port{0};
    std::uint16_t client_port{0};

    std::string raft_endpoint() const;
    std::string client_endpoint() const;
};

struct Config {
    std::int32_t node_id{0};

    std::string bind_address{"0.0.0.0"};
    std::uint16_t client_port{0};
    std::uint16_t raft_port{0};

    std::vector<ClusterNode> cluster_nodes;

    std::string requirepass;

    std::filesystem::path data_dir;
    std::filesystem::path raft_log_dir;
    std::filesystem::path snapshot_dir;

    std::int32_t heartbeat_ms{100};
    std::int32_t election_timeout_lower_ms{300};
    std::int32_t election_timeout_upper_ms{600};
    std::int32_t client_request_timeout_ms{5000};
    std::int32_t snapshot_distance{1000000};
    std::int32_t reserved_log_items{100000};

    std::size_t max_connections{10000};
    std::size_t max_request_size{1024 * 1024};

    std::string log_level{"info"};
    std::filesystem::path log_path;

    static Config load(const std::filesystem::path& path);
    void validate() const;

    const ClusterNode& local_node() const;
    const ClusterNode* find_node(std::int32_t id) const noexcept;
};

}  // namespace strongkv
