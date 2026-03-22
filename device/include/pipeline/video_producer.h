#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/types.h"

namespace sc {

// ============================================================
// H.264 NAL unit type constants
// ============================================================

enum class NalType : uint8_t {
    NON_IDR = 1,
    IDR = 5,
    SEI = 6,
    SPS = 7,
    PPS = 8,
};

// ============================================================
// DropReason — why a frame was dropped
// ============================================================

enum class DropReason : uint8_t {
    NAL_INVALID = 0,
    ICE_NEGOTIATING = 1,
};

// ============================================================
// DropStats — frame drop statistics
// ============================================================

struct DropStats {
    uint64_t total_dropped = 0;
    uint64_t nal_invalid = 0;
    uint64_t ice_negotiating = 0;
    std::chrono::steady_clock::time_point window_start;
};

// ============================================================
// EncodedFrame — an H.264 encoded frame for processing
// ============================================================

struct EncodedFrame {
    std::vector<uint8_t> data;
    uint64_t sequence_number = 0;
    std::chrono::steady_clock::time_point timestamp;
};

// ============================================================
// VideoProducer — frame-level resilience for encoded output
// ============================================================

class VideoProducer {
public:
    VideoProducer();
    ~VideoProducer() = default;

    // Process an encoded frame. Returns the frame if valid, or an error
    // if the frame was dropped.
    Result<EncodedFrame> process_frame(const EncodedFrame& frame);

    // Set ICE negotiating state (called by WebRTC_Agent)
    void set_ice_negotiating(bool negotiating);
    bool is_ice_negotiating() const;

    // Query whether the next frame must be an IDR
    bool force_idr_required() const;

    // Get current drop statistics
    DropStats drop_stats() const;

    // Reset drop statistics and start a new window
    void reset_stats();

    // Static NAL validation utilities
    static bool validate_nal_units(const uint8_t* data, size_t size);
    static bool is_idr_frame(const uint8_t* data, size_t size);

private:
    void record_drop(DropReason reason);

    mutable std::mutex mutex_;
    std::atomic<bool> ice_negotiating_{false};
    bool force_idr_ = false;
    DropStats stats_;
};

}  // namespace sc
