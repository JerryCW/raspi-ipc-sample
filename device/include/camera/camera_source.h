#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "config/config_manager.h"
#include "core/frame.h"
#include "core/stream_mode.h"
#include "core/types.h"

namespace sc {

// ============================================================
// ICameraSource — unified interface for all video sources
// ============================================================

class ICameraSource {
public:
    virtual ~ICameraSource() = default;

    virtual VoidResult open(const VideoPreset& preset) = 0;
    virtual VoidResult close() = 0;
    virtual Result<VideoFrame> capture_frame() = 0;
    virtual Result<CameraCapabilities> query_capabilities() = 0;
    virtual CameraSourceType type() const = 0;

    // Returns GStreamer source element description string for pipeline building
    virtual std::string gst_source_description() const = 0;
};

// ============================================================
// Factory — create the right source based on CameraSourceType
// ============================================================

Result<std::unique_ptr<ICameraSource>> create_camera_source(
    CameraSourceType type, const std::string& device_path = "");

}  // namespace sc
