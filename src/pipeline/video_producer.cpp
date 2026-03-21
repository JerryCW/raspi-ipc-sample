#include "pipeline/video_producer.h"

#include <cstring>

namespace sc {

// ============================================================
// NAL start code patterns
// ============================================================

static constexpr uint8_t kStartCode3[] = {0x00, 0x00, 0x01};
static constexpr uint8_t kStartCode4[] = {0x00, 0x00, 0x00, 0x01};

// Minimum NAL unit size: start code (3 bytes) + NAL header (1 byte)
static constexpr size_t kMinNalSize = 4;

// ============================================================
// Helper: find next NAL start code position
// Returns offset of start code, or size if not found.
// ============================================================

static size_t find_start_code(const uint8_t* data, size_t size, size_t offset) {
    for (size_t i = offset; i + 2 < size; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            // 4-byte start code: 00 00 00 01
            if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                return i;
            }
            // 3-byte start code: 00 00 01
            if (data[i + 2] == 0x01) {
                return i;
            }
        }
    }
    return size;
}

// ============================================================
// Helper: get start code length at position
// ============================================================

static size_t start_code_length(const uint8_t* data, size_t size, size_t offset) {
    if (offset + 3 < size &&
        data[offset] == 0x00 && data[offset + 1] == 0x00 &&
        data[offset + 2] == 0x00 && data[offset + 3] == 0x01) {
        return 4;
    }
    if (offset + 2 < size &&
        data[offset] == 0x00 && data[offset + 1] == 0x00 &&
        data[offset + 2] == 0x01) {
        return 3;
    }
    return 0;
}

// ============================================================
// Helper: extract NAL type from header byte
// ============================================================

static uint8_t nal_type_from_header(uint8_t header_byte) {
    return header_byte & 0x1F;
}

// ============================================================
// VideoProducer
// ============================================================

VideoProducer::VideoProducer() {
    stats_.window_start = std::chrono::steady_clock::now();
}

Result<EncodedFrame> VideoProducer::process_frame(const EncodedFrame& frame) {
    // Reject empty frames
    if (frame.data.empty()) {
        record_drop(DropReason::NAL_INVALID);
        return Result<EncodedFrame>::Err(
            Error{ErrorCode::NALUnitInvalid, "Empty frame data", "VideoProducer"});
    }

    // Check ICE negotiating state — drop frames to avoid accumulation
    if (ice_negotiating_.load(std::memory_order_relaxed)) {
        record_drop(DropReason::ICE_NEGOTIATING);
        return Result<EncodedFrame>::Err(
            Error{ErrorCode::NALUnitInvalid,
                  "Frame dropped: ICE negotiation in progress",
                  "VideoProducer"});
    }

    // Validate NAL unit integrity
    if (!validate_nal_units(frame.data.data(), frame.data.size())) {
        record_drop(DropReason::NAL_INVALID);
        return Result<EncodedFrame>::Err(
            Error{ErrorCode::NALUnitInvalid,
                  "NAL unit integrity check failed",
                  "VideoProducer"});
    }

    // If force_idr is set, only allow IDR frames through
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (force_idr_) {
            if (!is_idr_frame(frame.data.data(), frame.data.size())) {
                // Drop non-IDR frame when IDR is required
                // Don't count this as a "drop" in stats — it's a flow control decision
                // but we still need to keep force_idr_ set
                stats_.total_dropped++;
                return Result<EncodedFrame>::Err(
                    Error{ErrorCode::NALUnitInvalid,
                          "Non-IDR frame dropped: waiting for IDR after previous drop",
                          "VideoProducer"});
            }
            // Got the IDR frame we were waiting for — clear the flag
            force_idr_ = false;
        }
    }

    // Frame is valid — pass through
    return Result<EncodedFrame>::Ok(EncodedFrame{frame.data, frame.sequence_number, frame.timestamp});
}

void VideoProducer::set_ice_negotiating(bool negotiating) {
    ice_negotiating_.store(negotiating, std::memory_order_relaxed);
}

bool VideoProducer::is_ice_negotiating() const {
    return ice_negotiating_.load(std::memory_order_relaxed);
}

bool VideoProducer::force_idr_required() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return force_idr_;
}

DropStats VideoProducer::drop_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void VideoProducer::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = DropStats{};
    stats_.window_start = std::chrono::steady_clock::now();
}

void VideoProducer::record_drop(DropReason reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.total_dropped++;
    switch (reason) {
        case DropReason::NAL_INVALID:
            stats_.nal_invalid++;
            break;
        case DropReason::ICE_NEGOTIATING:
            stats_.ice_negotiating++;
            break;
    }
    // After any drop, force next sent frame to be IDR
    force_idr_ = true;
}

// ============================================================
// Static: validate all NAL units in a frame
// ============================================================

bool VideoProducer::validate_nal_units(const uint8_t* data, size_t size) {
    if (data == nullptr || size < kMinNalSize) {
        return false;
    }

    // Frame must start with a valid start code
    size_t sc_len = start_code_length(data, size, 0);
    if (sc_len == 0) {
        return false;
    }

    // Walk through all NAL units
    size_t pos = 0;
    bool found_nal = false;

    while (pos < size) {
        sc_len = start_code_length(data, size, pos);
        if (sc_len == 0) {
            // No start code at current position — invalid
            return false;
        }

        size_t header_pos = pos + sc_len;
        if (header_pos >= size) {
            // Start code at end with no NAL header — truncated
            return false;
        }

        // Validate NAL header: forbidden_zero_bit must be 0
        uint8_t header = data[header_pos];
        if ((header & 0x80) != 0) {
            return false;  // forbidden_zero_bit is set
        }

        // Check NAL type is in valid range (1-23 for single NAL, 24-31 for aggregation)
        uint8_t nal_type = nal_type_from_header(header);
        if (nal_type == 0) {
            return false;  // Unspecified NAL type
        }

        found_nal = true;

        // Find next NAL unit
        size_t next = find_start_code(data, size, header_pos + 1);
        pos = next;
    }

    return found_nal;
}

// ============================================================
// Static: check if frame contains an IDR NAL unit
// ============================================================

bool VideoProducer::is_idr_frame(const uint8_t* data, size_t size) {
    if (data == nullptr || size < kMinNalSize) {
        return false;
    }

    size_t pos = 0;
    while (pos < size) {
        size_t sc_len = start_code_length(data, size, pos);
        if (sc_len == 0) {
            break;
        }

        size_t header_pos = pos + sc_len;
        if (header_pos >= size) {
            break;
        }

        uint8_t nal_type = nal_type_from_header(data[header_pos]);
        if (nal_type == static_cast<uint8_t>(NalType::IDR)) {
            return true;
        }

        // Find next NAL unit
        size_t next = find_start_code(data, size, header_pos + 1);
        pos = next;
    }

    return false;
}

}  // namespace sc
