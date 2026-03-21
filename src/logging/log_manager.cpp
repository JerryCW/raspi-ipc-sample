#include "logging/log_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>

namespace sc {

// ============================================================
// Helpers
// ============================================================

namespace {

// Escape a string for JSON output
std::string json_escape(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:   result += c;      break;
        }
    }
    return result;
}

// Thread ID to hex string
std::string thread_id_to_string(std::thread::id id) {
    std::ostringstream oss;
    oss << "0x" << std::hex << id;
    return oss.str();
}

}  // namespace

// ============================================================
// Sensitive info redaction
// ============================================================

std::string LogManager::redact_sensitive(const std::string& input) {
    std::string result = input;

    // AWS Access Key IDs (AKIA followed by 16 alphanumeric chars)
    static const std::regex aws_access_key(R"(AKIA[0-9A-Z]{16})");
    result = std::regex_replace(result, aws_access_key, "[REDACTED]");

    // AWS Secret Access Keys (40-char base64-like strings after common prefixes)
    static const std::regex aws_secret_key(
        R"((aws_secret_access_key|SecretAccessKey|secret_access_key)\s*[=:]\s*\S+)");
    result = std::regex_replace(result, aws_secret_key, "$1=[REDACTED]");

    // AWS Session Tokens (long base64 strings after common prefixes)
    static const std::regex aws_session_token(
        R"((aws_session_token|SessionToken|session_token)\s*[=:]\s*\S+)");
    result = std::regex_replace(result, aws_session_token, "$1=[REDACTED]");

    // Private key content (BEGIN/END PRIVATE KEY blocks)
    static const std::regex private_key_block(
        R"(-----BEGIN[A-Z ]*PRIVATE KEY-----[\s\S]*?-----END[A-Z ]*PRIVATE KEY-----)");
    result = std::regex_replace(result, private_key_block, "[REDACTED]");

    // Certificate fingerprints (SHA-256 hex patterns like AA:BB:CC:...)
    static const std::regex cert_fingerprint(
        R"(([0-9A-Fa-f]{2}:){15,}[0-9A-Fa-f]{2})");
    result = std::regex_replace(result, cert_fingerprint, "[REDACTED]");

    return result;
}

// ============================================================
// Timestamp formatting (ISO 8601)
// ============================================================

std::string LogManager::format_timestamp(std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  tp.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &time_t_val);
#else
    gmtime_r(&time_t_val, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

// ============================================================
// JSON formatting
// ============================================================

std::string LogManager::format_json(const LogEntry& entry) {
    std::ostringstream oss;
    oss << "{\"timestamp\":\"" << format_timestamp(entry.timestamp)
        << "\",\"level\":\"" << log_level_to_string(entry.level)
        << "\",\"module\":\"" << json_escape(entry.module)
        << "\",\"thread_id\":\"" << thread_id_to_string(entry.thread_id)
        << "\",\"message\":\"" << json_escape(entry.message)
        << "\"}";
    return oss.str();
}

// ============================================================
// LogManager lifecycle
// ============================================================

LogManager::~LogManager() = default;

VoidResult LogManager::initialize(const std::string& file_path,
                                  LogLevel level,
                                  uint32_t max_file_size_mb,
                                  uint32_t max_files,
                                  bool log_to_stdout) {
    std::lock_guard<std::mutex> lock(mutex_);

    file_path_ = file_path;
    level_.store(level, std::memory_order_relaxed);
    max_file_size_bytes_ = max_file_size_mb * 1024u * 1024u;
    max_files_ = max_files;
    log_to_stdout_ = log_to_stdout;

    // Try to open/create the log file if path is not empty
    if (!file_path_.empty()) {
        std::ofstream test(file_path_, std::ios::app);
        if (!test.is_open()) {
            return ErrVoid(ErrorCode::Unknown,
                           "Cannot open log file: " + file_path_,
                           "LogManager::initialize");
        }
    }

    initialized_ = true;
    return OkVoid();
}

// ============================================================
// Dynamic level adjustment
// ============================================================

void LogManager::set_level(LogLevel level) {
    level_.store(level, std::memory_order_relaxed);
}

LogLevel LogManager::current_level() const {
    return level_.load(std::memory_order_relaxed);
}

// ============================================================
// Core log method
// ============================================================

void LogManager::log(LogLevel level, const std::string& module,
                     const std::string& message) {
    if (static_cast<uint8_t>(level) < static_cast<uint8_t>(current_level())) {
        return;
    }

    LogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.level = level;
    entry.module = module;
    entry.thread_id = std::this_thread::get_id();
    entry.message = redact_sensitive(message);

    std::string json_line = format_json(entry);
    write_line(json_line);
}

// ============================================================
// Performance metrics logging
// ============================================================

void LogManager::log_metrics(const PerformanceMetrics& metrics) {
    std::ostringstream msg;
    msg << "Performance: fps=" << metrics.fps
        << " bitrate_kbps=" << metrics.bitrate_kbps
        << " latency_ms=" << metrics.latency_ms
        << " dropped_frames=" << metrics.dropped_frames
        << " active_connections=" << metrics.active_connections;

    log(LogLevel::INFO, "Metrics", msg.str());
}

// ============================================================
// File I/O
// ============================================================

void LogManager::write_line(const std::string& json_line) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (log_to_stdout_) {
        std::cout << json_line << "\n";
    }

    if (!file_path_.empty() && initialized_) {
        rotate_if_needed();

        std::ofstream file(file_path_, std::ios::app);
        if (file.is_open()) {
            file << json_line << "\n";
        }
    }
}

std::size_t LogManager::file_size() const {
    std::ifstream file(file_path_, std::ios::ate | std::ios::binary);
    if (!file.is_open()) return 0;
    auto pos = file.tellg();
    return (pos < 0) ? 0 : static_cast<std::size_t>(pos);
}

void LogManager::rotate_if_needed() {
    if (max_file_size_bytes_ == 0) return;
    if (file_size() >= max_file_size_bytes_) {
        rotate_files();
    }
}

void LogManager::rotate_files() {
    // Delete the oldest file if it exceeds max_files
    std::string oldest = file_path_ + "." + std::to_string(max_files_);
    std::remove(oldest.c_str());

    // Shift existing rotated files: .N-1 → .N
    for (uint32_t i = max_files_ - 1; i >= 1; --i) {
        std::string src = file_path_ + "." + std::to_string(i);
        std::string dst = file_path_ + "." + std::to_string(i + 1);
        std::rename(src.c_str(), dst.c_str());
    }

    // Rename current file to .1
    std::rename(file_path_.c_str(), (file_path_ + ".1").c_str());
}

}  // namespace sc
