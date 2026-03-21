#include "camera/camera_source.h"

#ifdef HAS_LIBCAMERA

#include <chrono>
#include <fstream>
#include <string>

namespace sc {

// ============================================================
// LibcameraSource — CSI camera via libcamerasrc (Linux aarch64)
// Raspberry Pi 5 uses rp1-cfe driver, Pi 4 uses bcm2835-unicam.
// ============================================================

class LibcameraSource : public ICameraSource {
public:
    explicit LibcameraSource(std::string device_path)
        : device_path_(std::move(device_path)) {}

    VoidResult open(const VideoPreset& preset) override;
    VoidResult close() override;
    Result<VideoFrame> capture_frame() override;
    Result<CameraCapabilities> query_capabilities() override;
    CameraSourceType type() const override { return CameraSourceType::LIBCAMERA_CSI; }
    std::string gst_source_description() const override;

private:
    static bool detect_csi_driver();

    std::string device_path_;
    bool opened_ = false;
    VideoPreset preset_{};
    uint64_t sequence_ = 0;
};

// ------------------------------------------------------------

bool LibcameraSource::detect_csi_driver() {
    // Check /proc/modules for known CSI driver names
    std::ifstream modules("/proc/modules");
    if (!modules.is_open()) return false;

    std::string line;
    while (std::getline(modules, line)) {
        // Pi 5: rp1-cfe (appears as rp1_cfe in modules)
        if (line.find("rp1_cfe") != std::string::npos) return true;
        // Pi 4: bcm2835-unicam (appears as bcm2835_unicam)
        if (line.find("bcm2835_unicam") != std::string::npos) return true;
    }
    return false;
}

VoidResult LibcameraSource::open(const VideoPreset& preset) {
    if (opened_) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Libcamera source already open", device_path_);
    }

    if (!detect_csi_driver()) {
        return ErrVoid(ErrorCode::CameraDeviceNotFound,
                       "No CSI camera driver detected (expected rp1-cfe or bcm2835-unicam)",
                       device_path_);
    }

    preset_ = preset;
    sequence_ = 0;
    opened_ = true;
    return OkVoid();
}

VoidResult LibcameraSource::close() {
    opened_ = false;
    sequence_ = 0;
    return OkVoid();
}

Result<VideoFrame> LibcameraSource::capture_frame() {
    if (!opened_) {
        return Result<VideoFrame>::Err(
            Error{ErrorCode::CameraOpenFailed,
                  "Libcamera source not open", device_path_});
    }

    // Real implementation would use libcamera API to dequeue a buffer.
    // Placeholder: return empty frame with correct metadata.
    VideoFrame frame;
    uint32_t w = preset_.width;
    uint32_t h = preset_.height;
    frame.data.resize(static_cast<size_t>(w) * h * 3 / 2, 0);
    frame.info.width = w;
    frame.info.height = h;
    frame.info.stride = w;
    frame.info.timestamp = std::chrono::steady_clock::now();
    frame.info.sequence_number = sequence_++;
    return Result<VideoFrame>::Ok(std::move(frame));
}

Result<CameraCapabilities> LibcameraSource::query_capabilities() {
    if (!detect_csi_driver()) {
        return Result<CameraCapabilities>::Err(
            Error{ErrorCode::CameraCapabilityQueryFailed,
                  "No CSI camera driver detected (expected rp1-cfe or bcm2835-unicam)",
                  device_path_});
    }

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

std::string LibcameraSource::gst_source_description() const {
    return "libcamerasrc";
}

// Factory helper
std::unique_ptr<ICameraSource> create_libcamera_source(const std::string& device_path) {
    return std::make_unique<LibcameraSource>(device_path);
}

}  // namespace sc

#endif  // HAS_LIBCAMERA
