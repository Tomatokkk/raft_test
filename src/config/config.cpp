#include <strongkv/config.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace strongkv {
namespace {

std::string trim(const std::string& input) {
    const auto first = std::find_if_not(
        input.begin(), input.end(),
        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(
        input.rbegin(), input.rend(),
        [](unsigned char c) { return std::isspace(c) != 0; }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string unquote(std::string value) {
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string resolve_environment_reference(
        const std::string& value, const std::string& field) {
    if (value.size() < 4 || value.rfind("${", 0) != 0 ||
        value.back() != '}') {
        return value;
    }
    const std::string name = value.substr(2, value.size() - 3);
    if (name.empty()) {
        throw std::runtime_error(
            field + " contains an empty environment variable reference");
    }
    const char* resolved = std::getenv(name.c_str());
    if (resolved == nullptr || *resolved == '\0') {
        throw std::runtime_error(
            field + " requires non-empty environment variable " + name);
    }
    return resolved;
}

std::pair<std::string, std::string> split_assignment(
        const std::string& text, std::size_t line_number) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) {
        throw std::runtime_error(
            "config line " + std::to_string(line_number) +
            ": expected key: value");
    }
    const std::string key = trim(text.substr(0, colon));
    const std::string value = unquote(text.substr(colon + 1));
    if (key.empty()) {
        throw std::runtime_error(
            "config line " + std::to_string(line_number) + ": empty key");
    }
    return {key, value};
}

std::uint64_t parse_unsigned(const std::string& text,
                             const std::string& field) {
    if (text.empty()) {
        throw std::runtime_error("missing numeric config field: " + field);
    }
    std::uint64_t value = 0;
    for (const unsigned char c : text) {
        if (c < '0' || c > '9') {
            throw std::runtime_error(
                "invalid unsigned value for " + field + ": " + text);
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) /
                        10U) {
            throw std::runtime_error("numeric overflow for " + field);
        }
        value = value * 10U + digit;
    }
    return value;
}

template <typename T>
T parse_bounded(const std::string& text, const std::string& field,
                std::uint64_t minimum, std::uint64_t maximum) {
    const auto value = parse_unsigned(text, field);
    if (value < minimum || value > maximum) {
        throw std::runtime_error(
            field + " is outside allowed range [" +
            std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
    }
    return static_cast<T>(value);
}

bool parse_boolean(const std::string& text, const std::string& field) {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    throw std::runtime_error(
        "invalid boolean value for " + field + ": " + text);
}

void reject_unknown(const std::string& section, const std::string& key,
                    std::size_t line_number) {
    throw std::runtime_error(
        "config line " + std::to_string(line_number) +
        ": unknown field " + section + "." + key);
}

}  // namespace

std::string ClusterNode::raft_endpoint() const {
    return host + ":" + std::to_string(raft_port);
}

std::string ClusterNode::client_endpoint() const {
    return host + ":" + std::to_string(client_port);
}

Config Config::load(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open config file: " + path.string());
    }

    Config config;
    std::string section;
    bool cluster_nodes = false;
    ClusterNode* current_node = nullptr;
    std::string raw_line;
    std::size_t line_number = 0;

    while (std::getline(input, raw_line)) {
        ++line_number;
        if (!raw_line.empty() && raw_line.back() == '\r') {
            raw_line.pop_back();
        }
        if (raw_line.find('\t') != std::string::npos) {
            throw std::runtime_error(
                "config line " + std::to_string(line_number) +
                ": tabs are not allowed");
        }

        const auto comment = raw_line.find('#');
        if (comment != std::string::npos) {
            raw_line.erase(comment);
        }
        if (trim(raw_line).empty()) {
            continue;
        }

        const auto first = raw_line.find_first_not_of(' ');
        const std::size_t indent =
            first == std::string::npos ? 0 : first;
        const std::string text = trim(raw_line);

        if (indent == 0) {
            if (text.back() != ':') {
                throw std::runtime_error(
                    "config line " + std::to_string(line_number) +
                    ": top-level section must end in ':'");
            }
            section = text.substr(0, text.size() - 1);
            const std::set<std::string> allowed{
                "node", "network", "cluster", "security", "storage",
                "raft", "server", "logging"};
            if (allowed.count(section) == 0) {
                throw std::runtime_error(
                    "config line " + std::to_string(line_number) +
                    ": unknown section " + section);
            }
            cluster_nodes = false;
            current_node = nullptr;
            continue;
        }

        if (section.empty()) {
            throw std::runtime_error(
                "config line " + std::to_string(line_number) +
                ": field appears before a section");
        }

        if (section == "cluster" && text == "nodes:") {
            cluster_nodes = true;
            current_node = nullptr;
            continue;
        }

        std::string assignment = text;
        if (section == "cluster" && cluster_nodes &&
            assignment.rfind("- ", 0) == 0) {
            config.cluster_nodes.emplace_back();
            current_node = &config.cluster_nodes.back();
            assignment.erase(0, 2);
        }

        const auto [key, value] =
            split_assignment(assignment, line_number);

        if (section == "cluster") {
            if (!cluster_nodes || current_node == nullptr) {
                throw std::runtime_error(
                    "config line " + std::to_string(line_number) +
                    ": cluster fields must be inside cluster.nodes");
            }
            if (key == "id") {
                current_node->id = parse_bounded<std::int32_t>(
                    value, "cluster.nodes.id", 1,
                    std::numeric_limits<std::int32_t>::max());
            } else if (key == "host") {
                current_node->host = value;
            } else if (key == "raft_port") {
                current_node->raft_port = parse_bounded<std::uint16_t>(
                    value, "cluster.nodes.raft_port", 1, 65535);
            } else if (key == "client_port") {
                current_node->client_port = parse_bounded<std::uint16_t>(
                    value, "cluster.nodes.client_port", 1, 65535);
            } else {
                reject_unknown(section + ".nodes", key, line_number);
            }
            continue;
        }

        if (section == "node") {
            if (key == "id") {
                config.node_id = parse_bounded<std::int32_t>(
                    value, "node.id", 1,
                    std::numeric_limits<std::int32_t>::max());
            } else {
                reject_unknown(section, key, line_number);
            }
        } else if (section == "network") {
            if (key == "bind") {
                config.bind_address = value;
            } else if (key == "client_port") {
                config.client_port = parse_bounded<std::uint16_t>(
                    value, "network.client_port", 1, 65535);
            } else if (key == "raft_port") {
                config.raft_port = parse_bounded<std::uint16_t>(
                    value, "network.raft_port", 1, 65535);
            } else {
                reject_unknown(section, key, line_number);
            }
        } else if (section == "security") {
            if (key == "requirepass") {
                config.requirepass = resolve_environment_reference(
                    value, "security.requirepass");
            } else {
                reject_unknown(section, key, line_number);
            }
        } else if (section == "storage") {
            if (key == "data_dir") {
                config.data_dir = value;
            } else if (key == "raft_log_dir") {
                config.raft_log_dir = value;
            } else if (key == "snapshot_dir") {
                config.snapshot_dir = value;
            } else {
                reject_unknown(section, key, line_number);
            }
        } else if (section == "raft") {
            if (key == "heartbeat_ms") {
                config.heartbeat_ms = parse_bounded<std::int32_t>(
                    value, "raft.heartbeat_ms", 10, 60000);
            } else if (key == "election_timeout_lower_ms") {
                config.election_timeout_lower_ms =
                    parse_bounded<std::int32_t>(
                        value, "raft.election_timeout_lower_ms", 20, 120000);
            } else if (key == "election_timeout_upper_ms") {
                config.election_timeout_upper_ms =
                    parse_bounded<std::int32_t>(
                        value, "raft.election_timeout_upper_ms", 20, 120000);
            } else if (key == "client_request_timeout_ms") {
                config.client_request_timeout_ms =
                    parse_bounded<std::int32_t>(
                        value, "raft.client_request_timeout_ms", 100, 300000);
            } else if (key == "enable_lease_reads") {
                config.enable_lease_reads =
                    parse_boolean(value, "raft.enable_lease_reads");
            } else if (key == "snapshot_distance") {
                config.snapshot_distance = parse_bounded<std::int32_t>(
                    value, "raft.snapshot_distance", 1,
                    std::numeric_limits<std::int32_t>::max());
            } else if (key == "reserved_log_items") {
                config.reserved_log_items = parse_bounded<std::int32_t>(
                    value, "raft.reserved_log_items", 0,
                    std::numeric_limits<std::int32_t>::max());
            } else {
                reject_unknown(section, key, line_number);
            }
        } else if (section == "server") {
            if (key == "max_connections") {
                config.max_connections = parse_bounded<std::size_t>(
                    value, "server.max_connections", 1, 1000000);
            } else if (key == "max_request_size") {
                config.max_request_size = parse_bounded<std::size_t>(
                    value, "server.max_request_size", 64, 1024ULL * 1024 * 1024);
            } else {
                reject_unknown(section, key, line_number);
            }
        } else if (section == "logging") {
            if (key == "level") {
                config.log_level = value;
            } else if (key == "path") {
                config.log_path = value;
            } else {
                reject_unknown(section, key, line_number);
            }
        }
    }

    config.validate();
    return config;
}

void Config::validate() const {
    if (node_id <= 0) {
        throw std::runtime_error("node.id is required");
    }
    if (bind_address.empty() || client_port == 0 || raft_port == 0) {
        throw std::runtime_error(
            "network.bind, client_port, and raft_port are required");
    }
    if (cluster_nodes.size() < 3) {
        throw std::runtime_error(
            "StrongKV prototype requires at least three voting nodes");
    }

    std::set<std::int32_t> ids;
    std::set<std::string> raft_endpoints;
    std::set<std::string> client_endpoints;
    for (const auto& node : cluster_nodes) {
        if (node.id <= 0 || node.host.empty() ||
            node.raft_port == 0 || node.client_port == 0) {
            throw std::runtime_error(
                "each cluster node requires id, host, raft_port, client_port");
        }
        if (!ids.insert(node.id).second) {
            throw std::runtime_error("duplicate cluster node id");
        }
        if (!raft_endpoints.insert(node.raft_endpoint()).second) {
            throw std::runtime_error("duplicate cluster Raft endpoint");
        }
        if (!client_endpoints.insert(node.client_endpoint()).second) {
            throw std::runtime_error("duplicate cluster client endpoint");
        }
    }

    const auto* local = find_node(node_id);
    if (local == nullptr) {
        throw std::runtime_error("node.id is absent from cluster.nodes");
    }
    if (local->raft_port != raft_port ||
        local->client_port != client_port) {
        throw std::runtime_error(
            "local network ports do not match cluster.nodes entry");
    }
    if (requirepass.empty()) {
        throw std::runtime_error("security.requirepass must not be empty");
    }
    if (data_dir.empty() || raft_log_dir.empty() || snapshot_dir.empty()) {
        throw std::runtime_error("all storage directories are required");
    }
    if (heartbeat_ms >= election_timeout_lower_ms ||
        election_timeout_lower_ms >= election_timeout_upper_ms) {
        throw std::runtime_error(
            "require heartbeat_ms < election lower < election upper");
    }
    if (snapshot_distance <= reserved_log_items) {
        throw std::runtime_error(
            "raft.snapshot_distance must exceed reserved_log_items");
    }
    const std::set<std::string> levels{"debug", "info", "warn", "error"};
    if (levels.count(log_level) == 0) {
        throw std::runtime_error(
            "logging.level must be debug, info, warn, or error");
    }
    if (log_path.empty()) {
        throw std::runtime_error("logging.path is required");
    }
}

const ClusterNode& Config::local_node() const {
    const auto* node = find_node(node_id);
    if (node == nullptr) {
        throw std::logic_error("validated config lost local node");
    }
    return *node;
}

const ClusterNode* Config::find_node(std::int32_t id) const noexcept {
    const auto it = std::find_if(
        cluster_nodes.begin(), cluster_nodes.end(),
        [id](const ClusterNode& node) { return node.id == id; });
    return it == cluster_nodes.end() ? nullptr : &*it;
}

}  // namespace strongkv
