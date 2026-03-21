#include <gtest/gtest.h>
#include "pipeline/gstreamer_pipeline.h"

#include <string>
#include <thread>
#include <vector>

using namespace sc;

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
    EXPECT_TRUE(config.kvs_iot_certificate.empty());
    EXPECT_EQ(config.kvs_storage_size_mb, 128u);
    EXPECT_EQ(config.kvs_retention_hours, 168u);
    EXPECT_FALSE(config.kvs_enabled);
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
    config.kvs_iot_certificate = "iot-thing-name=cam,endpoint=ep,cert-path=/c,key-path=/k,ca-path=/ca,role-aliases=role";
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
    config.kvs_iot_certificate = "iot-thing-name=cam,endpoint=ep,cert-path=/c,key-path=/k,ca-path=/ca,role-aliases=role";

    ASSERT_TRUE(pipeline->build(config).is_ok());
    ASSERT_TRUE(pipeline->start().is_ok());
    ASSERT_TRUE(pipeline->set_bitrate(1024).is_ok());
    ASSERT_TRUE(pipeline->stop().is_ok());
    ASSERT_TRUE(pipeline->destroy().is_ok());
    EXPECT_EQ(pipeline->current_state(), IGStreamerPipeline::State::NULL_STATE);
}
