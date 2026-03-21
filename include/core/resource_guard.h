#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "core/types.h"

namespace sc {

// ============================================================
// CircularBuffer — fixed-size byte ring buffer for local video cache
// ============================================================

class CircularBuffer {
public:
    /// Construct with capacity in bytes (default 200 MB)
    explicit CircularBuffer(size_t capacity_bytes = 200ULL * 1024 * 1024);

    CircularBuffer(const CircularBuffer&) = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;
    CircularBuffer(CircularBuffer&&) = delete;
    CircularBuffer& operator=(CircularBuffer&&) = delete;

    /// Write data into the buffer. Overwrites oldest data when full.
    /// Returns the number of bytes written (always == size).
    size_t write(const uint8_t* data, size_t size);

    /// Read up to `size` bytes from the buffer (oldest first).
    /// Returns the actual bytes read in a vector.
    std::vector<uint8_t> read(size_t size);

    /// Number of bytes available to read
    size_t available() const;

    /// Total buffer capacity
    size_t capacity() const;

    /// Total bytes ever written (including overwrites)
    uint64_t total_written() const;

    /// Number of times oldest data was overwritten
    uint64_t overwrite_count() const;

    /// Reset buffer to empty state
    void reset();

private:
    std::vector<uint8_t> buffer_;
    size_t capacity_;
    size_t head_ = 0;   // next write position
    size_t tail_ = 0;   // next read position
    size_t used_ = 0;   // bytes currently stored
    uint64_t total_written_ = 0;
    uint64_t overwrite_count_ = 0;
};

// ============================================================
// ResourceGuardConfig — tuneable parameters
// ============================================================

struct ResourceGuardConfig {
    size_t cache_buffer_bytes = 200ULL * 1024 * 1024;  // 200 MB circular buffer
    size_t memory_limit_bytes = 512ULL * 1024 * 1024;  // 512 MB process memory limit
    double memory_warning_ratio = 0.9;                  // 90% triggers cleanup
    uint32_t cpu_quota_percent = 80;                    // 80% CPU limit
};

// ============================================================
// ResourceGuard — resource protection (circular buffer + memory/CPU monitoring)
// ============================================================

class ResourceGuard {
public:
    explicit ResourceGuard(ResourceGuardConfig config = {});
    ~ResourceGuard() = default;

    ResourceGuard(const ResourceGuard&) = delete;
    ResourceGuard& operator=(const ResourceGuard&) = delete;

    /// Initialize the resource guard (allocates circular buffer)
    VoidResult initialize();

    /// Write video data to the circular cache buffer (thread-safe)
    size_t cache_write(const uint8_t* data, size_t size);

    /// Read cached video data (thread-safe)
    std::vector<uint8_t> cache_read(size_t size);

    /// Available bytes in cache
    size_t cache_available() const;

    /// Cache capacity
    size_t cache_capacity() const;

    /// Cache overwrite count
    uint64_t cache_overwrite_count() const;

    /// Set current memory usage for monitoring (called externally)
    void set_memory_usage(size_t bytes);

    /// Get current memory usage
    size_t memory_usage() const;

    /// Check memory status. Returns:
    ///   - OkVoid() if within limits
    ///   - Err with MemoryLimitExceeded if over hard limit
    ///   Triggers cleanup callback at 90% threshold.
    VoidResult check_memory();

    /// Register a callback for memory cleanup (triggered at 90% threshold)
    using CleanupCallback = std::function<void()>;
    void on_memory_cleanup(CleanupCallback cb);

    /// Register a callback for graceful restart (triggered when cleanup insufficient)
    using RestartCallback = std::function<void(const std::string& reason)>;
    void on_restart_needed(RestartCallback cb);

    /// Get the CPU quota percent from config
    uint32_t cpu_quota_percent() const;

    /// Get the config
    const ResourceGuardConfig& config() const;

private:
    ResourceGuardConfig config_;

    mutable std::shared_mutex buffer_mutex_;
    std::unique_ptr<CircularBuffer> buffer_;

    std::atomic<size_t> memory_usage_{0};

    std::mutex callback_mutex_;
    CleanupCallback cleanup_callback_;
    RestartCallback restart_callback_;
};

}  // namespace sc
