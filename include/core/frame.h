#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace sc {

// ============================================================
// FrameInfo — metadata for a single video frame
// ============================================================

struct FrameInfo {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    std::chrono::steady_clock::time_point timestamp;
    uint64_t sequence_number;
};

// ============================================================
// VideoFrame — raw pixel data + metadata
// ============================================================

struct VideoFrame {
    std::vector<uint8_t> data;
    FrameInfo info;
};

// ============================================================
// CameraCapabilities — what a camera source supports
// ============================================================

struct CameraCapabilities {
    struct Resolution {
        uint32_t width;
        uint32_t height;
    };

    std::vector<Resolution> supported_resolutions;
    uint32_t min_fps;
    uint32_t max_fps;
    std::string pixel_format;  // "NV12", "YUY2", etc.
};

}  // namespace sc
