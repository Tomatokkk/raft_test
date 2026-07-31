#pragma once

#include <libnuraft/logger.hxx>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace strongkv {

enum class LogLevel : int {
    kError = 2,
    kWarn = 3,
    kInfo = 4,
    kDebug = 5,
};

LogLevel parse_log_level(const std::string& value);

class Logger final : public nuraft::logger {
public:
    Logger(const std::filesystem::path& path, LogLevel level);
    ~Logger() override;

    void debug(const std::string& line) override;
    void info(const std::string& line) override;
    void warn(const std::string& line) override;
    void err(const std::string& line) override;
    void set_level(int level) override;
    int get_level() override;
    void put_details(int level, const char* source_file,
                     const char* func_name, std::size_t line_number,
                     const std::string& line) override;

    void log(LogLevel level, const std::string& line);
    void flush();

private:
    static const char* name(LogLevel level) noexcept;
    static LogLevel normalize(int level) noexcept;

    std::mutex mutex_;
    std::ofstream stream_;
    int level_;
};

}  // namespace strongkv
