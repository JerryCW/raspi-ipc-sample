#include "pipeline/gstreamer_pipeline.h"

#include <iostream>
#include <sstream>
#include <thread>

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
        // threads=2: 限制编码线程数，避免 x264 自动开 4-8 个线程
        // 抢占全部核心。720p@15fps ultrafast 下 2 线程足够，
        // 把剩余核心留给解码、AI、系统调度，减少上下文切换开销
        pipeline_desc =
            "x264enc speed-preset=ultrafast tune=zerolatency "
            "threads=2 "
            "bitrate=" + std::to_string(config_.video_preset.bitrate_kbps) +
            " key-int-max=" + std::to_string(gop) +
            " bframes=0";
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
            // 强制 MJPG 采集 + jpegdec 解码：
            // USB 摄像头 YUYV 720p@15fps ≈ 27MB/s，超出 USB 2.0 带宽
            // MJPG ≈ 3-5MB/s，由摄像头硬件压缩
            ss << "v4l2src device=" << config.device_path
               << " ! image/jpeg,width=" << config.video_preset.width
               << ",height=" << config.video_preset.height
               << ",framerate=" << config.video_preset.fps << "/1"
               << " ! jpegdec";
            break;
        case CameraSourceType::VIDEOTESTSRC:
        default:
            ss << "videotestsrc is-live=true pattern=smpte";
            break;
    }

    // videoconvert + videoscale ensure format compatibility
    // (libcamerasrc may output NV12/Rec709 that the encoder doesn't accept)
    // add-borders=false: stretch to fill target resolution instead of adding
    // black pillarbox/letterbox bars when source aspect ratio differs
    // pixel-aspect-ratio=1/1: force square pixels in H.264 SPS so Safari
    // doesn't misinterpret the aspect ratio (Safari respects SAR, Chrome ignores it)
    ss << " ! videoconvert"
       << " ! videoscale add-borders=false"
       << " ! video/x-raw,width=" << config.video_preset.width
       << ",height=" << config.video_preset.height
       << ",framerate=" << config.video_preset.fps << "/1"
       << ",pixel-aspect-ratio=1/1";

    // 两级 Tee 架构：
    //   raw_t (raw video) → 编码链 + AI 分支
    //   h264_t (H.264)    → KVS + WebRTC
    // 编码器只实例化一次，KVS 和 WebRTC 共享同一份 H.264 码流
    ss << " ! tee name=raw_t";

    // ── 编码链：encode once → h264parse → caps → tee(h264) ──
    // h264parse 后必须加 caps filter 固定 stream-format 和 alignment，
    // 否则 tee 下游的 kvssink 和 appsink 对 H.264 caps 协商冲突
    ss << " raw_t. ! queue max-size-buffers=3 leaky=downstream"
       << " ! " << encoder_desc
       << " ! h264parse config-interval=-1"
       << " ! video/x-h264,stream-format=byte-stream,alignment=au"
       << " ! tee name=h264_t";

    // ── KVS 分支：从 h264_t 拿编码后的码流 ──
    // kvssink 直接接受 H.264 数据，不需要额外 caps filter
    if (config.kvs_enabled && !config.kvs_stream_name.empty()) {
        ss << " h264_t. ! queue max-size-buffers=3 leaky=downstream"
           << " ! kvssink name=kvs_sink"
           << " stream-name=" << config.kvs_stream_name
           << " storage-size=" << config.kvs_storage_size_mb
           << " aws-region=" << config.kvs_region
           << " frame-timecodes=true"
           << " retention-period=" << config.kvs_retention_hours;
        if (!config.iot_credential_endpoint.empty()) {
            ss << " iot-certificate=\"iot-certificate"
               << ", iot-thing-name=(string)" << config.iot_thing_name
               << ", endpoint=(string)" << config.iot_credential_endpoint
               << ", cert-path=(string)" << config.iot_cert_path
               << ", key-path=(string)" << config.iot_key_path
               << ", ca-path=(string)" << config.iot_ca_path
               << ", role-aliases=(string)" << config.iot_role_alias
               << "\"";
        }
    } else {
        ss << " h264_t. ! queue max-size-buffers=3 leaky=downstream"
           << " ! fakesink name=kvs_sink sync=false";
    }

    // ── WebRTC 分支：从 h264_t 拿编码后的码流 ──
    // appsink 需要显式 caps 确保拿到 byte-stream 格式
    ss << " h264_t. ! queue max-size-buffers=3 leaky=downstream"
       << " ! video/x-h264,stream-format=byte-stream,alignment=au"
       << " ! appsink name=webrtc_sink max-buffers=1 drop=true sync=false async=false emit-signals=false";

    // ── AI 分支：从 raw_t 拿原始像素，转 BGR 给 Python ──
    ss << " raw_t. ! queue max-size-buffers=2 leaky=downstream"
       << " ! videoconvert"
       << " ! video/x-raw,format=BGR"
       << " ! appsink name=ai_sink max-buffers=2 drop=true sync=false async=false emit-signals=false";

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

    // iot-certificate is now set inline in the pipeline description string
    // (GstStructure serialization format works with gst_parse_launch)

    state_ = State::READY;
    return OkVoid();
}

// ------------------------------------------------------------
// Bus callback — handles error/warning/EOS from pipeline
// ------------------------------------------------------------

gboolean GStreamerPipeline::bus_callback(GstBus* /*bus*/, GstMessage* msg, gpointer data) {
    auto* self = static_cast<GStreamerPipeline*>(data);

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* debug_info = nullptr;
        gst_message_parse_error(msg, &err, &debug_info);
        std::cerr << "[GStreamerPipeline] ERROR: " << (err->message ? err->message : "unknown");
        if (debug_info) std::cerr << " [" << debug_info << "]";
        std::cerr << std::endl;
        self->state_ = State::ERROR;
        g_error_free(err);
        g_free(debug_info);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError* warn = nullptr;
        gchar* debug_info = nullptr;
        gst_message_parse_warning(msg, &warn, &debug_info);
        std::cerr << "[GStreamerPipeline] WARNING: " << (warn->message ? warn->message : "unknown") << std::endl;
        g_error_free(warn);
        g_free(debug_info);
        break;
    }
    case GST_MESSAGE_EOS:
        std::cerr << "[GStreamerPipeline] End-of-stream" << std::endl;
        break;
    default:
        break;
    }
    return TRUE;
}

// ------------------------------------------------------------
// start() — set PLAYING + start GMainLoop for bus dispatching
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

    // Attach bus watch to default context
    GstBus* bus = gst_element_get_bus(pipeline_.get());
    gst_bus_add_watch(bus, bus_callback, this);
    gst_object_unref(bus);

    // Start GMainLoop on a dedicated bus thread — required for kvssink's
    // internal async operations (putMedia, credential refresh).
    // This matches the ipc-kvs-demo architecture.
    loop_ = g_main_loop_new(nullptr, FALSE);
    bus_thread_ = std::thread([this]() {
        g_main_loop_run(loop_);
    });

    // Release lock before set_state — kvssink's state change may trigger
    // callbacks on the bus thread that could interact with our state.
    // Also, set_state can block for several seconds during credential fetch.
    lock.unlock();

    // Set pipeline to PLAYING (may trigger async kvssink operations that
    // require the GMainLoop to be running)
    GstStateChangeReturn ret = gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING);

    lock.lock();

    if (ret == GST_STATE_CHANGE_FAILURE) {
        state_ = State::ERROR;
        return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                       "Failed to set pipeline to PLAYING state",
                       "GStreamerPipeline::start");
    }

    state_ = State::PLAYING;
    std::cerr << "[GStreamerPipeline] Pipeline PLAYING (ret=" << ret << ")" << std::endl;

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

    // Stop GMainLoop so bus thread can join
    if (loop_) {
        g_main_loop_quit(loop_);
    }
    lock.unlock();
    if (bus_thread_.joinable()) {
        bus_thread_.join();
    }
    lock.lock();
    if (loop_) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }

    return OkVoid();
}

// ------------------------------------------------------------
// destroy()
// ------------------------------------------------------------

VoidResult GStreamerPipeline::destroy() {
    std::unique_lock lock(mutex_);

    // Stop GMainLoop if still running
    if (loop_) {
        g_main_loop_quit(loop_);
        lock.unlock();
        if (bus_thread_.joinable()) bus_thread_.join();
        lock.lock();
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }

    encoder_element_ = nullptr;
    pipeline_.reset();

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

GstElement* GStreamerPipeline::get_pipeline_element() {
    std::shared_lock lock(mutex_);
    return pipeline_ ? pipeline_.get() : nullptr;
}

#endif  // HAS_GSTREAMER

}  // namespace sc
