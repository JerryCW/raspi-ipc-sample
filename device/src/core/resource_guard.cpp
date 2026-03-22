#include "core/resource_guard.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace sc {

// ============================================================
// CircularBuffer
// ============================================================

CircularBuffer::CircularBuffer(size_t capacity_bytes)
    : buffer_(capacity_bytes), capacity_(capacity_bytes) {}

size_t CircularBuffer::write(const uint8_t* data, size_t size) {
    if (size == 0 || capacity_ == 0) return 0;

    // If writing more than capacity, only keep the last capacity_ bytes
    if (size > capacity_) {
        data += (size - capacity_);
        size = capacity_;
    }

    // Check if we need to overwrite oldest data
    size_t free_space = capacity_ - used_;
    if (size > free_space) {
        size_t overwrite_bytes = size - free_space;
        // Advance tail past the overwritten data
        tail_ = (tail_ + overwrite_bytes) % capacity_;
        used_ -= overwrite_bytes;
        ++overwrite_count_;

        std::cerr << "[ResourceGuard] INFO: Circular buffer full, "
                  << "overwriting " << overwrite_bytes << " bytes of oldest data. "
                  << "total_written=" << (total_written_ + size)
                  << " overwrite_count=" << overwrite_count_ << std::endl;
    }

    // Write data in up to two segments (wrap-around)
    size_t first_chunk = std::min(size, capacity_ - head_);
    std::memcpy(buffer_.data() + head_, data, first_chunk);
    if (first_chunk < size) {
        std::memcpy(buffer_.data(), data + first_chunk, size - first_chunk);
    }

    head_ = (head_ + size) % capacity_;
    used_ += size;
    total_written_ += size;

    return size;
}

std::vector<uint8_t> CircularBuffer::read(size_t size) {
    if (size == 0 || used_ == 0) return {};

    size_t to_read = std::min(size, used_);
    std::vector<uint8_t> result(to_read);

    // Read in up to two segments (wrap-around)
    size_t first_chunk = std::min(to_read, capacity_ - tail_);
    std::memcpy(result.data(), buffer_.data() + tail_, first_chunk);
    if (first_chunk < to_read) {
        std::memcpy(result.data() + first_chunk, buffer_.data(), to_read - first_chunk);
    }

    tail_ = (tail_ + to_read) % capacity_;
    used_ -= to_read;

    return result;
}

size_t CircularBuffer::available() const {
    return used_;
}

size_t CircularBuffer::capacity() const {
    return capacity_;
}

uint64_t CircularBuffer::total_written() const {
    return total_written_;
}

uint64_t CircularBuffer::overwrite_count() const {
    return overwrite_count_;
}

void CircularBuffer::reset() {
    head_ = 0;
    tail_ = 0;
    used_ = 0;
    total_written_ = 0;
    overwrite_count_ = 0;
}

// ============================================================
// ResourceGuard
// ============================================================

ResourceGuard::ResourceGuard(ResourceGuardConfig config)
    : config_(std::move(config)) {}

VoidResult ResourceGuard::initialize() {
    std::unique_lock lock(buffer_mutex_);
    buffer_ = std::make_unique<CircularBuffer>(config_.cache_buffer_bytes);
    return OkVoid();
}

size_t ResourceGuard::cache_write(const uint8_t* data, size_t size) {
    std::unique_lock lock(buffer_mutex_);
    if (!buffer_) return 0;
    return buffer_->write(data, size);
}

std::vector<uint8_t> ResourceGuard::cache_read(size_t size) {
    std::unique_lock lock(buffer_mutex_);
    if (!buffer_) return {};
    return buffer_->read(size);
}

size_t ResourceGuard::cache_available() const {
    std::shared_lock lock(buffer_mutex_);
    if (!buffer_) return 0;
    return buffer_->available();
}

size_t ResourceGuard::cache_capacity() const {
    std::shared_lock lock(buffer_mutex_);
    if (!buffer_) return 0;
    return buffer_->capacity();
}

uint64_t ResourceGuard::cache_overwrite_count() const {
    std::shared_lock lock(buffer_mutex_);
    if (!buffer_) return 0;
    return buffer_->overwrite_count();
}

void ResourceGuard::set_memory_usage(size_t bytes) {
    memory_usage_.store(bytes);
}

size_t ResourceGuard::memory_usage() const {
    return memory_usage_.load();
}

VoidResult ResourceGuard::check_memory() {
    size_t current = memory_usage_.load();
    size_t limit = config_.memory_limit_bytes;
    size_t warning_threshold = static_cast<size_t>(
        static_cast<double>(limit) * config_.memory_warning_ratio);

    // Over hard limit → trigger restart
    if (current > limit) {
        std::cerr << "[ResourceGuard] CRITICAL: Memory limit exceeded. "
                  << "usage=" << (current / (1024 * 1024)) << "MB"
                  << " limit=" << (limit / (1024 * 1024)) << "MB" << std::endl;

        {
            std::lock_guard lock(callback_mutex_);
            if (restart_callback_) {
                restart_callback_("Memory limit exceeded");
            }
        }

        return ErrVoid(ErrorCode::MemoryLimitExceeded,
                       "Memory usage exceeds configured limit",
                       "ResourceGuard::check_memory");
    }

    // Over 90% threshold → trigger cleanup
    if (current > warning_threshold) {
        std::cerr << "[ResourceGuard] WARNING: Memory at "
                  << static_cast<int>(config_.memory_warning_ratio * 100)
                  << "% of limit. "
                  << "usage=" << (current / (1024 * 1024)) << "MB"
                  << " limit=" << (limit / (1024 * 1024)) << "MB" << std::endl;

        {
            std::lock_guard lock(callback_mutex_);
            if (cleanup_callback_) {
                cleanup_callback_();
            }
        }

        // Re-check after cleanup
        size_t after_cleanup = memory_usage_.load();
        if (after_cleanup > limit) {
            std::cerr << "[ResourceGuard] CRITICAL: Memory still over limit after cleanup. "
                      << "usage=" << (after_cleanup / (1024 * 1024)) << "MB"
                      << " limit=" << (limit / (1024 * 1024)) << "MB" << std::endl;

            {
                std::lock_guard lock(callback_mutex_);
                if (restart_callback_) {
                    restart_callback_("Memory still over limit after cleanup");
                }
            }

            return ErrVoid(ErrorCode::MemoryLimitExceeded,
                           "Memory still exceeds limit after cleanup",
                           "ResourceGuard::check_memory");
        }
    }

    return OkVoid();
}

void ResourceGuard::on_memory_cleanup(CleanupCallback cb) {
    std::lock_guard lock(callback_mutex_);
    cleanup_callback_ = std::move(cb);
}

void ResourceGuard::on_restart_needed(RestartCallback cb) {
    std::lock_guard lock(callback_mutex_);
    restart_callback_ = std::move(cb);
}

uint32_t ResourceGuard::cpu_quota_percent() const {
    return config_.cpu_quota_percent;
}

const ResourceGuardConfig& ResourceGuard::config() const {
    return config_;
}

}  // namespace sc
