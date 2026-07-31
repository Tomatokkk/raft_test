#include "test.h"

#include <strongkv/config.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace strongkv;

namespace {

std::filesystem::path write_config(const std::string& body) {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("strongkv-config-" + std::to_string(stamp) + ".yaml");
    std::ofstream output(path);
    output << body;
    output.close();
    return path;
}

const char* valid_config = R"YAML(
node:
  id: 2
network:
  bind: 0.0.0.0
  client_port: 7402
  raft_port: 7502
cluster:
  nodes:
    - id: 1
      host: 127.0.0.1
      raft_port: 7501
      client_port: 7401
    - id: 2
      host: 127.0.0.1
      raft_port: 7502
      client_port: 7402
    - id: 3
      host: 127.0.0.1
      raft_port: 7503
      client_port: 7403
security:
  requirepass: unit-test-password
storage:
  data_dir: ./data/node2
  raft_log_dir: ./data/node2/raft
  snapshot_dir: ./data/node2/snapshot
raft:
  heartbeat_ms: 100
  election_timeout_lower_ms: 300
  election_timeout_upper_ms: 600
  client_request_timeout_ms: 5000
  snapshot_distance: 100
  reserved_log_items: 10
server:
  max_connections: 100
  max_request_size: 1048576
logging:
  level: info
  path: ./logs/node2.log
)YAML";

}  // namespace

SKV_TEST(config_parser_loads_cluster_and_local_node) {
    const auto path = write_config(valid_config);
    const Config config = Config::load(path);
    std::filesystem::remove(path);

    SKV_EXPECT_EQ(config.node_id, 2);
    SKV_EXPECT_EQ(config.local_node().raft_endpoint(),
                  std::string("127.0.0.1:7502"));
    SKV_EXPECT_EQ(config.cluster_nodes.size(), std::size_t{3});
    SKV_EXPECT_EQ(config.max_request_size, std::size_t{1048576});
}

SKV_TEST(config_parser_rejects_port_mismatch) {
    std::string body(valid_config);
    const auto position = body.find("client_port: 7402");
    body.replace(position, std::string("client_port: 7402").size(),
                 "client_port: 7499");
    const auto path = write_config(body);
    SKV_EXPECT_THROW(Config::load(path));
    std::filesystem::remove(path);
}

SKV_TEST(config_parser_rejects_invalid_timeout_order) {
    std::string body(valid_config);
    const auto position = body.find("heartbeat_ms: 100");
    body.replace(position, std::string("heartbeat_ms: 100").size(),
                 "heartbeat_ms: 400");
    const auto path = write_config(body);
    SKV_EXPECT_THROW(Config::load(path));
    std::filesystem::remove(path);
}
