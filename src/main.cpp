#include "logging/logger.h"
#include "raft/raft_node.h"
#include "server/command_service.h"
#include "server/tcp_server.h"

#include <strongkv/config.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <thread>

namespace {

std::atomic<bool> stop_requested{false};

extern "C" void handle_signal(int) {
    stop_requested.store(true);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: strongkv-server <config.yaml>\n";
        return 2;
    }

    try {
        const strongkv::Config config =
            strongkv::Config::load(argv[1]);
        auto logger = nuraft::cs_new<strongkv::Logger>(
            config.log_path,
            strongkv::parse_log_level(config.log_level));
        logger->info("StrongKV starting, configuration loaded");

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        strongkv::RaftNode raft(config, logger);
        strongkv::CommandService commands(config, raft, logger);
        strongkv::TcpServer server(config, commands, logger);

        raft.start();
        server.start();
        while (!stop_requested.load() &&
               raft.fatal_storage_error() == 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100));
        }

        const int fatal_storage_error = raft.fatal_storage_error();
        server.stop();
        raft.stop();
        logger->info("StrongKV shutdown complete");
        logger->flush();
        return fatal_storage_error == 0 ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "StrongKV fatal error: " << error.what() << '\n';
        return 1;
    }
}
