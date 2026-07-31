#include <strongkv/client.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr
            << "Usage: strongkv-concurrent-incr"
               " <password> <threads> <increments-per-thread>"
               " <seed> [seed ...]\n";
        return 2;
    }

    const std::string password = argv[1];
    const int thread_count = std::stoi(argv[2]);
    const int increments_per_thread = std::stoi(argv[3]);
    std::vector<std::string> seeds(argv + 4, argv + argc);
    if (thread_count <= 0 || increments_per_thread <= 0) {
        std::cerr << "thread and increment counts must be positive\n";
        return 2;
    }

    try {
        strongkv::Client setup(seeds);
        setup.connect();
        setup.auth(password);
        setup.set("concurrent-counter", "0");

        std::atomic<int> completed{0};
        std::mutex error_mutex;
        std::string first_error;
        std::vector<std::thread> workers;
        for (int thread = 0; thread < thread_count; ++thread) {
            workers.emplace_back([&, thread] {
                try {
                    strongkv::Client client(seeds);
                    client.connect();
                    client.auth(password);
                    for (int i = 0; i < increments_per_thread; ++i) {
                        client.incr("concurrent-counter");
                    }
                    completed.fetch_add(1);
                } catch (const std::exception& error) {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (first_error.empty()) {
                        first_error =
                            "worker " + std::to_string(thread) +
                            ": " + error.what();
                    }
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        if (!first_error.empty()) {
            std::cerr << first_error << '\n';
            return 1;
        }

        const auto final = setup.get("concurrent-counter");
        const std::int64_t expected =
            static_cast<std::int64_t>(thread_count) *
            increments_per_thread;
        if (completed.load() != thread_count || !final ||
            *final != std::to_string(expected)) {
            std::cerr
                << "expected " << expected << ", got "
                << (final ? *final : "(nil)") << '\n';
            return 1;
        }
        std::cout
            << "concurrent INCR passed: " << expected
            << " committed increments\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
