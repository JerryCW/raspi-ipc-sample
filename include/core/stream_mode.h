#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

// ============================================================
// StreamMode — operating mode based on endpoint availability
// ============================================================

enum class StreamMode : uint8_t {
    FULL = 0,          // KVS + WebRTC both active
    KVS_ONLY = 1,      // Only KVS upload
    WEBRTC_ONLY = 2,   // Only WebRTC live streaming
    DEGRADED = 3,      // Local cache only
};

inline const char* stream_mode_to_string(StreamMode mode) {
    switch (mode) {
        case StreamMode::FULL:         return "FULL";
        case StreamMode::KVS_ONLY:     return "KVS_ONLY";
        case StreamMode::WEBRTC_ONLY:  return "WEBRTC_ONLY";
        case StreamMode::DEGRADED:     return "DEGRADED";
    }
    return "UNKNOWN";
}

// ============================================================
// VideoPreset — resolution / fps / bitrate bundle
// ============================================================

struct VideoPreset {
    std::string name;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate_kbps;
};

// Built-in presets
inline const VideoPreset PRESET_DEFAULT     {"Default",       1280, 720,  15, 2048};
inline const VideoPreset PRESET_HD          {"HD",            1920, 1080, 30, 4096};
inline const VideoPreset PRESET_LOW_BW      {"Low_Bandwidth", 640,  480,  15, 512};

// Helper: get all built-in presets
inline std::vector<VideoPreset> builtin_presets() {
    return {PRESET_DEFAULT, PRESET_HD, PRESET_LOW_BW};
}

}  // namespace sc
