#include "camera/camera_source.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace sc {

// ============================================================
// SMPTE color bar definitions (NV12 Y/U/V values)
// 7 bars: white, yellow, cyan, green, magenta, red, blue
// ============================================================

struct YUVColor {
    uint8_t y;
    uint8_t u;
    uint8_t v;
};

static constexpr YUVColor SMPTE_BARS[] = {
    {235, 128, 128},  // white
    {210, 16,  146},  // yellow
    {170, 166, 16},   // cyan
    {145, 54,  34},   // green
    {106, 202, 222},  // magenta
    {81,  90,  240},  // red
    {41,  240, 110},  // blue
};

static constexpr size_t NUM_BARS = sizeof(SMPTE_BARS) / sizeof(SMPTE_BARS[0]);

// ============================================================
// VideoTestSrcSource — generates SMPTE test pattern in software
// ============================================================

class VideoTestSrcSource : public ICameraSource {
public:
    VideoTestSrcSource() = default;

    VoidResult open(const VideoPreset& preset) override;
    VoidResult close() override;
    Result<VideoFrame> capture_frame() override;
    Result<CameraCapabilities> query_capabilities() override;
    CameraSourceType type() const override { return CameraSourceType::VIDEOTESTSRC; }
    std::string gst_source_description() const override;

private:
    void generate_smpte_nv12(std::vector<uint8_t>& buf, uint32_t w, uint32_t h) const;

    bool opened_ = false;
    VideoPreset preset_{};
    uint64_t sequence_ = 0;
};

// ------------------------------------------------------------

VoidResult VideoTestSrcSource::open(const VideoPreset& preset) {
    if (opened_) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "VideoTestSrc source already open", "videotestsrc");
    }
    preset_ = preset;
    sequence_ = 0;
    opened_ = true;
    return OkVoid();
}

VoidResult VideoTestSrcSource::close() {
    opened_ = false;
    sequence_ = 0;
    return OkVoid();
}

Result<VideoFrame> VideoTestSrcSource::capture_frame() {
    if (!opened_) {
        return Result<VideoFrame>::Err(
            Error{ErrorCode::CameraOpenFailed,
                  "VideoTestSrc source not open", "videotestsrc"});
    }

    uint32_t w = preset_.width;
    uint32_t h = preset_.height;
    // NV12: Y plane = w*h, UV plane = w*h/2
    size_t frame_size = static_cast<size_t>(w) * h * 3 / 2;

    VideoFrame frame;
    frame.data.resize(frame_size);
    generate_smpte_nv12(frame.data, w, h);

    frame.info.width = w;
    frame.info.height = h;
    frame.info.stride = w;
    frame.info.timestamp = std::chrono::steady_clock::now();
    frame.info.sequence_number = sequence_++;

    return Result<VideoFrame>::Ok(std::move(frame));
}

Result<CameraCapabilities> VideoTestSrcSource::query_capabilities() {
    CameraCapabilities caps;
    caps.supported_resolutions = {
        {640, 480},
        {1280, 720},
        {1920, 1080},
    };
    caps.min_fps = 1;
    caps.max_fps = 30;
    caps.pixel_format = "NV12";
    return Result<CameraCapabilities>::Ok(std::move(caps));
}

std::string VideoTestSrcSource::gst_source_description() const {
    return "videotestsrc pattern=smpte is-live=true";
}

void VideoTestSrcSource::generate_smpte_nv12(
    std::vector<uint8_t>& buf, uint32_t w, uint32_t h) const {

    uint8_t* y_plane = buf.data();
    uint8_t* uv_plane = buf.data() + static_cast<size_t>(w) * h;

    // Fill Y plane
    for (uint32_t row = 0; row < h; ++row) {
        for (uint32_t col = 0; col < w; ++col) {
            size_t bar_idx = static_cast<size_t>(col) * NUM_BARS / w;
            if (bar_idx >= NUM_BARS) bar_idx = NUM_BARS - 1;
            y_plane[row * w + col] = SMPTE_BARS[bar_idx].y;
        }
    }

    // Fill UV plane (interleaved U,V pairs, half resolution)
    uint32_t uv_h = h / 2;
    uint32_t uv_w = w / 2;
    for (uint32_t row = 0; row < uv_h; ++row) {
        for (uint32_t col = 0; col < uv_w; ++col) {
            size_t bar_idx = static_cast<size_t>(col * 2) * NUM_BARS / w;
            if (bar_idx >= NUM_BARS) bar_idx = NUM_BARS - 1;
            size_t offset = row * w + col * 2;  // w stride for UV plane in NV12
            uv_plane[offset]     = SMPTE_BARS[bar_idx].u;
            uv_plane[offset + 1] = SMPTE_BARS[bar_idx].v;
        }
    }
}

// ============================================================
// Registration helper (used by factory in camera_source_factory.cpp)
// ============================================================

std::unique_ptr<ICameraSource> create_videotestsrc_source() {
    return std::make_unique<VideoTestSrcSource>();
}

}  // namespace sc
