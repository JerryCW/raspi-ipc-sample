#include "camera/camera_source.h"

#ifdef HAS_V4L2

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sc {

// ============================================================
// V4L2Source — USB camera via v4l2src (Linux only)
// ============================================================

class V4L2Source : public ICameraSource {
public:
    explicit V4L2Source(std::string device_path)
        : device_path_(std::move(device_path)) {}

    VoidResult open(const VideoPreset& preset) override;
    VoidResult close() override;
    Result<VideoFrame> capture_frame() override;
    Result<CameraCapabilities> query_capabilities() override;
    CameraSourceType type() const override { return CameraSourceType::V4L2_USB; }
    std::string gst_source_description() const override;

private:
    bool device_exists() const;

    std::string device_path_;
    bool opened_ = false;
    VideoPreset preset_{};
    uint64_t sequence_ = 0;
    int fd_ = -1;
};

// ------------------------------------------------------------

bool V4L2Source::device_exists() const {
    struct stat st;
    if (::stat(device_path_.c_str(), &st) != 0) return false;
    return S_ISCHR(st.st_mode);
}

VoidResult V4L2Source::open(const VideoPreset& preset) {
    if (opened_) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "V4L2 source already open", device_path_);
    }

    if (!device_exists()) {
        return ErrVoid(ErrorCode::CameraDeviceNotFound,
                       "V4L2 device not found: " + device_path_,
                       device_path_);
    }

    fd_ = ::open(device_path_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        return ErrVoid(ErrorCode::CameraOpenFailed,
                       "Failed to open V4L2 device: " + device_path_ +
                       " (" + std::strerror(errno) + ")",
                       device_path_);
    }

    // Verify it's a video capture device
    struct v4l2_capability cap;
    std::memset(&cap, 0, sizeof(cap));
    if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        ::close(fd_);
        fd_ = -1;
        return ErrVoid(ErrorCode::CameraOpenFailed,
                       "Device is not a video capture device: " + device_path_,
                       device_path_);
    }

    preset_ = preset;
    sequence_ = 0;
    opened_ = true;
    return OkVoid();
}

VoidResult V4L2Source::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    opened_ = false;
    sequence_ = 0;
    return OkVoid();
}

Result<VideoFrame> V4L2Source::capture_frame() {
    if (!opened_) {
        return Result<VideoFrame>::Err(
            Error{ErrorCode::CameraOpenFailed,
                  "V4L2 source not open", device_path_});
    }

    // Real implementation would use V4L2 streaming I/O (MMAP or USERPTR).
    // Placeholder: return frame with correct metadata.
    uint32_t w = preset_.width;
    uint32_t h = preset_.height;
    VideoFrame frame;
    frame.data.resize(static_cast<size_t>(w) * h * 3 / 2, 0);
    frame.info.width = w;
    frame.info.height = h;
    frame.info.stride = w;
    frame.info.timestamp = std::chrono::steady_clock::now();
    frame.info.sequence_number = sequence_++;
    return Result<VideoFrame>::Ok(std::move(frame));
}

Result<CameraCapabilities> V4L2Source::query_capabilities() {
    if (!device_exists()) {
        return Result<CameraCapabilities>::Err(
            Error{ErrorCode::CameraCapabilityQueryFailed,
                  "V4L2 device not found: " + device_path_,
                  device_path_});
    }

    // Real implementation would enumerate formats via VIDIOC_ENUM_FMT / VIDIOC_ENUM_FRAMESIZES.
    CameraCapabilities caps;
    caps.supported_resolutions = {
        {640, 480},
        {1280, 720},
        {1920, 1080},
    };
    caps.min_fps = 1;
    caps.max_fps = 30;
    caps.pixel_format = "YUY2";
    return Result<CameraCapabilities>::Ok(std::move(caps));
}

std::string V4L2Source::gst_source_description() const {
    // 强制 MJPG 采集：让摄像头硬件做 JPEG 压缩，大幅降低 USB 带宽
    // （YUYV 720p@15fps ≈ 27MB/s → MJPG ≈ 3-5MB/s）
    // jpegdec 解码后再进入后续管线
    return "v4l2src device=" + device_path_ +
           " ! image/jpeg,width=" + std::to_string(preset_.width) +
           ",height=" + std::to_string(preset_.height) +
           ",framerate=" + std::to_string(preset_.fps) + "/1"
           " ! jpegdec";
}

// Factory helper
std::unique_ptr<ICameraSource> create_v4l2_source(const std::string& device_path) {
    return std::make_unique<V4L2Source>(device_path);
}

}  // namespace sc

#endif  // HAS_V4L2
