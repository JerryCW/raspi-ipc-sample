#include <gtest/gtest.h>
#include "camera/camera_source.h"

#include <string>

using namespace sc;

// ============================================================
// Factory: create_camera_source
// ============================================================

TEST(CameraSourceFactory, CreateVideoTestSrcSucceeds) {
    auto result = create_camera_source(CameraSourceType::VIDEOTESTSRC);
    ASSERT_TRUE(result.is_ok());
    EXPECT_NE(result.value(), nullptr);
}

#ifndef HAS_LIBCAMERA
TEST(CameraSourceFactory, CreateLibcameraReturnsErrorWithoutSupport) {
    auto result = create_camera_source(CameraSourceType::LIBCAMERA_CSI);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::CameraDeviceNotFound);
    EXPECT_NE(result.error().message.find("libcamera"), std::string::npos);
}
#endif

#ifndef HAS_V4L2
TEST(CameraSourceFactory, CreateV4L2ReturnsErrorWithoutSupport) {
    auto result = create_camera_source(CameraSourceType::V4L2_USB, "/dev/video0");
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::CameraDeviceNotFound);
    EXPECT_NE(result.error().message.find("V4L2"), std::string::npos);
}
#endif

// ============================================================
// VideoTestSrc: type()
// ============================================================

TEST(VideoTestSrc, TypeReturnsVideoTestSrc) {
    auto result = create_camera_source(CameraSourceType::VIDEOTESTSRC);
    ASSERT_TRUE(result.is_ok());
    auto& src = result.value();
    EXPECT_EQ(src->type(), CameraSourceType::VIDEOTESTSRC);
}

// ============================================================
// VideoTestSrc: open()
// ============================================================

TEST(VideoTestSrc, OpenSucceedsWithValidPreset) {
    auto result = create_camera_source(CameraSourceType::VIDEOTESTSRC);
    ASSERT_TRUE(result.is_ok());
    auto& src = result.value();

    auto open_result = src->open(PRESET_DEFAULT);
    EXPECT_TRUE(open_result.is_ok());
}

// ============================================================
// VideoTestSrc: capture_frame() fails when not opened
// ============================================================

TEST(VideoTestSrc, CaptureFrameFailsWhenNotOpened) {
    auto result = create_camera_source(CameraSourceType::VIDEOTESTSRC);
    ASSERT_TRUE(result.is_ok());
    auto& src = result.value();

    auto frame_result = src->capture_frame();
    ASSERT_TRUE(frame_result.is_err());
    EXPECT_EQ(frame_result.error().code, ErrorCode::CameraOpenFailed);
}

// ============================================================
// VideoTestSrc: capture_frame() returns valid frame
// ============================================================

TEST(VideoTestSrc, CaptureFrameReturnsValidFrame) {
    auto result = create_camera_source(CameraSourceType::VIDEOTESTSRC);
    ASSERT_TRUE(result.is_ok());
    auto& src = result.value();

    src->open(PRESET_DEFAULT);
    auto frame_result = src->capture_frame();
    ASSERT_TRUE(frame_result.is_ok());

    const auto& frame = frame_result.value();
    EXPECT_EQ(frame.info.width, PRESET_DEFAULT.width);
    EXPECT_EQ(frame.info.height, PRESET_DEFAULT.height);
    EXPECT_EQ(frame.info.stride, PRESET_DEFAULT.width);
    EXPECT_FALSE(frame.data.empty());
}

// ============================================================
// VideoTestSrc: NV12 frame size is correct (w*h*3/2)
// ============================================================

TEST(VideoTestSrc, NV12FrameSizeIsCorrect) {
    auto result = create_camera_source(CameraSourceType::VIDEOTESTSRC);
    ASSERT_TRUE(result.is_ok());
    auto& src = result.value();

    src->open(PRESET_DEFAULT);
    auto frame_result = src->capture_frame();
    ASSERT_TRUE(frame_result.is_ok());

    const auto& frame = frame_result.value();
    size_t expected_size = static_cast<size_t>(PRESET_DEFAULT.width)
                         * PRESET_DEFAULT.height * 3 / 2;
    EXPECT_EQ(frame.data.size(), expected_size);
}

// ============================================================
// VideoTestSrc: sequence numbers increment
// ============================================================

TEST(VideoTestSrc, SequenceNumbersIncrement) {
    auto result = create_camera_source(CameraSourceType::VIDEOTESTSRC);
    ASSERT_TRUE(result.is_ok());
    auto& src = result.value();

    src->open(PRESET_LOW_BW);

    auto f0 = src->capture_frame();
    auto f1 = src->capture_frame();
    auto f2 = src->capture_frame();
    ASSERT_TRUE(f0.is_ok());
    ASSERT_TRUE(f1.is_ok());
    ASSERT_TRUE(f2.is_ok());

    EXPECT_EQ(f0.value().info.sequence_number, 0u);
    EXPECT_EQ(f1.value().info.sequence_number, 1u);
    EXPECT_EQ(f2.value().info.sequence_number, 2u);
}

// ============================================================
// VideoTestSrc: query_capabilities()
// ============================================================

TEST(VideoTestSrc, QueryCapabilitiesReturnsSupportedResolutions) {
    auto result = create_camera_source(CameraSourceType::VIDEOTESTSRC);
    ASSERT_TRUE(result.is_ok());
    auto& src = result.value();

    auto caps_result = src->query_capabilities();
    ASSERT_TRUE(caps_result.is_ok());

    const auto& caps = caps_result.value();
    EXPECT_FALSE(caps.supported_resolutions.empty());
    EXPECT_GE(caps.max_fps, caps.min_fps);
    EXPECT_EQ(caps.pixel_format, "NV12");
}

// ============================================================
// VideoTestSrc: gst_source_description() contains "videotestsrc"
// ============================================================

TEST(VideoTestSrc, GstSourceDescriptionContainsVideoTestSrc) {
    auto result = create_camera_source(CameraSourceType::VIDEOTESTSRC);
    ASSERT_TRUE(result.is_ok());
    auto& src = result.value();

    std::string desc = src->gst_source_description();
    EXPECT_NE(desc.find("videotestsrc"), std::string::npos);
}

// ============================================================
// VideoTestSrc: close() and reopen works
// ============================================================

TEST(VideoTestSrc, CloseAndReopenWorks) {
    auto result = create_camera_source(CameraSourceType::VIDEOTESTSRC);
    ASSERT_TRUE(result.is_ok());
    auto& src = result.value();

    // Open, capture, close
    ASSERT_TRUE(src->open(PRESET_DEFAULT).is_ok());
    ASSERT_TRUE(src->capture_frame().is_ok());
    ASSERT_TRUE(src->close().is_ok());

    // Capture should fail after close
    EXPECT_TRUE(src->capture_frame().is_err());

    // Reopen with different preset and capture again
    ASSERT_TRUE(src->open(PRESET_HD).is_ok());
    auto frame_result = src->capture_frame();
    ASSERT_TRUE(frame_result.is_ok());

    // Verify new preset dimensions
    EXPECT_EQ(frame_result.value().info.width, PRESET_HD.width);
    EXPECT_EQ(frame_result.value().info.height, PRESET_HD.height);

    // Sequence should reset after reopen
    EXPECT_EQ(frame_result.value().info.sequence_number, 0u);
}
