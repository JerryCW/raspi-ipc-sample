#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

#include "config/config_manager.h"
#include "core/stream_mode.h"
#include "core/types.h"

#ifdef HAS_GSTREAMER
#include <gst/gst.h>
#endif

namespace sc {

// ============================================================
// PipelineConfig — parameters for building a GStreamer pipeline
// ============================================================

struct PipelineConfig {
    CameraSourceType source_type = CameraSourceType::VIDEOTESTSRC;
    std::string device_path;
    VideoPreset video_preset = PRESET_DEFAULT;
    bool prefer_hw_encoder = true;
    uint32_t gop_size = 0;              // 0 = match fps
    uint32_t webrtc_queue_size = 5;     // leaky queue capacity (frames)
    uint32_t kvs_buffer_duration_ms = 2000;

    // KVS upload config (used when kvssink is available)
    std::string kvs_stream_name;
    std::string kvs_region;             // AWS region for kvssink aws-region property
    uint32_t kvs_storage_size_mb = 128; // kvssink storage-size (already in MB)
    uint32_t kvs_retention_hours = 168; // 7 days
    bool kvs_enabled = false;           // set true when IoT + KVS config is valid

    // IoT certificate fields for kvssink iot-certificate GstStructure
    // Set programmatically via gst_structure_new() after gst_parse_launch
    std::string iot_thing_name;
    std::string iot_credential_endpoint;
    std::string iot_cert_path;
    std::string iot_key_path;
    std::string iot_ca_path;
    std::string iot_role_alias;
};

// ============================================================
// IGStreamerPipeline — abstract interface
// ============================================================

class IGStreamerPipeline {
public:
    virtual ~IGStreamerPipeline() = default;

    virtual VoidResult build(const PipelineConfig& config) = 0;
    virtual VoidResult start() = 0;
    virtual VoidResult stop() = 0;
    virtual VoidResult destroy() = 0;

    // Dynamic bitrate adjustment (no pipeline rebuild needed)
    virtual VoidResult set_bitrate(uint32_t bitrate_kbps) = 0;

    // Pipeline state query
    enum class State : uint8_t {
        NULL_STATE,
        READY,
        PAUSED,
        PLAYING,
        ERROR
    };
    virtual State current_state() const = 0;

    // Encoder type name
    virtual std::string encoder_name() const = 0;

#ifdef HAS_GSTREAMER
    // Access underlying GstElement pipeline (for appsink frame pull, etc.)
    // Returns nullptr if pipeline not built or GStreamer not available.
    virtual GstElement* get_pipeline_element() = 0;
#endif
};

inline const char* pipeline_state_to_string(IGStreamerPipeline::State s) {
    switch (s) {
        case IGStreamerPipeline::State::NULL_STATE: return "NULL";
        case IGStreamerPipeline::State::READY:      return "READY";
        case IGStreamerPipeline::State::PAUSED:     return "PAUSED";
        case IGStreamerPipeline::State::PLAYING:    return "PLAYING";
        case IGStreamerPipeline::State::ERROR:      return "ERROR";
    }
    return "UNKNOWN";
}

// ============================================================
// GStreamerPipeline — concrete implementation
//   Real GStreamer when HAS_GSTREAMER is defined;
//   stub/mock otherwise (macOS dev environment).
// ============================================================

class GStreamerPipeline : public IGStreamerPipeline {
public:
    GStreamerPipeline();
    ~GStreamerPipeline() override;

    // Non-copyable, non-movable (owns GStreamer resources)
    GStreamerPipeline(const GStreamerPipeline&) = delete;
    GStreamerPipeline& operator=(const GStreamerPipeline&) = delete;
    GStreamerPipeline(GStreamerPipeline&&) = delete;
    GStreamerPipeline& operator=(GStreamerPipeline&&) = delete;

    VoidResult build(const PipelineConfig& config) override;
    VoidResult start() override;
    VoidResult stop() override;
    VoidResult destroy() override;
    VoidResult set_bitrate(uint32_t bitrate_kbps) override;
    State current_state() const override;
    std::string encoder_name() const override;

#ifdef HAS_GSTREAMER
    GstElement* get_pipeline_element() override;
#endif

private:
    mutable std::shared_mutex mutex_;
    State state_ = State::NULL_STATE;
    std::string encoder_name_;
    uint32_t bitrate_kbps_ = 0;
    PipelineConfig config_;

#ifdef HAS_GSTREAMER
    // RAII helpers for GStreamer objects
    struct GstPipelineDeleter {
        void operator()(GstElement* p) const {
            if (p) {
                gst_element_set_state(p, GST_STATE_NULL);
                gst_object_unref(p);
            }
        }
    };
    using GstPipelinePtr = std::unique_ptr<GstElement, GstPipelineDeleter>;

    GstPipelinePtr pipeline_;
    GstElement* encoder_element_ = nullptr;  // non-owning, owned by pipeline

    // GMainLoop + bus thread for kvssink async callback dispatching
    GMainLoop* loop_ = nullptr;
    std::thread bus_thread_;

    // Bus callback for error/warning/EOS handling
    static gboolean bus_callback(GstBus* bus, GstMessage* msg, gpointer data);

    // Build helpers
    VoidResult select_encoder(std::string& pipeline_desc) const;
    std::string build_pipeline_description(const PipelineConfig& config,
                                           const std::string& encoder_desc) const;
    GstElement* find_element_by_factory(GstBin* bin, const std::string& factory_name) const;
#endif
};

// Factory function
std::unique_ptr<IGStreamerPipeline> create_gstreamer_pipeline();

}  // namespace sc
