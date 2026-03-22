#include "buffer/frame_buffer_pool.h"

#include <algorithm>
#include <cstdio>

namespace sc {

// ============================================================
// FrameBuffer implementation
// ============================================================

FrameBuffer::FrameBuffer(std::vector<uint8_t> data, FrameInfo info)
    : data_(std::move(data)), info_(std::move(info)) {}

FrameBuffer::FrameBuffer(FrameBuffer&& other) noexcept
    : data_(std::move(other.data_)), info_(std::move(other.info_)) {}

FrameBuffer& FrameBuffer::operator=(FrameBuffer&& other) noexcept {
    if (this != &other) {
        data_ = std::move(other.data_);
        info_ = std::move(other.info_);
    }
    return *this;
}

const uint8_t* FrameBuffer::data() const {
    return data_.data();
}

size_t FrameBuffer::size() const {
    return data_.size();
}

const FrameInfo& FrameBuffer::info() const {
    return info_;
}

void FrameBuffer::reset(std::vector<uint8_t> data, FrameInfo info) {
    data_ = std::move(data);
    info_ = std::move(info);
}

bool FrameBuffer::empty() const {
    return data_.empty();
}

// ============================================================
// FrameBufferPool implementation
// ============================================================

VoidResult FrameBufferPool::initialize(uint32_t pool_size,
                                       uint32_t frame_width,
                                       uint32_t frame_height) {
    if (pool_size == 0) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Pool size must be greater than 0",
                       "FrameBufferPool::initialize");
    }
    if (frame_width == 0 || frame_height == 0) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Frame dimensions must be greater than 0",
                       "FrameBufferPool::initialize");
    }

    std::unique_lock lock(mutex_);

    if (initialized_) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Pool already initialized",
                       "FrameBufferPool::initialize");
    }

    pool_size_ = pool_size;
    frame_width_ = frame_width;
    frame_height_ = frame_height;

    // Pre-allocate all buffers
    all_buffers_.reserve(pool_size);
    for (uint32_t i = 0; i < pool_size; ++i) {
        auto buf = std::make_unique<FrameBuffer>();
        free_buffers_.push_back(buf.get());
        all_buffers_.push_back(std::move(buf));
    }

    initialized_ = true;
    return OkVoid();
}

Result<std::shared_ptr<FrameBuffer>> FrameBufferPool::acquire() {
    // Strategy: if pool is exhausted, remove the oldest pending frame from the
    // queue while holding the lock, then release the lock BEFORE destroying the
    // shared_ptr (whose custom deleter calls return_buffer → locks mutex_).
    // After the drop, re-lock and grab the now-free buffer.

    std::shared_ptr<FrameBuffer> to_drop;
    bool need_drop = false;

    {
        std::unique_lock lock(mutex_);

        if (!initialized_) {
            return Result<std::shared_ptr<FrameBuffer>>::Err(
                Error{ErrorCode::InvalidArgument,
                      "Pool not initialized",
                      "FrameBufferPool::acquire"});
        }

        // Fast path: free buffer available
        if (!free_buffers_.empty()) {
            FrameBuffer* raw = free_buffers_.front();
            free_buffers_.pop_front();
            auto ptr = std::shared_ptr<FrameBuffer>(raw, [this](FrameBuffer* buf) {
                this->return_buffer(buf);
            });
            return Result<std::shared_ptr<FrameBuffer>>::Ok(std::move(ptr));
        }

        // Slow path: no free buffers — try to reclaim from pending
        if (!pending_frames_.empty()) {
            to_drop = std::move(pending_frames_.front());
            pending_frames_.pop_front();
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            need_drop = true;

            std::fprintf(stderr,
                         "[WARNING] FrameBufferPool: all buffers occupied, "
                         "dropped oldest unconsumed frame (seq=%llu)\n",
                         static_cast<unsigned long long>(
                             to_drop->info().sequence_number));
        }
    }
    // Lock released — safe to destroy the dropped frame's shared_ptr.
    // The custom deleter will call return_buffer() which acquires mutex_.
    if (need_drop) {
        to_drop.reset();

        // Re-acquire lock and grab the freed buffer
        std::unique_lock lock(mutex_);
        if (!free_buffers_.empty()) {
            FrameBuffer* raw = free_buffers_.front();
            free_buffers_.pop_front();
            auto ptr = std::shared_ptr<FrameBuffer>(raw, [this](FrameBuffer* buf) {
                this->return_buffer(buf);
            });
            return Result<std::shared_ptr<FrameBuffer>>::Ok(std::move(ptr));
        }
        // Consumer still holds the dropped frame — buffer not yet returned
    }

    return Result<std::shared_ptr<FrameBuffer>>::Err(
        Error{ErrorCode::BufferPoolExhausted,
              "All frame buffers are in use by consumers",
              "FrameBufferPool::acquire"});
}

VoidResult FrameBufferPool::submit(std::shared_ptr<FrameBuffer> frame) {
    if (!frame) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Cannot submit null frame",
                       "FrameBufferPool::submit");
    }

    std::unique_lock lock(mutex_);

    if (!initialized_) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Pool not initialized",
                       "FrameBufferPool::submit");
    }

    pending_frames_.push_back(std::move(frame));
    return OkVoid();
}

Result<std::shared_ptr<FrameBuffer>> FrameBufferPool::consume_latest() {
    // Collect frames to drop — destroy them outside the lock
    std::vector<std::shared_ptr<FrameBuffer>> to_drop;
    std::shared_ptr<FrameBuffer> latest;

    {
        std::unique_lock lock(mutex_);

        if (!initialized_) {
            return Result<std::shared_ptr<FrameBuffer>>::Err(
                Error{ErrorCode::InvalidArgument,
                      "Pool not initialized",
                      "FrameBufferPool::consume_latest"});
        }

        if (pending_frames_.empty()) {
            return Result<std::shared_ptr<FrameBuffer>>::Err(
                Error{ErrorCode::NotFound,
                      "No pending frames available",
                      "FrameBufferPool::consume_latest"});
        }

        // Keep the latest (newest) frame
        latest = std::move(pending_frames_.back());
        pending_frames_.pop_back();

        // Move older frames to drop list
        if (!pending_frames_.empty()) {
            dropped_count_.fetch_add(
                static_cast<uint64_t>(pending_frames_.size()),
                std::memory_order_relaxed);
            to_drop.reserve(pending_frames_.size());
            for (auto& f : pending_frames_) {
                to_drop.push_back(std::move(f));
            }
            pending_frames_.clear();
        }
    }
    // Lock released — safe to destroy dropped frames (deleters can lock)
    to_drop.clear();

    return Result<std::shared_ptr<FrameBuffer>>::Ok(std::move(latest));
}

uint32_t FrameBufferPool::available_count() const {
    std::shared_lock lock(mutex_);
    return static_cast<uint32_t>(free_buffers_.size());
}

uint32_t FrameBufferPool::total_count() const {
    std::shared_lock lock(mutex_);
    return pool_size_;
}

uint64_t FrameBufferPool::dropped_count() const {
    return dropped_count_.load(std::memory_order_relaxed);
}

void FrameBufferPool::return_buffer(FrameBuffer* buf) {
    std::unique_lock lock(mutex_);
    free_buffers_.push_back(buf);
}

}  // namespace sc
