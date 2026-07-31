#include "logging/logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace strongkv {

LogLevel parse_log_level(const std::string& value) {
    if (value == "debug") {
        return LogLevel::kDebug;
    }
    if (value == "info") {
        return LogLevel::kInfo;
    }
    if (value == "warn") {
        return LogLevel::kWarn;
    }
    if (value == "error") {
        return LogLevel::kError;
    }
    throw std::invalid_argument("unknown log level: " + value);
}

Logger::Logger(const std::filesystem::path& path, LogLevel level)
    : level_(static_cast<int>(level)) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    stream_.open(path, std::ios::app);
    if (!stream_) {
        throw std::runtime_error("cannot open log file: " + path.string());
    }
}

Logger::~Logger() {
    flush();
}

void Logger::debug(const std::string& line) {
    log(LogLevel::kDebug, line);
}

void Logger::info(const std::string& line) {
    log(LogLevel::kInfo, line);
}

void Logger::warn(const std::string& line) {
    log(LogLevel::kWarn, line);
}

void Logger::err(const std::string& line) {
    log(LogLevel::kError, line);
}

void Logger::set_level(int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

int Logger::get_level() {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Logger::put_details(int level, const char* source_file,
                         const char* func_name, std::size_t line_number,
                         const std::string& line) {
    std::ostringstream message;
    message << line;
    if (source_file != nullptr && func_name != nullptr &&
        level >= static_cast<int>(LogLevel::kDebug)) {
        message << " [" << source_file << ':' << line_number
                << ' ' << func_name << ']';
    }
    log(normalize(level), message.str());
}

void Logger::log(LogLevel level, const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(level) > level_) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif

    stream_ << std::put_time(&local, "%Y-%m-%dT%H:%M:%S")
            << ' ' << name(level) << ' ' << line << '\n';
    stream_.flush();
    if (level == LogLevel::kError) {
        std::cerr << name(level) << ' ' << line << std::endl;
    }
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_) {
        stream_.flush();
    }
}

const char* Logger::name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::kDebug:
        return "DEBUG";
    case LogLevel::kInfo:
        return "INFO";
    case LogLevel::kWarn:
        return "WARN";
    case LogLevel::kError:
        return "ERROR";
    }
    return "INFO";
}

LogLevel Logger::normalize(int level) noexcept {
    if (level <= static_cast<int>(LogLevel::kError)) {
        return LogLevel::kError;
    }
    if (level == static_cast<int>(LogLevel::kWarn)) {
        return LogLevel::kWarn;
    }
    if (level == static_cast<int>(LogLevel::kInfo)) {
        return LogLevel::kInfo;
    }
    return LogLevel::kDebug;
}

}  // namespace strongkv
