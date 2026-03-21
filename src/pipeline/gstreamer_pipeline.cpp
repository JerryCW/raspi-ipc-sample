#include "pipeline/gstreamer_pipeline.h"

#include <iostream>
#include <sstream>

namespace sc {

// ============================================================
// Factory
// ============================================================

std::unique_ptr<IGStreamerPipeline> create_gstreamer_pipeline() {
    return std::make_unique<GStreamerPipeline>();
}

// ============================================================
// Stub implementation (no GStreamer available — macOS dev)
// ============================================================

#ifndef HAS_GSTREAMER

GStreamerPipeline::GStreamerPipeline() = default;
GStreamerPipeline::~GStreamerPipeline() = default;

VoidResult GStreamerPipeline::build(const PipelineConfig& config) {
    std::unique_lock lock(mutex_);
    config_ = config;
    bitrate_kbps_ = config.video_preset.bitrate_kbps;
    encoder_name_ = "stub";
    state_ = State::READY;
    return OkVoid();
}

VoidResult GStreamerPipeline::start() {
    std::unique_lock lock(mutex_);
    if (state_ != State::READY && state_ != State::PAUSED) {
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Cannot start: pipeline not in READY or PAUSED state",
                       "GStreamerPipeline::start");
    }
    state_ = State::PLAYING;
    return OkVoid();
}

VoidResult GStreamerPipeline::stop() {
    std::unique_lock lock(mutex_);
    if (state_ == State::NULL_STATE) {
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Cannot stop: pipeline in NULL state",
                       "GStreamerPipeline::stop");
    }
    state_ = State::PAUSED;
    return OkVoid();
}

VoidResult GStreamerPipeline::destroy() {
    std::unique_lock lock(mutex_);
    state_ = State::NULL_STATE;
    encoder_name_.clear();
    bitrate_kbps_ = 0;
    return OkVoid();
}

VoidResult GStreamerPipeline::set_bitrate(uint32_t bitrate_kbps) {
    std::unique_lock lock(mutex_);
    if (state_ == State::NULL_STATE) {
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Cannot set bitrate: pipeline not built",
                       "GStreamerPipeline::set_bitrate");
    }
    bitrate_kbps_ = bitrate_kbps;
    return OkVoid();
}

IGStreamerPipeline::State GStreamerPipeline::current_state() const {
    std::shared_lock lock(mutex_);
    return state_;
}

std::string GStreamerPipeline::encoder_name() const {
    std::shared_lock lock(mutex_);
    return encoder_name_;
}

#else  // HAS_GSTREAMER defined — real implementation

// ============================================================
// Real GStreamer implementation
// ============================================================

GStreamerPipeline::GStreamerPipeline() {
    // Ensure GStreamer is initialized (safe to call multiple times)
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }
}

GStreamerPipeline::~GStreamerPipeline() {
    // RAII: pipeline_ unique_ptr handles cleanup via GstPipelineDeleter
    std::unique_lock lock(mutex_);
    encoder_element_ = nullptr;
    // pipeline_ destructor runs after lock release — that's fine,
    // GstPipelineDeleter sets state to NULL and unrefs.
}

// ------------------------------------------------------------
// Encoder selection: prefer v4l2h264enc, fallback x264enc
// ------------------------------------------------------------

VoidResult GStreamerPipeline::select_encoder(std::string& pipeline_desc) const {
    if (config_.prefer_hw_encoder) {
        GstElementFactory* hw_factory = gst_element_factory_find("v4l2h264enc");
        if (hw_factory) {
            gst_object_unref(hw_factory);
            // v4l2h264enc with Baseline profile via extra-controls
            pipeline_desc =
                "v4l2h264enc "
                "extra-controls=\"encode,h264_profile=0,video_bitrate=" +
                std::to_string(config_.video_preset.bitrate_kbps * 1000) + "\"";
            return OkVoid();
        }
    }

    // Fallback: x264enc
    GstElementFactory* sw_factory = gst_element_factory_find("x264enc");
    if (sw_factory) {
        gst_object_unref(sw_factory);
        uint32_t gop = config_.gop_size > 0 ? config_.gop_size
                                             : config_.video_preset.fps;
        pipeline_desc =
            "x264enc speed-preset=ultrafast tune=zerolatency "
            "bitrate=" + std::to_string(config_.video_preset.bitrate_kbps) +
            " key-int-max=" + std::to_string(gop) +
            " option-string=\"--profile=baseline\"";
        return OkVoid();
    }

    return ErrVoid(ErrorCode::EncoderNotAvailable,
                   "Neither v4l2h264enc nor x264enc encoder available",
                   "GStreamerPipeline::select_encoder");
}

// ------------------------------------------------------------
// Build pipeline description string
// ------------------------------------------------------------

std::string GStreamerPipeline::build_pipeline_description(
    const PipelineConfig& config,
    const std::string& encoder_desc) const {

    std::ostringstream ss;

    // Source element
    switch (config.source_type) {
        case CameraSourceType::LIBCAMERA_CSI:
            ss << "libcamerasrc";
            break;
        case CameraSourceType::V4L2_USB:
            ss << "v4l2src device=" << config.device_path;
            break;
        case CameraSourceType::VIDEOTESTSRC:
        default:
            ss << "videotestsrc is-live=true pattern=smpte";
            break;
    }

    // videoconvert + videoscale ensure format compatibility
    // (libcamerasrc may output NV12/Rec709 that the encoder doesn't accept)
    ss << " ! videoconvert"
       << " ! videoscale"
       << " ! video/x-raw,width=" << config.video_preset.width
       << ",height=" << config.video_preset.height
       << ",framerate=" << config.video_preset.fps << "/1";

    // Tee raw video to multiple branches — each branch encodes independently
    ss << " ! tee name=t";

    // ── KVS branch: encode → h264parse → kvssink ──
    // kvssink handles byte-stream H.264 directly (no avc conversion needed).
    // iot-certificate is set programmatically in build() after gst_parse_launch.
    if (config.kvs_enabled && !config.kvs_stream_name.empty()) {
        ss << " t. ! queue max-size-buffers=3 leaky=downstream"
           << " ! " << encoder_desc
           << " ! h264parse config-interval=-1"
           << " ! kvssink name=kvs_sink"
           << " stream-name=" << config.kvs_stream_name
           << " storage-size=" << config.kvs_storage_size_mb
           << " aws-region=" << config.kvs_region
           << " frame-timecodes=true"
           << " retention-period=" << config.kvs_retention_hours;
    } else {
        ss << " t. ! queue max-size-buffers=3 leaky=downstream"
           << " ! fakesink name=kvs_sink sync=false";
    }

    // ── WebRTC branch: encode → h264parse → byte-stream appsink ──
    ss << " t. ! queue max-size-buffers=3 leaky=downstream"
       << " ! " << encoder_desc
       << " ! h264parse config-interval=-1"
       << " ! video/x-h264,stream-format=byte-stream,alignment=au"
       << " ! appsink name=webrtc_sink max-buffers=1 drop=true sync=false emit-signals=true";

    // ── AI branch: raw video appsink for Frame_Buffer_Pool ──
    ss << " t. ! queue max-size-buffers=2 leaky=downstream"
       << " ! appsink name=ai_sink emit-signals=true sync=false";

    return ss.str();
}

// ------------------------------------------------------------
// Find element by factory name (iterate bin elements)
// ------------------------------------------------------------

GstElement* GStreamerPipeline::find_element_by_factory(
    GstBin* bin, const std::string& factory_name) const {

    GstIterator* it = gst_bin_iterate_elements(bin);
    GValue item = G_VALUE_INIT;
    GstElement* found = nullptr;

    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        GstElement* elem = GST_ELEMENT(g_value_get_object(&item));
        GstElementFactory* factory = gst_element_get_factory(elem);
        if (factory) {
            const gchar* name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
            if (name && factory_name == name) {
                found = elem;
                g_value_unset(&item);
                break;
            }
        }
        g_value_unset(&item);
    }
    gst_iterator_free(it);
    return found;
}

// ------------------------------------------------------------
// build()
// ------------------------------------------------------------

VoidResult GStreamerPipeline::build(const PipelineConfig& config) {
    std::unique_lock lock(mutex_);

    config_ = config;
    bitrate_kbps_ = config.video_preset.bitrate_kbps;

    // Select encoder
    std::string encoder_desc;
    auto enc_result = select_encoder(encoder_desc);
    if (enc_result.is_err()) {
        state_ = State::ERROR;
        return enc_result;
    }

    // Determine encoder name for reporting
    if (encoder_desc.find("v4l2h264enc") != std::string::npos) {
        encoder_name_ = "v4l2h264enc";
    } else {
        encoder_name_ = "x264enc";
    }

    // Build pipeline description
    std::string desc = build_pipeline_description(config, encoder_desc);

    // Diagnostic: log the full pipeline description and encoder choice
    std::cerr << "[GStreamerPipeline] encoder=" << encoder_name_ << std::endl;
    std::cerr << "[GStreamerPipeline] pipeline=" << desc << std::endl;

    // Parse and create pipeline
    GError* error = nullptr;
    GstElement* raw_pipeline = gst_parse_launch(desc.c_str(), &error);
    if (!raw_pipeline || error) {
        std::string err_msg = error ? error->message : "Unknown parse error";
        if (error) g_error_free(error);
        state_ = State::ERROR;
        return ErrVoid(ErrorCode::PipelineBuildFailed,
                       "Failed to build pipeline: " + err_msg,
                       "GStreamerPipeline::build");
    }
    if (error) g_error_free(error);

    pipeline_.reset(raw_pipeline);

    // Find encoder element by factory name for dynamic bitrate control
    if (GST_IS_BIN(raw_pipeline)) {
        encoder_element_ = find_element_by_factory(
            GST_BIN(raw_pipeline), encoder_name_);
    }

    // Set iot-certificate on kvssink via gst_structure_new (NOT gst_structure_from_string)
    // This matches the proven pattern from ipc-kvs-demo.
    if (config.kvs_enabled && !config.iot_credential_endpoint.empty()
        && GST_IS_BIN(raw_pipeline)) {
        GstElement* kvs_sink = find_element_by_factory(
            GST_BIN(raw_pipeline), "kvssink");
        if (kvs_sink) {
            GstStructure* iot_creds = gst_structure_new(
                "iot-certificate",
                "iot-thing-name", G_TYPE_STRING, config.iot_thing_name.c_str(),
                "endpoint", G_TYPE_STRING, config.iot_credential_endpoint.c_str(),
                "cert-path", G_TYPE_STRING, config.iot_cert_path.c_str(),
                "key-path", G_TYPE_STRING, config.iot_key_path.c_str(),
                "ca-path", G_TYPE_STRING, config.iot_ca_path.c_str(),
                "role-aliases", G_TYPE_STRING, config.iot_role_alias.c_str(),
                NULL);
            g_object_set(G_OBJECT(kvs_sink), "iot-certificate", iot_creds, NULL);
            gst_structure_free(iot_creds);
        }
    }

    state_ = State::READY;
    return OkVoid();
}

// ------------------------------------------------------------
// start()
// ------------------------------------------------------------

VoidResult GStreamerPipeline::start() {
    std::unique_lock lock(mutex_);

    if (!pipeline_) {
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Cannot start: pipeline not built",
                       "GStreamerPipeline::start");
    }
    if (state_ != State::READY && state_ != State::PAUSED) {
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Cannot start: pipeline not in READY or PAUSED state",
                       "GStreamerPipeline::start");
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        state_ = State::ERROR;
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Failed to set pipeline to PLAYING state",
                       "GStreamerPipeline::start");
    }

    state_ = State::PLAYING;
    return OkVoid();
}

// ------------------------------------------------------------
// stop()
// ------------------------------------------------------------

VoidResult GStreamerPipeline::stop() {
    std::unique_lock lock(mutex_);

    if (!pipeline_) {
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Cannot stop: pipeline not built",
                       "GStreamerPipeline::stop");
    }
    if (state_ == State::NULL_STATE) {
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Cannot stop: pipeline in NULL state",
                       "GStreamerPipeline::stop");
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline_.get(), GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        state_ = State::ERROR;
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Failed to set pipeline to PAUSED state",
                       "GStreamerPipeline::stop");
    }

    state_ = State::PAUSED;
    return OkVoid();
}

// ------------------------------------------------------------
// destroy()
// ------------------------------------------------------------

VoidResult GStreamerPipeline::destroy() {
    std::unique_lock lock(mutex_);

    encoder_element_ = nullptr;
    pipeline_.reset();  // RAII: sets state to NULL and unrefs

    state_ = State::NULL_STATE;
    encoder_name_.clear();
    bitrate_kbps_ = 0;
    return OkVoid();
}

// ------------------------------------------------------------
// set_bitrate() — dynamic, no pipeline rebuild
// ------------------------------------------------------------

VoidResult GStreamerPipeline::set_bitrate(uint32_t bitrate_kbps) {
    std::unique_lock lock(mutex_);

    if (!pipeline_ || state_ == State::NULL_STATE) {
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Cannot set bitrate: pipeline not built",
                       "GStreamerPipeline::set_bitrate");
    }

    if (!encoder_element_) {
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Cannot set bitrate: encoder element not found",
                       "GStreamerPipeline::set_bitrate");
    }

    // v4l2h264enc uses bits/sec in extra-controls; x264enc uses kbps
    if (encoder_name_ == "v4l2h264enc") {
        // v4l2h264enc: set via extra-controls video_bitrate (bits/sec)
        GstStructure* extra = gst_structure_new("controls",
            "video_bitrate", G_TYPE_INT, static_cast<gint>(bitrate_kbps * 1000),
            nullptr);
        g_object_set(encoder_element_, "extra-controls", extra, nullptr);
        gst_structure_free(extra);
    } else {
        // x264enc: bitrate property in kbps
        g_object_set(encoder_element_, "bitrate", static_cast<guint>(bitrate_kbps), nullptr);
    }

    bitrate_kbps_ = bitrate_kbps;
    return OkVoid();
}

// ------------------------------------------------------------
// current_state()
// ------------------------------------------------------------

IGStreamerPipeline::State GStreamerPipeline::current_state() const {
    std::shared_lock lock(mutex_);
    return state_;
}

// ------------------------------------------------------------
// encoder_name()
// ------------------------------------------------------------

std::string GStreamerPipeline::encoder_name() const {
    std::shared_lock lock(mutex_);
    return encoder_name_;
}

#endif  // HAS_GSTREAMER

}  // namespace sc
