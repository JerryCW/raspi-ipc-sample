#include "core/resource_guard.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

namespace sc {
namespace {

// ============================================================
// CircularBuffer tests
// ============================================================

TEST(CircularBufferTest, WriteAndReadBasic) {
    CircularBuffer buf(64);
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    buf.write(data.data(), data.size());

    EXPECT_EQ(buf.available(), 5u);
    EXPECT_EQ(buf.capacity(), 64u);
    EXPECT_EQ(buf.total_written(), 5u);
    EXPECT_EQ(buf.overwrite_count(), 0u);

    auto result = buf.read(5);
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result, data);
    EXPECT_EQ(buf.available(), 0u);
}

TEST(CircularBufferTest, WriteWrapsAround) {
    CircularBuffer buf(8);

    // Fill buffer completely
    std::vector<uint8_t> data1 = {1, 2, 3, 4, 5, 6, 7, 8};
    buf.write(data1.data(), data1.size());
    EXPECT_EQ(buf.available(), 8u);

    // Read 4 bytes to free space
    auto r1 = buf.read(4);
    EXPECT_EQ(r1.size(), 4u);
    EXPECT_EQ(buf.available(), 4u);

    // Write 4 more bytes — wraps around
    std::vector<uint8_t> data2 = {9, 10, 11, 12};
    buf.write(data2.data(), data2.size());
    EXPECT_EQ(buf.available(), 8u);

    // Read all — should get {5,6,7,8,9,10,11,12}
    auto r2 = buf.read(8);
    ASSERT_EQ(r2.size(), 8u);
    std::vector<uint8_t> expected = {5, 6, 7, 8, 9, 10, 11, 12};
    EXPECT_EQ(r2, expected);
}

TEST(CircularBufferTest, OverwriteOldestData) {
    CircularBuffer buf(8);

    // Fill buffer
    std::vector<uint8_t> data1 = {1, 2, 3, 4, 5, 6, 7, 8};
    buf.write(data1.data(), data1.size());
    EXPECT_EQ(buf.overwrite_count(), 0u);

    // Write 3 more bytes — should overwrite oldest 3 bytes
    std::vector<uint8_t> data2 = {9, 10, 11};
    buf.write(data2.data(), data2.size());
    EXPECT_EQ(buf.overwrite_count(), 1u);
    EXPECT_EQ(buf.available(), 8u);

    // Read all — should get {4,5,6,7,8,9,10,11}
    auto result = buf.read(8);
    ASSERT_EQ(result.size(), 8u);
    std::vector<uint8_t> expected = {4, 5, 6, 7, 8, 9, 10, 11};
    EXPECT_EQ(result, expected);
}

TEST(CircularBufferTest, MultipleOverwrites) {
    CircularBuffer buf(4);

    // Write 4 bytes
    std::vector<uint8_t> d1 = {1, 2, 3, 4};
    buf.write(d1.data(), d1.size());

    // Overwrite twice
    std::vector<uint8_t> d2 = {5, 6};
    buf.write(d2.data(), d2.size());
    EXPECT_EQ(buf.overwrite_count(), 1u);

    std::vector<uint8_t> d3 = {7, 8, 9};
    buf.write(d3.data(), d3.size());
    EXPECT_EQ(buf.overwrite_count(), 2u);

    // Should contain last 4 bytes: {6, 7, 8, 9}
    auto result = buf.read(4);
    ASSERT_EQ(result.size(), 4u);
    std::vector<uint8_t> expected = {6, 7, 8, 9};
    EXPECT_EQ(result, expected);
}

TEST(CircularBufferTest, WriteLargerThanCapacity) {
    CircularBuffer buf(4);

    // Write 6 bytes into a 4-byte buffer — only last 4 kept
    std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6};
    buf.write(data.data(), data.size());

    EXPECT_EQ(buf.available(), 4u);
    auto result = buf.read(4);
    std::vector<uint8_t> expected = {3, 4, 5, 6};
    EXPECT_EQ(result, expected);
}

TEST(CircularBufferTest, ReadMoreThanAvailable) {
    CircularBuffer buf(16);
    std::vector<uint8_t> data = {10, 20, 30};
    buf.write(data.data(), data.size());

    auto result = buf.read(100);
    EXPECT_EQ(result.size(), 3u);
    EXPECT_EQ(result, data);
}

TEST(CircularBufferTest, EmptyReadReturnsEmpty) {
    CircularBuffer buf(16);
    auto result = buf.read(10);
    EXPECT_TRUE(result.empty());
}

TEST(CircularBufferTest, ZeroSizeWriteIsNoop) {
    CircularBuffer buf(16);
    uint8_t dummy = 0;
    EXPECT_EQ(buf.write(&dummy, 0), 0u);
    EXPECT_EQ(buf.available(), 0u);
}

TEST(CircularBufferTest, Reset) {
    CircularBuffer buf(16);
    std::vector<uint8_t> data = {1, 2, 3};
    buf.write(data.data(), data.size());
    EXPECT_EQ(buf.available(), 3u);

    buf.reset();
    EXPECT_EQ(buf.available(), 0u);
    EXPECT_EQ(buf.total_written(), 0u);
    EXPECT_EQ(buf.overwrite_count(), 0u);
}

TEST(CircularBufferTest, TotalWrittenTracksAllBytes) {
    CircularBuffer buf(4);
    std::vector<uint8_t> d1 = {1, 2, 3, 4};
    buf.write(d1.data(), d1.size());
    std::vector<uint8_t> d2 = {5, 6};
    buf.write(d2.data(), d2.size());

    EXPECT_EQ(buf.total_written(), 6u);
}

// ============================================================
// ResourceGuard tests
// ============================================================

TEST(ResourceGuardTest, InitializeCreatesBuffer) {
    ResourceGuardConfig cfg;
    cfg.cache_buffer_bytes = 1024;
    ResourceGuard guard(cfg);

    auto result = guard.initialize();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(guard.cache_capacity(), 1024u);
    EXPECT_EQ(guard.cache_available(), 0u);
}

TEST(ResourceGuardTest, CacheWriteAndRead) {
    ResourceGuardConfig cfg;
    cfg.cache_buffer_bytes = 64;
    ResourceGuard guard(cfg);
    guard.initialize();

    std::vector<uint8_t> data = {10, 20, 30, 40};
    guard.cache_write(data.data(), data.size());
    EXPECT_EQ(guard.cache_available(), 4u);

    auto result = guard.cache_read(4);
    EXPECT_EQ(result, data);
}

TEST(ResourceGuardTest, CacheOverwriteTracked) {
    ResourceGuardConfig cfg;
    cfg.cache_buffer_bytes = 8;
    ResourceGuard guard(cfg);
    guard.initialize();

    std::vector<uint8_t> d1 = {1, 2, 3, 4, 5, 6, 7, 8};
    guard.cache_write(d1.data(), d1.size());
    EXPECT_EQ(guard.cache_overwrite_count(), 0u);

    std::vector<uint8_t> d2 = {9, 10};
    guard.cache_write(d2.data(), d2.size());
    EXPECT_EQ(guard.cache_overwrite_count(), 1u);
}

TEST(ResourceGuardTest, MemoryCheckOkWhenUnderLimit) {
    ResourceGuardConfig cfg;
    cfg.memory_limit_bytes = 1024;
    cfg.memory_warning_ratio = 0.9;
    ResourceGuard guard(cfg);
    guard.initialize();

    guard.set_memory_usage(500);
    auto result = guard.check_memory();
    EXPECT_TRUE(result.is_ok());
}

TEST(ResourceGuardTest, MemoryCheckTriggersCleanupAt90Percent) {
    ResourceGuardConfig cfg;
    cfg.memory_limit_bytes = 1000;
    cfg.memory_warning_ratio = 0.9;
    ResourceGuard guard(cfg);
    guard.initialize();

    bool cleanup_called = false;
    guard.on_memory_cleanup([&cleanup_called]() {
        cleanup_called = true;
    });

    // Set memory to 91% of limit (910 > 900 threshold)
    guard.set_memory_usage(910);
    auto result = guard.check_memory();
    EXPECT_TRUE(result.is_ok());  // still under hard limit
    EXPECT_TRUE(cleanup_called);
}

TEST(ResourceGuardTest, MemoryCheckTriggersRestartOverLimit) {
    ResourceGuardConfig cfg;
    cfg.memory_limit_bytes = 1000;
    ResourceGuard guard(cfg);
    guard.initialize();

    bool restart_called = false;
    std::string restart_reason;
    guard.on_restart_needed([&](const std::string& reason) {
        restart_called = true;
        restart_reason = reason;
    });

    guard.set_memory_usage(1100);
    auto result = guard.check_memory();
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::MemoryLimitExceeded);
    EXPECT_TRUE(restart_called);
}

TEST(ResourceGuardTest, MemoryCleanupThenStillOverLimitTriggersRestart) {
    ResourceGuardConfig cfg;
    cfg.memory_limit_bytes = 1000;
    cfg.memory_warning_ratio = 0.9;
    ResourceGuard guard(cfg);
    guard.initialize();

    bool cleanup_called = false;
    bool restart_called = false;

    // Cleanup callback sets memory to still over limit
    guard.on_memory_cleanup([&]() {
        cleanup_called = true;
        guard.set_memory_usage(1050);  // still over limit after cleanup
    });

    guard.on_restart_needed([&](const std::string&) {
        restart_called = true;
    });

    // Set memory at 95% (triggers cleanup), cleanup sets it to 105% (over limit)
    guard.set_memory_usage(950);
    auto result = guard.check_memory();
    EXPECT_TRUE(cleanup_called);
    EXPECT_TRUE(restart_called);
    EXPECT_TRUE(result.is_err());
}

TEST(ResourceGuardTest, CpuQuotaFromConfig) {
    ResourceGuardConfig cfg;
    cfg.cpu_quota_percent = 75;
    ResourceGuard guard(cfg);
    EXPECT_EQ(guard.cpu_quota_percent(), 75u);
}

TEST(ResourceGuardTest, DefaultConfig) {
    ResourceGuard guard;
    const auto& cfg = guard.config();
    EXPECT_EQ(cfg.cache_buffer_bytes, 200ULL * 1024 * 1024);
    EXPECT_EQ(cfg.memory_limit_bytes, 512ULL * 1024 * 1024);
    EXPECT_DOUBLE_EQ(cfg.memory_warning_ratio, 0.9);
    EXPECT_EQ(cfg.cpu_quota_percent, 80u);
}

TEST(ResourceGuardTest, CacheBeforeInitializeReturnsZero) {
    ResourceGuard guard;
    uint8_t data = 42;
    EXPECT_EQ(guard.cache_write(&data, 1), 0u);
    EXPECT_EQ(guard.cache_available(), 0u);
    EXPECT_TRUE(guard.cache_read(1).empty());
}

}  // namespace
}  // namespace sc
