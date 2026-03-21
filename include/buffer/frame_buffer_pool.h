#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "core/frame.h"
#include "core/types.h"

namespace sc {

// ============================================================
// FrameBuffer — single frame buffer with move semantics
// ============================================================

class FrameBuffer {
public:
    FrameBuffer() = default;
    FrameBuffer(std::vector<uint8_t> data, FrameInfo info);

    // Move semantics
    FrameBuffer(FrameBuffer&& other) noexcept;
    FrameBuffer& operator=(FrameBuffer&& other) noexcept;

    // No copy
    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;

    const uint8_t* data() const;
    size_t size() const;
    const FrameInfo& info() const;

    /// Reset buffer content without deallocating underlying storage
    void reset(std::vector<uint8_t> data, FrameInfo info);

    /// Check if buffer holds valid data
    bool empty() const;

private:
    std::vector<uint8_t> data_;
    FrameInfo info_{};
};

// ============================================================
// IFrameBufferPool — interface for frame buffer pool
// ============================================================

class IFrameBufferPool {
public:
    virtual ~IFrameBufferPool() = default;

    virtual VoidResult initialize(uint32_t pool_size,
                                  uint32_t frame_width,
                                  uint32_t frame_height) = 0;

    /// Acquire a free buffer for the producer to fill
    virtual Result<std::shared_ptr<FrameBuffer>> acquire() = 0;

    /// Submit a filled buffer for consumers to read
    virtual VoidResult submit(std::shared_ptr<FrameBuffer> frame) = 0;

    /// Consume the latest submitted frame
    virtual Result<std::shared_ptr<FrameBuffer>> consume_latest() = 0;

    virtual uint32_t available_count() const = 0;
    virtual uint32_t total_count() const = 0;
    virtual uint64_t dropped_count() const = 0;
};

// ============================================================
// FrameBufferPool — concrete implementation
// ============================================================

class FrameBufferPool : public IFrameBufferPool {
public:
    FrameBufferPool() = default;
    ~FrameBufferPool() override = default;

    FrameBufferPool(const FrameBufferPool&) = delete;
    FrameBufferPool& operator=(const FrameBufferPool&) = delete;
    FrameBufferPool(FrameBufferPool&&) = delete;
    FrameBufferPool& operator=(FrameBufferPool&&) = delete;

    VoidResult initialize(uint32_t pool_size,
                          uint32_t frame_width,
                          uint32_t frame_height) override;

    Result<std::shared_ptr<FrameBuffer>> acquire() override;
    VoidResult submit(std::shared_ptr<FrameBuffer> frame) override;
    Result<std::shared_ptr<FrameBuffer>> consume_latest() override;

    uint32_t available_count() const override;
    uint32_t total_count() const override;
    uint64_t dropped_count() const override;

private:
    /// Return a buffer to the free pool (called via shared_ptr custom deleter)
    void return_buffer(FrameBuffer* buf);

    mutable std::shared_mutex mutex_;

    // Pre-allocated raw buffers owned by the pool
    std::vector<std::unique_ptr<FrameBuffer>> all_buffers_;

    // Free buffers available for acquire()
    std::deque<FrameBuffer*> free_buffers_;

    // Submitted (unconsumed) frames in FIFO order
    std::deque<std::shared_ptr<FrameBuffer>> pending_frames_;

    uint32_t pool_size_ = 0;
    uint32_t frame_width_ = 0;
    uint32_t frame_height_ = 0;
    std::atomic<uint64_t> dropped_count_{0};
    bool initialized_ = false;
};

}  // namespace sc
