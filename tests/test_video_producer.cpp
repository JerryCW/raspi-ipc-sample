#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include "pipeline/video_producer.h"

namespace sc {
namespace {

// ============================================================
// Helper: build a valid H.264 NAL unit with 4-byte start code
// ============================================================

static std::vector<uint8_t> make_nal(NalType type, size_t payload_size = 10) {
    std::vector<uint8_t> data;
    // 4-byte start code
    data.push_back(0x00);
    data.push_back(0x00);
    data.push_back(0x00);
    data.push_back(0x01);
    // NAL header: forbidden_zero_bit=0, nal_ref_idc=3, nal_unit_type
    uint8_t header = 0x60 | static_cast<uint8_t>(type);  // nal_ref_idc = 3 (0b011)
    data.push_back(header);
    // Payload
    for (size_t i = 0; i < payload_size; ++i) {
        data.push_back(static_cast<uint8_t>(i & 0xFF));
    }
    return data;
}

// Helper: build an IDR frame (SPS + PPS + IDR)
static std::vector<uint8_t> make_idr_frame() {
    auto sps = make_nal(NalType::SPS, 5);
    auto pps = make_nal(NalType::PPS, 3);
    auto idr = make_nal(NalType::IDR, 20);
    std::vector<uint8_t> frame;
    frame.insert(frame.end(), sps.begin(), sps.end());
    frame.insert(frame.end(), pps.begin(), pps.end());
    frame.insert(frame.end(), idr.begin(), idr.end());
    return frame;
}

// Helper: build a non-IDR frame
static std::vector<uint8_t> make_non_idr_frame() {
    return make_nal(NalType::NON_IDR, 20);
}

// Helper: build an EncodedFrame from raw data
static EncodedFrame make_encoded(std::vector<uint8_t> data, uint64_t seq = 1) {
    EncodedFrame f;
    f.data = std::move(data);
    f.sequence_number = seq;
    f.timestamp = std::chrono::steady_clock::now();
    return f;
}

// ============================================================
// NAL unit validation tests
// Validates: Requirements 20.1
// ============================================================

TEST(VideoProducerTest, ValidNalUnit_PassesThrough) {
    VideoProducer vp;
    auto frame = make_encoded(make_non_idr_frame());
    auto result = vp.process_frame(frame);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().sequence_number, 1u);
}

TEST(VideoProducerTest, ValidIdrFrame_PassesThrough) {
    VideoProducer vp;
    auto frame = make_encoded(make_idr_frame());
    auto result = vp.process_frame(frame);
    ASSERT_TRUE(result.is_ok());
}

TEST(VideoProducerTest, EmptyFrame_Rejected) {
    VideoProducer vp;
    auto frame = make_encoded({});
    auto result = vp.process_frame(frame);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::NALUnitInvalid);
}

TEST(VideoProducerTest, NoStartCode_Rejected) {
    VideoProducer vp;
    // Random bytes without a start code
    std::vector<uint8_t> bad_data = {0x65, 0x88, 0x04, 0x00, 0x12, 0x34, 0x56, 0x78};
    auto frame = make_encoded(bad_data);
    auto result = vp.process_frame(frame);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::NALUnitInvalid);
}

TEST(VideoProducerTest, TruncatedNal_StartCodeOnly_Rejected) {
    VideoProducer vp;
    // Just a start code with no NAL header
    std::vector<uint8_t> truncated = {0x00, 0x00, 0x00, 0x01};
    auto frame = make_encoded(truncated);
    auto result = vp.process_frame(frame);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::NALUnitInvalid);
}

TEST(VideoProducerTest, ForbiddenBitSet_Rejected) {
    VideoProducer vp;
    // Valid start code but forbidden_zero_bit = 1
    std::vector<uint8_t> bad_header = {0x00, 0x00, 0x00, 0x01, 0x85, 0x00, 0x00};
    auto frame = make_encoded(bad_header);
    auto result = vp.process_frame(frame);
    ASSERT_TRUE(result.is_err());
}

TEST(VideoProducerTest, NalTypeZero_Rejected) {
    VideoProducer vp;
    // NAL type 0 is unspecified/invalid
    std::vector<uint8_t> type_zero = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    auto frame = make_encoded(type_zero);
    auto result = vp.process_frame(frame);
    ASSERT_TRUE(result.is_err());
}

TEST(VideoProducerTest, ThreeByteStartCode_Valid) {
    VideoProducer vp;
    // 3-byte start code: 00 00 01
    std::vector<uint8_t> data = {0x00, 0x00, 0x01, 0x65, 0x88, 0x04, 0x00};
    auto frame = make_encoded(data);
    auto result = vp.process_frame(frame);
    ASSERT_TRUE(result.is_ok());
}

TEST(VideoProducerTest, MultipleNalUnits_AllValid) {
    VideoProducer vp;
    auto frame = make_encoded(make_idr_frame());  // SPS + PPS + IDR
    auto result = vp.process_frame(frame);
    ASSERT_TRUE(result.is_ok());
}

TEST(VideoProducerTest, TooShortData_Rejected) {
    VideoProducer vp;
    // Less than minimum NAL size
    std::vector<uint8_t> tiny = {0x00, 0x00, 0x01};
    auto frame = make_encoded(tiny);
    auto result = vp.process_frame(frame);
    ASSERT_TRUE(result.is_err());
}

// ============================================================
// Force IDR after drop tests
// Validates: Requirements 20.4
// ============================================================

TEST(VideoProducerTest, ForceIdr_SetAfterNalDrop) {
    VideoProducer vp;
    // Drop an invalid frame
    auto bad = make_encoded({0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    vp.process_frame(bad);

    EXPECT_TRUE(vp.force_idr_required());
}

TEST(VideoProducerTest, ForceIdr_NonIdrDroppedAfterPreviousDrop) {
    VideoProducer vp;
    // First: drop an invalid frame to set force_idr
    auto bad = make_encoded({0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    vp.process_frame(bad);
    ASSERT_TRUE(vp.force_idr_required());

    // Second: valid non-IDR frame should be dropped (waiting for IDR)
    auto non_idr = make_encoded(make_non_idr_frame());
    auto result = vp.process_frame(non_idr);
    EXPECT_TRUE(result.is_err());
    EXPECT_TRUE(vp.force_idr_required());
}

TEST(VideoProducerTest, ForceIdr_ClearedWhenIdrSent) {
    VideoProducer vp;
    // Drop an invalid frame to set force_idr
    auto bad = make_encoded({0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    vp.process_frame(bad);
    ASSERT_TRUE(vp.force_idr_required());

    // Send an IDR frame — should pass and clear force_idr
    auto idr = make_encoded(make_idr_frame());
    auto result = vp.process_frame(idr);
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(vp.force_idr_required());
}

TEST(VideoProducerTest, ForceIdr_SetAfterIceDrop) {
    VideoProducer vp;
    vp.set_ice_negotiating(true);

    auto frame = make_encoded(make_non_idr_frame());
    vp.process_frame(frame);

    EXPECT_TRUE(vp.force_idr_required());

    vp.set_ice_negotiating(false);
}

// ============================================================
// ICE negotiating tests
// Validates: Requirements 20.3
// ============================================================

TEST(VideoProducerTest, IceNegotiating_DropsFrames) {
    VideoProducer vp;
    vp.set_ice_negotiating(true);

    auto frame = make_encoded(make_idr_frame());
    auto result = vp.process_frame(frame);
    EXPECT_TRUE(result.is_err());

    vp.set_ice_negotiating(false);
}

TEST(VideoProducerTest, IceNegotiating_PassesAfterDone) {
    VideoProducer vp;
    vp.set_ice_negotiating(true);

    // Drop during negotiation
    auto frame1 = make_encoded(make_idr_frame(), 1);
    EXPECT_TRUE(vp.process_frame(frame1).is_err());

    // Negotiation complete
    vp.set_ice_negotiating(false);

    // Next IDR frame should pass (force_idr was set by the drop)
    auto frame2 = make_encoded(make_idr_frame(), 2);
    auto result = vp.process_frame(frame2);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().sequence_number, 2u);
}

TEST(VideoProducerTest, IceNegotiating_DefaultFalse) {
    VideoProducer vp;
    EXPECT_FALSE(vp.is_ice_negotiating());
}

// ============================================================
// Drop statistics tests
// Validates: Requirements 20.5
// ============================================================

TEST(VideoProducerTest, DropStats_InitiallyZero) {
    VideoProducer vp;
    auto stats = vp.drop_stats();
    EXPECT_EQ(stats.total_dropped, 0u);
    EXPECT_EQ(stats.nal_invalid, 0u);
    EXPECT_EQ(stats.ice_negotiating, 0u);
}

TEST(VideoProducerTest, DropStats_TracksNalInvalid) {
    VideoProducer vp;
    auto bad = make_encoded({0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    vp.process_frame(bad);
    vp.process_frame(bad);

    auto stats = vp.drop_stats();
    EXPECT_EQ(stats.nal_invalid, 2u);
    EXPECT_EQ(stats.total_dropped, 2u);
}

TEST(VideoProducerTest, DropStats_TracksIceNegotiating) {
    VideoProducer vp;
    vp.set_ice_negotiating(true);

    auto frame = make_encoded(make_idr_frame());
    vp.process_frame(frame);
    vp.process_frame(frame);
    vp.process_frame(frame);

    auto stats = vp.drop_stats();
    EXPECT_EQ(stats.ice_negotiating, 3u);
    EXPECT_GE(stats.total_dropped, 3u);

    vp.set_ice_negotiating(false);
}

TEST(VideoProducerTest, DropStats_ResetClearsAll) {
    VideoProducer vp;
    auto bad = make_encoded({0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    vp.process_frame(bad);

    vp.reset_stats();
    auto stats = vp.drop_stats();
    EXPECT_EQ(stats.total_dropped, 0u);
    EXPECT_EQ(stats.nal_invalid, 0u);
}

TEST(VideoProducerTest, DropStats_WindowStartSet) {
    VideoProducer vp;
    auto before = std::chrono::steady_clock::now();
    vp.reset_stats();
    auto after = std::chrono::steady_clock::now();

    auto stats = vp.drop_stats();
    EXPECT_GE(stats.window_start, before);
    EXPECT_LE(stats.window_start, after);
}

// ============================================================
// Static validation utility tests
// ============================================================

TEST(VideoProducerTest, ValidateNal_NullData_ReturnsFalse) {
    EXPECT_FALSE(VideoProducer::validate_nal_units(nullptr, 10));
}

TEST(VideoProducerTest, ValidateNal_ZeroSize_ReturnsFalse) {
    uint8_t data[] = {0x00};
    EXPECT_FALSE(VideoProducer::validate_nal_units(data, 0));
}

TEST(VideoProducerTest, IsIdrFrame_DetectsIdr) {
    auto idr_data = make_idr_frame();
    EXPECT_TRUE(VideoProducer::is_idr_frame(idr_data.data(), idr_data.size()));
}

TEST(VideoProducerTest, IsIdrFrame_NonIdr_ReturnsFalse) {
    auto non_idr = make_non_idr_frame();
    EXPECT_FALSE(VideoProducer::is_idr_frame(non_idr.data(), non_idr.size()));
}

}  // namespace
}  // namespace sc
