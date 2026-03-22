#include <gtest/gtest.h>
#include "pipeline/gstreamer_pipeline.h"

#include <string>
#include <thread>
#include <vector>

using namespace sc;

// ============================================================
// Helper: populate IoT credential fields on PipelineConfig
// ============================================================

namespace {

void set_iot_fields(PipelineConfig& config,
                    const std::string& thing = "cam",
                    const std::string& endpoint = "ep",
                    const std::string& cert = "/c",
                    const std::string& key = "/k",
                    const std::string& ca = "/ca",
                    const std::string& role = "role") {
    config.iot_thing_name = thing;
    config.iot_credential_endpoint = endpoint;
    config.iot_cert_path = cert;
    config.iot_key_path = key;
    config.iot_ca_path = ca;
    config.iot_role_alias = role;
}

void clear_iot_fields(PipelineConfig& config) {
    config.iot_thing_name.clear();
    config.iot_credential_endpoint.clear();
    config.iot_cert_path.clear();
    config.iot_key_path.clear();
    config.iot_ca_path.clear();
    config.iot_role_alias.clear();
}

}  // namespace

// ============================================================
// Factory
// ============================================================

TEST(GStreamerPipelineFactory, CreateReturnsNonNull) {
    auto pipeline = create_gstreamer_pipeline();
    ASSERT_NE(pipeline, nullptr);
}

// ============================================================
// Initial state
// ============================================================

TEST(GStreamerPipeline, InitialStateIsNull) {
    auto pipeline = create_gstreamer_pipeline();
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

TEST(GStreamerPipeline, InitialEncoderNameIsEmpty) {
    auto pipeline = create_gstreamer_pipeline();
    EXPECT_TRUE(pipeline->encoder_name().empty());
}

// ============================================================
// build() — pipeline construction with videotestsrc
// Validates: Requirements 4.1, 4.7
// ============================================================

TEST(GStreamerPipeline, BuildWithDefaultConfigSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, BuildWithHDPresetSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_HD;

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, BuildWithLowBandwidthPresetSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_LOW_BW;

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, BuildSetsEncoderName) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    std::string enc = pipeline->encoder_name();
    EXPECT_FALSE(enc.empty());
}

// ============================================================
// Encoder fallback logic
// Validates: Requirements 4.3
// On macOS stub: encoder_name is "stub"
// On real GStreamer: v4l2h264enc → x264enc → EncoderNotAvailable
// ============================================================

TEST(GStreamerPipeline, EncoderNameIsSetAfterBuild) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.prefer_hw_encoder = true;

    auto result = pipeline->build(config);
    ASSERT_TRUE(result.is_ok());

    std::string enc = pipeline->encoder_name();
    // In stub mode: "stub"; in real GStreamer: "v4l2h264enc" or "x264enc"
    EXPECT_FALSE(enc.empty());
#ifndef HAS_GSTREAMER
    EXPECT_EQ(enc, "stub");
#else
    // Real GStreamer: should be one of the known encoders
    EXPECT_TRUE(enc == "v4l2h264enc" || enc == "x264enc");
#endif
}

TEST(GStreamerPipeline, BuildWithHWEncoderPreference) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.prefer_hw_encoder = true;

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
}

TEST(GStreamerPipeline, BuildWithSWEncoderFallback) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.prefer_hw_encoder = false;

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
}

// ============================================================
// Dynamic bitrate adjustment
// Validates: Requirements 4.5
// ============================================================

TEST(GStreamerPipeline, SetBitrateSucceedsWhenBuilt) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    auto result = pipeline->set_bitrate(1024);
    EXPECT_TRUE(result.is_ok());
}

TEST(GStreamerPipeline, SetBitrateFailsWhenNotBuilt) {
    auto pipeline = create_gstreamer_pipeline();
    auto result = pipeline->set_bitrate(1024);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::PipelineStateChangeFailed);
}

TEST(GStreamerPipeline, SetBitrateSucceedsWhilePlaying) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    pipeline->start();
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    auto result = pipeline->set_bitrate(512);
    EXPECT_TRUE(result.is_ok());
}

TEST(GStreamerPipeline, SetBitrateSucceedsWhilePaused) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    pipeline->start();
    pipeline->stop();
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    auto result = pipeline->set_bitrate(4096);
    EXPECT_TRUE(result.is_ok());
}

TEST(GStreamerPipeline, SetBitrateFailsAfterDestroy) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    pipeline->destroy();

    auto result = pipeline->set_bitrate(1024);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::PipelineStateChangeFailed);
}

// ============================================================
// Pipeline state transitions
// Validates: Requirements 4.5 (state management)
// ============================================================

TEST(GStreamerPipeline, StartFromReadySucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);

    auto result = pipeline->start();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);
}

TEST(GStreamerPipeline, StopFromPlayingSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    pipeline->start();
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    auto result = pipeline->stop();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);
}

TEST(GStreamerPipeline, StartFromPausedSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    pipeline->start();
    pipeline->stop();
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    auto result = pipeline->start();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);
}

TEST(GStreamerPipeline, StartFromNullFails) {
    auto pipeline = create_gstreamer_pipeline();
    auto result = pipeline->start();
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::PipelineStateChangeFailed);
}

TEST(GStreamerPipeline, StopFromNullFails) {
    auto pipeline = create_gstreamer_pipeline();
    auto result = pipeline->stop();
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::PipelineStateChangeFailed);
}

TEST(GStreamerPipeline, StartFromPlayingFails) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    pipeline->start();

    auto result = pipeline->start();
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::PipelineStateChangeFailed);
}

TEST(GStreamerPipeline, DestroyFromPlayingSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    pipeline->start();

    auto result = pipeline->destroy();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

TEST(GStreamerPipeline, DestroyFromReadySucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    auto result = pipeline->destroy();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

TEST(GStreamerPipeline, DestroyFromNullSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    auto result = pipeline->destroy();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

TEST(GStreamerPipeline, DestroyClearsEncoderName) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    EXPECT_FALSE(pipeline->encoder_name().empty());

    pipeline->destroy();
    EXPECT_TRUE(pipeline->encoder_name().empty());
}

// ============================================================
// Full lifecycle: build → start → stop → destroy
// ============================================================

TEST(GStreamerPipeline, FullLifecycle) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    // build
    ASSERT_TRUE(pipeline->build(config).is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);

    // start
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    // dynamic bitrate
    ASSERT_TRUE(pipeline->set_bitrate(1024).is_ok());

    // stop
    ASSERT_TRUE(pipeline->stop().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    // restart
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    // destroy
    ASSERT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
    EXPECT_TRUE(pipeline->encoder_name().empty());
}

// ============================================================
// Rebuild after destroy
// ============================================================

TEST(GStreamerPipeline, RebuildAfterDestroy) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    // First lifecycle
    pipeline->build(config);
    pipeline->start();
    pipeline->destroy();
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);

    // Rebuild with different preset
    config.video_preset = PRESET_HD;
    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
    EXPECT_FALSE(pipeline->encoder_name().empty());
}

// ============================================================
// pipeline_state_to_string helper
// ============================================================

TEST(PipelineStateToString, AllStates) {
    EXPECT_STREQ(pipeline_state_to_string(IGStreamerPipeline::State::NULL_STATE), "NULL");
    EXPECT_STREQ(pipeline_state_to_string(IGStreamerPipeline::State::READY), "READY");
    EXPECT_STREQ(pipeline_state_to_string(IGStreamerPipeline::State::PAUSED), "PAUSED");
    EXPECT_STREQ(pipeline_state_to_string(IGStreamerPipeline::State::PLAYING), "PLAYING");
    EXPECT_STREQ(pipeline_state_to_string(IGStreamerPipeline::State::ERROR), "ERROR");
}

// ============================================================
// Thread safety: concurrent state reads during transitions
// ============================================================

TEST(GStreamerPipeline, ConcurrentStateReads) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    pipeline->build(config);
    pipeline->start();

    // Spawn multiple reader threads
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&pipeline]() {
            for (int j = 0; j < 100; ++j) {
                auto state = pipeline->current_state();
                (void)state;
                auto enc = pipeline->encoder_name();
                (void)enc;
            }
        });
    }

    for (auto& t : readers) {
        t.join();
    }

    // Pipeline should still be in a valid state
    auto state = pipeline->current_state();
    EXPECT_TRUE(state == IGStreamerPipeline::State::PLAYING ||
                state == IGStreamerPipeline::State::PAUSED ||
                state == IGStreamerPipeline::State::READY);
}

// ============================================================
// PipelineConfig defaults
// ============================================================

TEST(PipelineConfig, DefaultValues) {
    PipelineConfig config;
    EXPECT_EQ(config.source_type, CameraSourceType::VIDEOTESTSRC);
    EXPECT_TRUE(config.device_path.empty());
    EXPECT_TRUE(config.prefer_hw_encoder);
    EXPECT_EQ(config.gop_size, 0u);
    EXPECT_EQ(config.webrtc_queue_size, 5u);
    EXPECT_EQ(config.kvs_buffer_duration_ms, 2000u);

    // KVS upload config defaults
    EXPECT_TRUE(config.kvs_stream_name.empty());
    EXPECT_TRUE(config.kvs_region.empty());
    EXPECT_EQ(config.kvs_storage_size_mb, 128u);
    EXPECT_EQ(config.kvs_retention_hours, 168u);
    EXPECT_FALSE(config.kvs_enabled);

    // IoT certificate fields default empty
    EXPECT_TRUE(config.iot_thing_name.empty());
    EXPECT_TRUE(config.iot_credential_endpoint.empty());
    EXPECT_TRUE(config.iot_cert_path.empty());
    EXPECT_TRUE(config.iot_key_path.empty());
    EXPECT_TRUE(config.iot_ca_path.empty());
    EXPECT_TRUE(config.iot_role_alias.empty());
}

// ============================================================
// KVS config fields — storage-size is already in MB
// Validates: Key Gotcha "kvssink storage-size is already in MB"
// ============================================================

TEST(PipelineConfig, KvsStorageSizeIsMB) {
    PipelineConfig config;
    // Verify the default is a reasonable MB value, NOT accidentally multiplied
    EXPECT_EQ(config.kvs_storage_size_mb, 128u);
    EXPECT_LT(config.kvs_storage_size_mb, 1024u);
    EXPECT_GT(config.kvs_storage_size_mb, 0u);
}

TEST(PipelineConfig, KvsRetentionDefaultIs7Days) {
    PipelineConfig config;
    EXPECT_EQ(config.kvs_retention_hours, 168u);  // 7 * 24 = 168
}

// ============================================================
// Build with KVS config populated
// ============================================================

TEST(GStreamerPipeline, BuildWithKvsEnabledSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = true;
    config.kvs_stream_name = "test-stream";
    set_iot_fields(config);
    config.kvs_storage_size_mb = 256;
    config.kvs_retention_hours = 48;

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, BuildWithKvsDisabledSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = false;
    config.kvs_stream_name = "ignored-stream";

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, FullLifecycleWithKvsConfig) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = true;
    config.kvs_stream_name = "lifecycle-stream";
    set_iot_fields(config);

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    ASSERT_TRUE(pipeline->set_bitrate(1024).is_ok());
    ASSERT_TRUE(pipeline->stop().is_ok());
    ASSERT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

// ============================================================
// KVS iot-certificate edge cases
// The iot-certificate is a GstStructure property that cannot be
// set via gst_parse_launch. It must be set programmatically
// via g_object_set after pipeline creation. These tests verify
// the various config combinations around this behavior.
// ============================================================

TEST(GStreamerPipeline, BuildWithKvsEnabled_EmptyIotCertificate_Succeeds) {
    // KVS enabled with stream name but no iot-certificate — should still build
    // (iot-certificate is set post-parse only when non-empty)
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = true;
    config.kvs_stream_name = "test-stream";
    // IoT fields left empty — skip post-parse step

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, BuildWithKvsEnabled_EmptyStreamName_UsesFakesink) {
    // KVS enabled but empty stream name — should fall through to fakesink branch
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = true;
    config.kvs_stream_name = "";  // empty → fakesink path
    set_iot_fields(config);

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, BuildWithKvsDisabled_IotCertificateIgnored) {
    // KVS disabled — iot-certificate should be completely ignored
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = false;
    config.kvs_stream_name = "some-stream";
    set_iot_fields(config);

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, BuildWithKvsEnabled_CustomStorageAndRetention) {
    // Verify non-default storage-size and retention values are accepted
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = true;
    config.kvs_stream_name = "custom-stream";
    set_iot_fields(config);
    config.kvs_storage_size_mb = 64;    // smaller than default 128
    config.kvs_retention_hours = 24;    // 1 day instead of 7

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, RebuildWithDifferentKvsConfig) {
    // Build with KVS enabled, destroy, rebuild with KVS disabled
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = true;
    config.kvs_stream_name = "first-stream";
    set_iot_fields(config);

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->destroy().is_ok());

    // Rebuild with KVS disabled
    config.kvs_enabled = false;
    config.kvs_stream_name = "";
    clear_iot_fields(config);

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, BuildWithKvsEnabled_CustomBufferDuration) {
    // Verify custom kvs_buffer_duration_ms is accepted
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = true;
    config.kvs_stream_name = "buffered-stream";
    config.kvs_buffer_duration_ms = 5000;  // 5 seconds instead of default 2

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

// ============================================================
// Start/Stop lifecycle stress tests
// These exercise the start() → stop() → start() path that
// involves GMainLoop thread creation/teardown on real GStreamer.
// On stub, they verify state machine consistency under rapid cycling.
// ============================================================

TEST(GStreamerPipeline, RapidStartStopCycles) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());

    // Rapid start/stop cycles — exercises bus_thread_ join + re-creation
    for (int i = 0; i < 10; ++i) {
        auto start_result = pipeline->start();
        ASSERT_TRUE(start_result.is_ok())
            << "start() failed on cycle " << i;
        EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

        auto stop_result = pipeline->stop();
        ASSERT_TRUE(stop_result.is_ok())
            << "stop() failed on cycle " << i;
        EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);
    }

    // Final cleanup
    EXPECT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

TEST(GStreamerPipeline, StartStopWithBitrateChangeBetween) {
    // Verify bitrate can be changed while paused (between start/stop),
    // then pipeline restarts cleanly — exercises the full thread lifecycle
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    ASSERT_TRUE(pipeline->stop().is_ok());

    // Change bitrate while paused
    ASSERT_TRUE(pipeline->set_bitrate(512).is_ok());

    // Restart — on real GStreamer this creates a new GMainLoop + bus_thread_
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    ASSERT_TRUE(pipeline->destroy().is_ok());
}

TEST(GStreamerPipeline, DestroyFromPlayingThenRebuildAndStart) {
    // Destroy while PLAYING (skipping stop), then rebuild and start again.
    // On real GStreamer, destroy must quit the GMainLoop and join bus_thread_
    // before releasing pipeline resources.
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    // Destroy directly from PLAYING — no stop() first
    ASSERT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
    EXPECT_TRUE(pipeline->encoder_name().empty());

    // Rebuild and start fresh — verifies clean teardown
    config.video_preset = PRESET_HD;
    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);
    EXPECT_FALSE(pipeline->encoder_name().empty());

    ASSERT_TRUE(pipeline->destroy().is_ok());
}

TEST(GStreamerPipeline, ConcurrentStateReadsWhileStarting) {
    // Read state from multiple threads while start() is in progress.
    // On real GStreamer, start() holds the mutex while creating the
    // GMainLoop and setting PLAYING — readers must not deadlock.
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());

    std::atomic<bool> stop_readers{false};
    std::vector<std::thread> readers;

    // Start reader threads before calling start()
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&pipeline, &stop_readers]() {
            while (!stop_readers.load(std::memory_order_relaxed)) {
                auto state = pipeline->current_state();
                // State should always be a valid enum value
                EXPECT_TRUE(
                    state == IGStreamerPipeline::State::READY ||
                    state == IGStreamerPipeline::State::PLAYING ||
                    state == IGStreamerPipeline::State::PAUSED ||
                    state == IGStreamerPipeline::State::NULL_STATE);
                auto enc = pipeline->encoder_name();
                (void)enc;
            }
        });
    }

    // Start and stop while readers are active
    ASSERT_TRUE(pipeline->start().is_ok());
    ASSERT_TRUE(pipeline->stop().is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());

    stop_readers.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    ASSERT_TRUE(pipeline->destroy().is_ok());
}

TEST(GStreamerPipeline, StopFromPausedIsIdempotent) {
    // Calling stop() when already PAUSED — on real GStreamer this should
    // not try to quit/join a GMainLoop that was already torn down.
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    ASSERT_TRUE(pipeline->stop().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    // Second stop from PAUSED — should succeed without hanging
    auto result = pipeline->stop();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    ASSERT_TRUE(pipeline->destroy().is_ok());
}

TEST(GStreamerPipeline, StartWithKvsConfig_FullCycle) {
    // Full lifecycle with KVS config — on real GStreamer, start() must
    // create the GMainLoop before set_state(PLAYING) so kvssink can
    // dispatch async credential callbacks during the state transition.
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = true;
    config.kvs_stream_name = "start-test-stream";
    set_iot_fields(config);

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    // Dynamic bitrate while playing
    ASSERT_TRUE(pipeline->set_bitrate(1024).is_ok());

    ASSERT_TRUE(pipeline->stop().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    // Restart
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    ASSERT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

// ============================================================
// Additional edge-case tests for coverage gaps
// ============================================================

// stop() from READY state — stub transitions to PAUSED
TEST(GStreamerPipeline, StopFromReadySucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);

    auto result = pipeline->stop();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);
}

// build() called again while already in READY state (no destroy first)
// The stub overwrites state — verifies no crash or resource leak
TEST(GStreamerPipeline, DoubleBuildWithoutDestroy) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);

    // Second build with different preset — should succeed
    config.video_preset = PRESET_HD;
    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
    EXPECT_FALSE(pipeline->encoder_name().empty());
}

// destroy() from PAUSED state
TEST(GStreamerPipeline, DestroyFromPausedSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    ASSERT_TRUE(pipeline->stop().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    auto result = pipeline->destroy();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
    EXPECT_TRUE(pipeline->encoder_name().empty());
}

// set_bitrate(0) — zero bitrate edge case
TEST(GStreamerPipeline, SetBitrateZeroSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    auto result = pipeline->set_bitrate(0);
    EXPECT_TRUE(result.is_ok());
}

// set_bitrate with very large value
TEST(GStreamerPipeline, SetBitrateLargeValueSucceeds) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    auto result = pipeline->set_bitrate(100000);
    EXPECT_TRUE(result.is_ok());
}

// build() stores config — verify encoder_name reflects stub
TEST(GStreamerPipeline, BuildStoresConfigAndSetsStubEncoder) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_LOW_BW;
    config.prefer_hw_encoder = false;

    ASSERT_TRUE(pipeline->build(config).is_ok());
#ifndef HAS_GSTREAMER
    EXPECT_EQ(pipeline->encoder_name(), "stub");
#endif
}

// Multiple destroy calls are idempotent
TEST(GStreamerPipeline, MultipleDestroysAreIdempotent) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());

    EXPECT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);

    // Second and third destroy should also succeed
    EXPECT_TRUE(pipeline->destroy().is_ok());
    EXPECT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

// Full lifecycle with all camera source types in config
TEST(GStreamerPipeline, BuildWithV4L2SourceType) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::V4L2_USB;
    config.device_path = "/dev/video0";
    config.video_preset = PRESET_DEFAULT;

    // Stub mode: build always succeeds regardless of source type
    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

TEST(GStreamerPipeline, BuildWithLibcameraSourceType) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::LIBCAMERA_CSI;
    config.video_preset = PRESET_DEFAULT;

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

// Concurrent build + state reads — verify no data race
TEST(GStreamerPipeline, ConcurrentBuildAndStateReads) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    std::atomic<bool> stop_readers{false};
    std::vector<std::thread> readers;

    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&pipeline, &stop_readers]() {
            while (!stop_readers.load(std::memory_order_relaxed)) {
                auto state = pipeline->current_state();
                EXPECT_TRUE(
                    state == IGStreamerPipeline::State::NULL_STATE ||
                    state == IGStreamerPipeline::State::READY ||
                    state == IGStreamerPipeline::State::PLAYING ||
                    state == IGStreamerPipeline::State::PAUSED);
                auto enc = pipeline->encoder_name();
                (void)enc;
            }
        });
    }

    // Build, start, stop, destroy while readers are active
    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    ASSERT_TRUE(pipeline->stop().is_ok());
    ASSERT_TRUE(pipeline->destroy().is_ok());

    stop_readers.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

// Start from READY after stop (READY → start → stop → start)
// Verifies the PAUSED → PLAYING transition works after a stop
TEST(GStreamerPipeline, StartAfterStopFromReady) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    // stop from READY → PAUSED
    ASSERT_TRUE(pipeline->stop().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    // start from PAUSED → PLAYING
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    ASSERT_TRUE(pipeline->destroy().is_ok());
}

// Build with custom GOP size
TEST(GStreamerPipeline, BuildWithCustomGopSize) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.gop_size = 60;  // Custom GOP instead of matching fps

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

// Build with custom WebRTC queue size
TEST(GStreamerPipeline, BuildWithCustomWebrtcQueueSize) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.webrtc_queue_size = 10;

    auto result = pipeline->build(config);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::READY);
}

// ============================================================
// Appsink emit-signals=false verification
// On real GStreamer builds, verify that the webrtc_sink and
// ai_sink appsinks have emit-signals=false. This prevents
// new-sample signals from queuing in the GMainContext when
// no consumer is pulling samples yet.
// Validates: Pipeline description correctness after emit-signals fix
// ============================================================

#ifdef HAS_GSTREAMER
TEST(GStreamerPipeline, AppsinkEmitSignalsDisabled) {
    // This test verifies that the pipeline description sets emit-signals=false
    // on appsinks. We verify this indirectly: if the pipeline reaches PLAYING
    // state (ret=1 SUCCESS, not ASYNC), it means appsinks aren't blocking
    // the state transition with unhandled signals.
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);
    pipeline->destroy();

    ASSERT_TRUE(pipeline->destroy().is_ok());
}
#endif

// ============================================================
// Bus thread lifecycle tests
// These tests target the GMainLoop + bus_thread_ lifecycle
// introduced in start(). On the stub, they verify state machine
// correctness. On real GStreamer, they exercise the thread
// create/join/cleanup paths in start(), stop(), and destroy().
// ============================================================

// Verify that stop() after start() leaves pipeline in a state
// where it can be started again — exercises bus_thread_ join
// and GMainLoop teardown/recreation on real GStreamer.
TEST(GStreamerPipeline, BusThread_StopThenRestartCreatesNewLoop) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());

    // First cycle: start creates loop+thread, stop joins thread
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);
    ASSERT_TRUE(pipeline->stop().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    // Second cycle: start must create a NEW loop+thread (old was joined)
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);
    ASSERT_TRUE(pipeline->stop().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    // Third cycle: verify no resource leak from repeated create/join
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    ASSERT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

// destroy() while PLAYING must quit the GMainLoop and join
// bus_thread_ before freeing the pipeline. Verify no hang.
TEST(GStreamerPipeline, BusThread_DestroyWhilePlayingCleansUpThread) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

    // destroy() directly from PLAYING — must handle loop+thread cleanup
    ASSERT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);

    // Verify pipeline is fully reusable after destroy-from-playing
    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);
    ASSERT_TRUE(pipeline->destroy().is_ok());
}

// Rapid start/stop exercises bus_thread_ join + recreation.
// On real GStreamer, each start() creates a GMainLoop + thread,
// and each stop() must quit + join before the next start().
TEST(GStreamerPipeline, BusThread_RapidStartStopNoResourceLeak) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = true;
    config.kvs_stream_name = "bus-thread-test";
    set_iot_fields(config);

    ASSERT_TRUE(pipeline->build(config).is_ok());

    // 20 rapid cycles with KVS config — stresses the bus thread lifecycle
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(pipeline->start().is_ok())
            << "start() failed on cycle " << i;
        ASSERT_TRUE(pipeline->stop().is_ok())
            << "stop() failed on cycle " << i;
    }

    ASSERT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

// stop() when already PAUSED should not try to quit/join a
// GMainLoop that was already torn down in the previous stop().
TEST(GStreamerPipeline, BusThread_DoubleStopDoesNotDoubleJoin) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());

    // First stop: quits loop, joins thread
    ASSERT_TRUE(pipeline->stop().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    // Second stop: loop_ is already null, thread already joined
    // Must not crash or hang
    auto result = pipeline->stop();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PAUSED);

    ASSERT_TRUE(pipeline->destroy().is_ok());
}

// destroy() after stop() — loop_ already cleaned up by stop(),
// destroy() should handle the null loop_ gracefully.
TEST(GStreamerPipeline, BusThread_DestroyAfterStopHandlesNullLoop) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    ASSERT_TRUE(pipeline->stop().is_ok());

    // stop() already cleaned up loop_ and bus_thread_
    // destroy() must not double-free or crash
    ASSERT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}

// Concurrent state reads while start() is creating the bus thread.
// On real GStreamer, start() releases the lock before set_state()
// and reacquires after — readers must see consistent state.
TEST(GStreamerPipeline, BusThread_ConcurrentReadsWhileStarting) {
    auto pipeline = create_gstreamer_pipeline();
    PipelineConfig config;
    config.source_type = CameraSourceType::VIDEOTESTSRC;
    config.video_preset = PRESET_DEFAULT;
    config.kvs_enabled = true;
    config.kvs_stream_name = "concurrent-test";

    ASSERT_TRUE(pipeline->build(config).is_ok());

    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> readers;

    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&pipeline, &stop_flag]() {
            while (!stop_flag.load(std::memory_order_relaxed)) {
                auto state = pipeline->current_state();
                EXPECT_TRUE(
                    state == IGStreamerPipeline::State::READY ||
                    state == IGStreamerPipeline::State::PLAYING ||
                    state == IGStreamerPipeline::State::PAUSED ||
                    state == IGStreamerPipeline::State::NULL_STATE);
            }
        });
    }

    // Multiple start/stop cycles while readers are active
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(pipeline->start().is_ok());
        ASSERT_TRUE(pipeline->stop().is_ok());
    }

    stop_flag.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    ASSERT_TRUE(pipeline->destroy().is_ok());
}

// Destructor must clean up even if pipeline is still PLAYING.
// On real GStreamer, the destructor should handle loop+thread
// cleanup (or delegate to destroy()). This test verifies no hang.
TEST(GStreamerPipeline, BusThread_DestructorCleansUpFromPlaying) {
    {
        auto pipeline = create_gstreamer_pipeline();
        PipelineConfig config;
        config.source_type = CameraSourceType::VIDEOTESTSRC;
        config.video_preset = PRESET_DEFAULT;

        ASSERT_TRUE(pipeline->build(config).is_ok());
        ASSERT_TRUE(pipeline->start().is_ok());
        EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::PLAYING);

        // pipeline goes out of scope here — destructor runs
        // On real GStreamer, must quit loop + join thread + unref
    }
    // If we reach here without hanging, cleanup succeeded
    SUCCEED();
}
