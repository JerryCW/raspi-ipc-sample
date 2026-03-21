#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/types.h"

namespace sc {

// ============================================================
// LogLevel — six-level logging
// ============================================================

enum class LogLevel : uint8_t {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARNING = 3,
    ERROR = 4,
    CRITICAL = 5,
};

inline const char* log_level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE:    return "TRACE";
        case LogLevel::DEBUG:    return "DEBUG";
        case LogLevel::INFO:     return "INFO";
        case LogLevel::WARNING:  return "WARNING";
        case LogLevel::ERROR:    return "ERROR";
        case LogLevel::CRITICAL: return "CRITICAL";
    }
    return "UNKNOWN";
}

inline LogLevel log_level_from_string(const std::string& s) {
    if (s == "TRACE")    return LogLevel::TRACE;
    if (s == "DEBUG")    return LogLevel::DEBUG;
    if (s == "INFO")     return LogLevel::INFO;
    if (s == "WARNING")  return LogLevel::WARNING;
    if (s == "ERROR")    return LogLevel::ERROR;
    if (s == "CRITICAL") return LogLevel::CRITICAL;
    return LogLevel::INFO;  // default
}

// ============================================================
// LogEntry — a single structured log record
// ============================================================

struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string module;
    std::thread::id thread_id;
    std::string message;
};

// ============================================================
// PerformanceMetrics — periodic performance snapshot
// ============================================================

struct PerformanceMetrics {
    double fps;
    uint32_t bitrate_kbps;
    double latency_ms;
    uint64_t dropped_frames;
    uint32_t active_connections;
};

// ============================================================
// ILogManager — structured logging interface
// ============================================================

class ILogManager {
public:
    virtual ~ILogManager() = default;

    virtual VoidResult initialize(const std::string& file_path,
                                  LogLevel level,
                                  uint32_t max_file_size_mb,
                                  uint32_t max_files,
                                  bool log_to_stdout) = 0;

    virtual void log(LogLevel level, const std::string& module,
                     const std::string& message) = 0;

    // Dynamic log level adjustment (takes effect immediately)
    virtual void set_level(LogLevel level) = 0;
    virtual LogLevel current_level() const = 0;

    // Performance metrics logging (every 30 seconds)
    virtual void log_metrics(const PerformanceMetrics& metrics) = 0;
};

// ============================================================
// LogManager — concrete implementation
// ============================================================

class LogManager : public ILogManager {
public:
    LogManager() = default;
    ~LogManager() override;

    VoidResult initialize(const std::string& file_path,
                          LogLevel level,
                          uint32_t max_file_size_mb,
                          uint32_t max_files,
                          bool log_to_stdout) override;

    void log(LogLevel level, const std::string& module,
             const std::string& message) override;

    void set_level(LogLevel level) override;
    LogLevel current_level() const override;

    void log_metrics(const PerformanceMetrics& metrics) override;

    // Exposed for testing
    static std::string redact_sensitive(const std::string& input);
    static std::string format_timestamp(std::chrono::system_clock::time_point tp);
    static std::string format_json(const LogEntry& entry);

private:
    void write_line(const std::string& json_line);
    void rotate_if_needed();
    void rotate_files();
    std::size_t file_size() const;

    std::string file_path_;
    uint32_t max_file_size_bytes_ = 10 * 1024 * 1024;
    uint32_t max_files_ = 5;
    bool log_to_stdout_ = true;
    bool initialized_ = false;

    std::atomic<LogLevel> level_{LogLevel::INFO};
    mutable std::mutex mutex_;
};

}  // namespace sc
