#include <strongkv/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 7) {
        std::cerr
            << "Usage: strongkv-benchmark <password> <connections>"
               " <operations-per-connection> <read-percent>"
               " <value-bytes> <seed> [seed ...]\n";
        return 2;
    }

    const std::string password = argv[1];
    const int connection_count = std::stoi(argv[2]);
    const int operations_per_connection = std::stoi(argv[3]);
    const int read_percent = std::stoi(argv[4]);
    const int value_bytes = std::stoi(argv[5]);
    std::vector<std::string> seeds(argv + 6, argv + argc);
    if (connection_count <= 0 || operations_per_connection <= 0 ||
        read_percent < 0 || read_percent > 100 ||
        value_bytes < 0) {
        std::cerr << "invalid benchmark arguments\n";
        return 2;
    }

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<std::uint64_t> completed{0};
    std::mutex error_mutex;
    std::string first_error;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(connection_count));
    const std::string value(
        static_cast<std::size_t>(value_bytes), 'x');

    for (int worker = 0; worker < connection_count; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                strongkv::Client client(seeds);
                client.connect();
                client.auth(password);
                static_cast<void>(client.get("benchmark-warmup"));
                ready.fetch_add(1);
                while (!start.load()) {
                    std::this_thread::yield();
                }

                for (int operation = 0;
                     operation < operations_per_connection;
                     ++operation) {
                    const std::string key =
                        "benchmark:" + std::to_string(worker) + ":" +
                        std::to_string(operation % 1000);
                    if ((operation + worker) % 100 < read_percent) {
                        static_cast<void>(client.get(key));
                    } else {
                        client.set(key, value);
                    }
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (first_error.empty()) {
                    first_error =
                        "worker " + std::to_string(worker) +
                        ": " + error.what();
                }
                ready.fetch_add(1);
            }
        });
    }

    while (ready.load() < connection_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto begin = std::chrono::steady_clock::now();
    start.store(true);
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - begin)
            .count();
    if (!first_error.empty()) {
        std::cerr << first_error << '\n';
        return 1;
    }

    const auto operations = completed.load();
    std::cout << "operations=" << operations
              << " elapsed_seconds=" << std::fixed
              << std::setprecision(3) << elapsed
              << " ops_per_second=" << std::setprecision(1)
              << static_cast<double>(operations) / elapsed << '\n';
    return 0;
}
