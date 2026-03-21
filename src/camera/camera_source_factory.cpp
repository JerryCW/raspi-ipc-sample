#include "camera/camera_source.h"

namespace sc {

// Forward declarations for source constructors
std::unique_ptr<ICameraSource> create_videotestsrc_source();

#ifdef HAS_LIBCAMERA
std::unique_ptr<ICameraSource> create_libcamera_source(const std::string& device_path);
#endif

#ifdef HAS_V4L2
std::unique_ptr<ICameraSource> create_v4l2_source(const std::string& device_path);
#endif

Result<std::unique_ptr<ICameraSource>> create_camera_source(
    CameraSourceType type, const std::string& device_path) {

    switch (type) {
        case CameraSourceType::VIDEOTESTSRC:
            return Result<std::unique_ptr<ICameraSource>>::Ok(
                create_videotestsrc_source());

        case CameraSourceType::LIBCAMERA_CSI:
#ifdef HAS_LIBCAMERA
            return Result<std::unique_ptr<ICameraSource>>::Ok(
                create_libcamera_source(device_path));
#else
            return Result<std::unique_ptr<ICameraSource>>::Err(
                Error{ErrorCode::CameraDeviceNotFound,
                      "libcamera support not available (HAS_LIBCAMERA not defined). "
                      "This source requires Linux aarch64 with libcamera installed.",
                      device_path});
#endif

        case CameraSourceType::V4L2_USB:
#ifdef HAS_V4L2
            return Result<std::unique_ptr<ICameraSource>>::Ok(
                create_v4l2_source(device_path));
#else
            return Result<std::unique_ptr<ICameraSource>>::Err(
                Error{ErrorCode::CameraDeviceNotFound,
                      "V4L2 support not available (HAS_V4L2 not defined). "
                      "This source requires Linux with V4L2 headers.",
                      device_path});
#endif

        default:
            return Result<std::unique_ptr<ICameraSource>>::Err(
                Error{ErrorCode::InvalidArgument,
                      "Unknown camera source type: " +
                          std::to_string(static_cast<int>(type)),
                      ""});
    }
}

}  // namespace sc
