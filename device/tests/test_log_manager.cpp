#include <gtest/gtest.h>
#include "logging/log_manager.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

using namespace sc;

namespace fs = std::filesystem;

// ============================================================
// Helper: create a temp directory for file-based tests
// ============================================================

namespace {

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() / ("log_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    std::string path() const { return path_.string(); }
    std::string file(const std::string& name) const {
        return (path_ / name).string();
    }
private:
    fs::path path_;
};

// Read entire file content
std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

// ============================================================
// format_json() — six log levels produce correct JSON
// Validates: Requirements 10.1, 10.2
// ============================================================

TEST(LogManagerFormatJson, AllSixLevels) {
    const LogLevel levels[] = {
        LogLevel::TRACE, LogLevel::DEBUG, LogLevel::INFO,
        LogLevel::WARNING, LogLevel::ERROR, LogLevel::CRITICAL,
    };
    const char* names[] = {
        "TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL",
    };

    for (int i = 0; i < 6; ++i) {
        LogEntry entry;
        entry.timestamp = std::chrono::system_clock::now();
        entry.level = levels[i];
        entry.module = "TestModule";
        entry.thread_id = std::this_thread::get_id();
        entry.message = "hello";

        std::string json = LogManager::format_json(entry);

        // Verify it contains the correct level string
        std::string level_field = std::string("\"level\":\"") + names[i] + "\"";
        EXPECT_NE(json.find(level_field), std::string::npos)
            << "Level " << names[i] << " not found in: " << json;

        // Verify other required fields exist
        EXPECT_NE(json.find("\"timestamp\":\""), std::string::npos);
        EXPECT_NE(json.find("\"module\":\"TestModule\""), std::string::npos);
        EXPECT_NE(json.find("\"thread_id\":\""), std::string::npos);
        EXPECT_NE(json.find("\"message\":\"hello\""), std::string::npos);
    }
}


// ============================================================
// format_timestamp() — ISO 8601 format
// Validates: Requirements 10.2
// ============================================================

TEST(LogManagerTimestamp, ISO8601Format) {
    // Use a known time point: 2024-01-15T09:50:45.123Z UTC
    auto epoch = std::chrono::system_clock::from_time_t(0);
    auto tp = epoch + std::chrono::seconds(1705312245) + std::chrono::milliseconds(123);

    std::string ts = LogManager::format_timestamp(tp);

    // Should match ISO 8601 pattern: YYYY-MM-DDTHH:MM:SS.mmmZ
    std::regex iso_pattern(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)");
    EXPECT_TRUE(std::regex_match(ts, iso_pattern))
        << "Timestamp does not match ISO 8601: " << ts;

    // Verify the specific value (1705312245 = 2024-01-15T09:50:45 UTC)
    EXPECT_EQ(ts, "2024-01-15T09:50:45.123Z");
}

TEST(LogManagerTimestamp, EpochZero) {
    auto tp = std::chrono::system_clock::from_time_t(0);
    std::string ts = LogManager::format_timestamp(tp);
    EXPECT_EQ(ts, "1970-01-01T00:00:00.000Z");
}

// ============================================================
// redact_sensitive() — AWS credentials redaction
// Validates: Requirements 10.4, 15.6
// ============================================================

TEST(LogManagerRedact, AWSAccessKey) {
    std::string input = "Using key AKIAIOSFODNN7EXAMPLE for auth";
    std::string result = LogManager::redact_sensitive(input);
    EXPECT_EQ(result.find("AKIAIOSFODNN7EXAMPLE"), std::string::npos);
    EXPECT_NE(result.find("[REDACTED]"), std::string::npos);
}

TEST(LogManagerRedact, AWSSecretAccessKey) {
    std::string input = "aws_secret_access_key = wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    std::string result = LogManager::redact_sensitive(input);
    EXPECT_EQ(result.find("wJalrXUtnFEMI"), std::string::npos);
    EXPECT_NE(result.find("[REDACTED]"), std::string::npos);
}

TEST(LogManagerRedact, AWSSecretKeyColonFormat) {
    std::string input = "SecretAccessKey: wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    std::string result = LogManager::redact_sensitive(input);
    EXPECT_EQ(result.find("wJalrXUtnFEMI"), std::string::npos);
    EXPECT_NE(result.find("[REDACTED]"), std::string::npos);
}

TEST(LogManagerRedact, AWSSessionToken) {
    std::string input = "aws_session_token = FwoGZXIvYXdzEBYaDHqa0AP1/EXAMPLE+TOKEN";
    std::string result = LogManager::redact_sensitive(input);
    EXPECT_EQ(result.find("FwoGZXIvYXdzEBYaDHqa0AP1"), std::string::npos);
    EXPECT_NE(result.find("[REDACTED]"), std::string::npos);
}

TEST(LogManagerRedact, SessionTokenColonFormat) {
    std::string input = "SessionToken: FwoGZXIvYXdzEBYaDHqa0AP1/EXAMPLE+TOKEN";
    std::string result = LogManager::redact_sensitive(input);
    EXPECT_EQ(result.find("FwoGZXIvYXdzEBYaDHqa0AP1"), std::string::npos);
    EXPECT_NE(result.find("[REDACTED]"), std::string::npos);
}

TEST(LogManagerRedact, PrivateKeyBlock) {
    std::string input =
        "Key content: -----BEGIN PRIVATE KEY-----\n"
        "MIIEvgIBADANBgkqhkiG9w0BAQEFAASC\n"
        "-----END PRIVATE KEY-----";
    std::string result = LogManager::redact_sensitive(input);
    EXPECT_EQ(result.find("MIIEvgIBADANBgkqhkiG9w0BAQEFAASC"), std::string::npos);
    EXPECT_EQ(result.find("BEGIN PRIVATE KEY"), std::string::npos);
    EXPECT_NE(result.find("[REDACTED]"), std::string::npos);
}

TEST(LogManagerRedact, RSAPrivateKeyBlock) {
    std::string input =
        "-----BEGIN RSA PRIVATE KEY-----\n"
        "MIIEowIBAAKCAQEA0Z3VS5JJcds3xfn\n"
        "-----END RSA PRIVATE KEY-----";
    std::string result = LogManager::redact_sensitive(input);
    EXPECT_EQ(result.find("MIIEowIBAAKCAQEA0Z3VS5JJcds3xfn"), std::string::npos);
    EXPECT_NE(result.find("[REDACTED]"), std::string::npos);
}

TEST(LogManagerRedact, CertificateFingerprint) {
    std::string input =
        "Fingerprint: AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99"
        ":AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
    std::string result = LogManager::redact_sensitive(input);
    EXPECT_EQ(result.find("AA:BB:CC:DD"), std::string::npos);
    EXPECT_NE(result.find("[REDACTED]"), std::string::npos);
}

TEST(LogManagerRedact, NormalTextUnchanged) {
    std::string input = "Starting camera pipeline on /dev/video0 at 1280x720";
    std::string result = LogManager::redact_sensitive(input);
    EXPECT_EQ(result, input);
}

TEST(LogManagerRedact, EmptyString) {
    EXPECT_EQ(LogManager::redact_sensitive(""), "");
}


// ============================================================
// File rotation logic
// Validates: Requirements 10.3
// ============================================================

TEST(LogManagerRotation, RotatesWhenFileSizeExceeded) {
    TempDir tmp;
    std::string log_path = tmp.file("app.log");

    LogManager lm;
    // Initialize with tiny max size (1 byte as MB → use raw bytes approach)
    // We'll use 1 MB = too large. Instead, write enough data to trigger rotation.
    // The implementation uses max_file_size_mb * 1024 * 1024.
    // We need a workaround: initialize with 0 MB won't rotate (guard check).
    // Let's use 1 MB and write > 1MB of data.
    // Actually, looking at the code: max_file_size_bytes_ = max_file_size_mb * 1024u * 1024u
    // With max_file_size_mb = 1, that's 1MB. We need to write > 1MB.
    // That's too much for a unit test. Let's test with the smallest possible: 1 MB.
    // We'll write lines in a loop until rotation happens.

    // Better approach: we can test rotation by directly checking the mechanism.
    // Initialize with 1 MB, write a large block, check rotation.
    // Actually, let's just write enough to exceed 1MB.

    auto init_result = lm.initialize(log_path, LogLevel::TRACE, 1, 3, false);
    ASSERT_TRUE(init_result.is_ok());

    // Write enough data to exceed 1MB
    std::string big_message(1024, 'X');  // 1KB per message
    for (int i = 0; i < 1100; ++i) {
        lm.log(LogLevel::INFO, "Test", big_message);
    }

    // After writing > 1MB, rotation should have occurred
    EXPECT_TRUE(fs::exists(log_path)) << "Current log file should exist";
    EXPECT_TRUE(fs::exists(log_path + ".1")) << "Rotated file .1 should exist";
}

TEST(LogManagerRotation, MaxFilesRespected) {
    TempDir tmp;
    std::string log_path = tmp.file("app.log");

    LogManager lm;
    auto init_result = lm.initialize(log_path, LogLevel::TRACE, 1, 2, false);
    ASSERT_TRUE(init_result.is_ok());

    // Write enough to trigger multiple rotations (> 2MB for 2 rotations with 1MB limit)
    std::string big_message(1024, 'Y');
    for (int i = 0; i < 3300; ++i) {
        lm.log(LogLevel::INFO, "Test", big_message);
    }

    // With max_files=2, we should have app.log, app.log.1, app.log.2
    EXPECT_TRUE(fs::exists(log_path));
    EXPECT_TRUE(fs::exists(log_path + ".1"));
    EXPECT_TRUE(fs::exists(log_path + ".2"));
}

// ============================================================
// Dynamic log level adjustment
// Validates: Requirements 10.5
// ============================================================

TEST(LogManagerLevel, DefaultLevelIsINFO) {
    LogManager lm;
    EXPECT_EQ(lm.current_level(), LogLevel::INFO);
}

TEST(LogManagerLevel, SetLevelChangesCurrentLevel) {
    LogManager lm;
    lm.set_level(LogLevel::DEBUG);
    EXPECT_EQ(lm.current_level(), LogLevel::DEBUG);

    lm.set_level(LogLevel::CRITICAL);
    EXPECT_EQ(lm.current_level(), LogLevel::CRITICAL);

    lm.set_level(LogLevel::TRACE);
    EXPECT_EQ(lm.current_level(), LogLevel::TRACE);
}

TEST(LogManagerLevel, MessagesBelowLevelAreFiltered) {
    TempDir tmp;
    std::string log_path = tmp.file("level_test.log");

    LogManager lm;
    auto init_result = lm.initialize(log_path, LogLevel::WARNING, 10, 5, false);
    ASSERT_TRUE(init_result.is_ok());

    // These should be filtered (below WARNING)
    lm.log(LogLevel::TRACE, "Test", "trace msg");
    lm.log(LogLevel::DEBUG, "Test", "debug msg");
    lm.log(LogLevel::INFO, "Test", "info msg");

    // These should be written
    lm.log(LogLevel::WARNING, "Test", "warning msg");
    lm.log(LogLevel::ERROR, "Test", "error msg");
    lm.log(LogLevel::CRITICAL, "Test", "critical msg");

    std::string content = read_file(log_path);
    EXPECT_EQ(content.find("trace msg"), std::string::npos);
    EXPECT_EQ(content.find("debug msg"), std::string::npos);
    EXPECT_EQ(content.find("info msg"), std::string::npos);
    EXPECT_NE(content.find("warning msg"), std::string::npos);
    EXPECT_NE(content.find("error msg"), std::string::npos);
    EXPECT_NE(content.find("critical msg"), std::string::npos);
}

TEST(LogManagerLevel, DynamicLevelChangeFiltersImmediately) {
    TempDir tmp;
    std::string log_path = tmp.file("dynamic_level.log");

    LogManager lm;
    auto init_result = lm.initialize(log_path, LogLevel::TRACE, 10, 5, false);
    ASSERT_TRUE(init_result.is_ok());

    // Write at TRACE level — should appear
    lm.log(LogLevel::TRACE, "Test", "before_change");

    // Raise level to ERROR
    lm.set_level(LogLevel::ERROR);

    // INFO should now be filtered
    lm.log(LogLevel::INFO, "Test", "after_change_info");

    // ERROR should still appear
    lm.log(LogLevel::ERROR, "Test", "after_change_error");

    std::string content = read_file(log_path);
    EXPECT_NE(content.find("before_change"), std::string::npos);
    EXPECT_EQ(content.find("after_change_info"), std::string::npos);
    EXPECT_NE(content.find("after_change_error"), std::string::npos);
}

// ============================================================
// log_metrics() — performance metrics output
// Validates: Requirements 10.7
// ============================================================

TEST(LogManagerMetrics, MetricsOutputContainsAllFields) {
    TempDir tmp;
    std::string log_path = tmp.file("metrics.log");

    LogManager lm;
    auto init_result = lm.initialize(log_path, LogLevel::TRACE, 10, 5, false);
    ASSERT_TRUE(init_result.is_ok());

    PerformanceMetrics metrics;
    metrics.fps = 29.97;
    metrics.bitrate_kbps = 2048;
    metrics.latency_ms = 45.5;
    metrics.dropped_frames = 3;
    metrics.active_connections = 2;

    lm.log_metrics(metrics);

    std::string content = read_file(log_path);
    EXPECT_NE(content.find("fps="), std::string::npos);
    EXPECT_NE(content.find("bitrate_kbps=2048"), std::string::npos);
    EXPECT_NE(content.find("dropped_frames=3"), std::string::npos);
    EXPECT_NE(content.find("active_connections=2"), std::string::npos);
    EXPECT_NE(content.find("Metrics"), std::string::npos);
}

// ============================================================
// Initialize — error cases
// ============================================================

TEST(LogManagerInit, InvalidPathReturnsError) {
    LogManager lm;
    auto result = lm.initialize("/nonexistent/dir/app.log", LogLevel::INFO, 10, 5, false);
    EXPECT_TRUE(result.is_err());
}

TEST(LogManagerInit, ValidPathSucceeds) {
    TempDir tmp;
    LogManager lm;
    auto result = lm.initialize(tmp.file("ok.log"), LogLevel::INFO, 10, 5, false);
    EXPECT_TRUE(result.is_ok());
}

// ============================================================
// log_level_to_string / log_level_from_string
// Validates: Requirements 10.1
// ============================================================

TEST(LogLevel, ToStringAllLevels) {
    EXPECT_STREQ(log_level_to_string(LogLevel::TRACE), "TRACE");
    EXPECT_STREQ(log_level_to_string(LogLevel::DEBUG), "DEBUG");
    EXPECT_STREQ(log_level_to_string(LogLevel::INFO), "INFO");
    EXPECT_STREQ(log_level_to_string(LogLevel::WARNING), "WARNING");
    EXPECT_STREQ(log_level_to_string(LogLevel::ERROR), "ERROR");
    EXPECT_STREQ(log_level_to_string(LogLevel::CRITICAL), "CRITICAL");
}

TEST(LogLevel, FromStringAllLevels) {
    EXPECT_EQ(log_level_from_string("TRACE"), LogLevel::TRACE);
    EXPECT_EQ(log_level_from_string("DEBUG"), LogLevel::DEBUG);
    EXPECT_EQ(log_level_from_string("INFO"), LogLevel::INFO);
    EXPECT_EQ(log_level_from_string("WARNING"), LogLevel::WARNING);
    EXPECT_EQ(log_level_from_string("ERROR"), LogLevel::ERROR);
    EXPECT_EQ(log_level_from_string("CRITICAL"), LogLevel::CRITICAL);
}

TEST(LogLevel, FromStringUnknownDefaultsToINFO) {
    EXPECT_EQ(log_level_from_string("UNKNOWN"), LogLevel::INFO);
    EXPECT_EQ(log_level_from_string(""), LogLevel::INFO);
}
