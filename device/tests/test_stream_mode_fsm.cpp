#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "core/stream_mode.h"
#include "core/stream_mode_fsm.h"

namespace sc {
namespace {

// ============================================================
// Tests: Initial mode defaults to DEGRADED
// Validates: Requirements 1.5
// ============================================================

TEST(StreamModeFSMTest, DefaultConstructor_InitialModeDegraded) {
    StreamModeFSM fsm;
    EXPECT_EQ(fsm.current_mode(), StreamMode::DEGRADED);
}

TEST(StreamModeFSMTest, ExplicitConstructor_SetsInitialMode) {
    StreamModeFSM fsm(StreamMode::FULL);
    EXPECT_EQ(fsm.current_mode(), StreamMode::FULL);
}

// ============================================================
// Tests: FULL → KVS_ONLY (WebRTC goes offline)
// Validates: Requirements 1.3
// ============================================================

TEST(StreamModeFSMTest, Full_To_KvsOnly_WebrtcOffline) {
    StreamModeFSM fsm(StreamMode::FULL);

    auto result = fsm.evaluate(/*kvs=*/true, /*webrtc=*/false);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), StreamMode::KVS_ONLY);
    EXPECT_EQ(fsm.current_mode(), StreamMode::KVS_ONLY);
}

// ============================================================
// Tests: FULL → WEBRTC_ONLY (KVS goes offline)
// Validates: Requirements 1.4
// ============================================================

TEST(StreamModeFSMTest, Full_To_WebrtcOnly_KvsOffline) {
    StreamModeFSM fsm(StreamMode::FULL);

    auto result = fsm.evaluate(/*kvs=*/false, /*webrtc=*/true);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), StreamMode::WEBRTC_ONLY);
    EXPECT_EQ(fsm.current_mode(), StreamMode::WEBRTC_ONLY);
}

// ============================================================
// Tests: FULL → DEGRADED (both offline)
// Validates: Requirements 1.5
// ============================================================

TEST(StreamModeFSMTest, Full_To_Degraded_BothOffline) {
    StreamModeFSM fsm(StreamMode::FULL);

    auto result = fsm.evaluate(/*kvs=*/false, /*webrtc=*/false);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), StreamMode::DEGRADED);
    EXPECT_EQ(fsm.current_mode(), StreamMode::DEGRADED);
}

// ============================================================
// Tests: KVS_ONLY → FULL (WebRTC recovers)
// Validates: Requirements 1.1, 1.6
// ============================================================

TEST(StreamModeFSMTest, KvsOnly_To_Full_WebrtcRecovers) {
    StreamModeFSM fsm(StreamMode::KVS_ONLY);

    auto result = fsm.evaluate(/*kvs=*/true, /*webrtc=*/true);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), StreamMode::FULL);
    EXPECT_EQ(fsm.current_mode(), StreamMode::FULL);
}

// ============================================================
// Tests: KVS_ONLY → DEGRADED (KVS also goes offline)
// Validates: Requirements 1.5
// ============================================================

TEST(StreamModeFSMTest, KvsOnly_To_Degraded_KvsOffline) {
    StreamModeFSM fsm(StreamMode::KVS_ONLY);

    auto result = fsm.evaluate(/*kvs=*/false, /*webrtc=*/false);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), StreamMode::DEGRADED);
    EXPECT_EQ(fsm.current_mode(), StreamMode::DEGRADED);
}

// ============================================================
// Tests: WEBRTC_ONLY → FULL (KVS recovers)
// Validates: Requirements 1.1, 1.6
// ============================================================

TEST(StreamModeFSMTest, WebrtcOnly_To_Full_KvsRecovers) {
    StreamModeFSM fsm(StreamMode::WEBRTC_ONLY);

    auto result = fsm.evaluate(/*kvs=*/true, /*webrtc=*/true);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), StreamMode::FULL);
    EXPECT_EQ(fsm.current_mode(), StreamMode::FULL);
}

// ============================================================
// Tests: WEBRTC_ONLY → DEGRADED (WebRTC also goes offline)
// Validates: Requirements 1.5
// ============================================================

TEST(StreamModeFSMTest, WebrtcOnly_To_Degraded_WebrtcOffline) {
    StreamModeFSM fsm(StreamMode::WEBRTC_ONLY);

    auto result = fsm.evaluate(/*kvs=*/false, /*webrtc=*/false);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), StreamMode::DEGRADED);
    EXPECT_EQ(fsm.current_mode(), StreamMode::DEGRADED);
}

// ============================================================
// Tests: DEGRADED → KVS_ONLY (KVS recovers)
// Validates: Requirements 1.4, 1.6
// ============================================================

TEST(StreamModeFSMTest, Degraded_To_KvsOnly_KvsRecovers) {
    StreamModeFSM fsm(StreamMode::DEGRADED);

    auto result = fsm.evaluate(/*kvs=*/true, /*webrtc=*/false);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), StreamMode::KVS_ONLY);
    EXPECT_EQ(fsm.current_mode(), StreamMode::KVS_ONLY);
}

// ============================================================
// Tests: DEGRADED → WEBRTC_ONLY (WebRTC recovers)
// Validates: Requirements 1.3, 1.6
// ============================================================

TEST(StreamModeFSMTest, Degraded_To_WebrtcOnly_WebrtcRecovers) {
    StreamModeFSM fsm(StreamMode::DEGRADED);

    auto result = fsm.evaluate(/*kvs=*/false, /*webrtc=*/true);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), StreamMode::WEBRTC_ONLY);
    EXPECT_EQ(fsm.current_mode(), StreamMode::WEBRTC_ONLY);
}

// ============================================================
// Tests: DEGRADED → FULL (both recover)
// Validates: Requirements 1.1, 1.6
// ============================================================

TEST(StreamModeFSMTest, Degraded_To_Full_BothRecover) {
    StreamModeFSM fsm(StreamMode::DEGRADED);

    auto result = fsm.evaluate(/*kvs=*/true, /*webrtc=*/true);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), StreamMode::FULL);
    EXPECT_EQ(fsm.current_mode(), StreamMode::FULL);
}

// ============================================================
// Tests: No transition when mode doesn't change
// ============================================================

TEST(StreamModeFSMTest, NoTransition_FullStaysFull) {
    StreamModeFSM fsm(StreamMode::FULL);

    auto result = fsm.evaluate(/*kvs=*/true, /*webrtc=*/true);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(fsm.current_mode(), StreamMode::FULL);
}

TEST(StreamModeFSMTest, NoTransition_DegradedStaysDegraded) {
    StreamModeFSM fsm(StreamMode::DEGRADED);

    auto result = fsm.evaluate(/*kvs=*/false, /*webrtc=*/false);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(fsm.current_mode(), StreamMode::DEGRADED);
}

TEST(StreamModeFSMTest, NoTransition_KvsOnlyStaysKvsOnly) {
    StreamModeFSM fsm(StreamMode::KVS_ONLY);

    auto result = fsm.evaluate(/*kvs=*/true, /*webrtc=*/false);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(fsm.current_mode(), StreamMode::KVS_ONLY);
}

TEST(StreamModeFSMTest, NoTransition_WebrtcOnlyStaysWebrtcOnly) {
    StreamModeFSM fsm(StreamMode::WEBRTC_ONLY);

    auto result = fsm.evaluate(/*kvs=*/false, /*webrtc=*/true);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(fsm.current_mode(), StreamMode::WEBRTC_ONLY);
}

// ============================================================
// Tests: Callback notification on transition
// Validates: Requirements 1.1
// ============================================================

TEST(StreamModeFSMTest, Callback_NotifiedOnTransition) {
    StreamModeFSM fsm(StreamMode::FULL);

    StreamMode received_mode = StreamMode::FULL;
    bool called = false;
    fsm.on_mode_change([&](StreamMode mode) {
        received_mode = mode;
        called = true;
    });

    fsm.evaluate(/*kvs=*/true, /*webrtc=*/false);

    EXPECT_TRUE(called);
    EXPECT_EQ(received_mode, StreamMode::KVS_ONLY);
}

TEST(StreamModeFSMTest, Callback_NotCalledWhenNoTransition) {
    StreamModeFSM fsm(StreamMode::FULL);

    bool called = false;
    fsm.on_mode_change([&](StreamMode) {
        called = true;
    });

    fsm.evaluate(/*kvs=*/true, /*webrtc=*/true);

    EXPECT_FALSE(called);
}

TEST(StreamModeFSMTest, Callback_CalledMultipleTimes) {
    StreamModeFSM fsm(StreamMode::FULL);

    std::vector<StreamMode> transitions;
    fsm.on_mode_change([&](StreamMode mode) {
        transitions.push_back(mode);
    });

    fsm.evaluate(/*kvs=*/true, /*webrtc=*/false);   // FULL → KVS_ONLY
    fsm.evaluate(/*kvs=*/false, /*webrtc=*/false);   // KVS_ONLY → DEGRADED
    fsm.evaluate(/*kvs=*/true, /*webrtc=*/true);     // DEGRADED → FULL

    ASSERT_EQ(transitions.size(), 3u);
    EXPECT_EQ(transitions[0], StreamMode::KVS_ONLY);
    EXPECT_EQ(transitions[1], StreamMode::DEGRADED);
    EXPECT_EQ(transitions[2], StreamMode::FULL);
}

// ============================================================
// Tests: Last transition info
// ============================================================

TEST(StreamModeFSMTest, LastTransition_EmptyInitially) {
    StreamModeFSM fsm(StreamMode::FULL);

    auto last = fsm.last_transition();
    EXPECT_FALSE(last.has_value());
}

TEST(StreamModeFSMTest, LastTransition_RecordedAfterTransition) {
    StreamModeFSM fsm(StreamMode::FULL);

    fsm.evaluate(/*kvs=*/false, /*webrtc=*/true);

    auto last = fsm.last_transition();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->old_mode, StreamMode::FULL);
    EXPECT_EQ(last->new_mode, StreamMode::WEBRTC_ONLY);
    EXPECT_FALSE(last->reason.empty());
}

TEST(StreamModeFSMTest, LastTransition_UpdatedOnEachTransition) {
    StreamModeFSM fsm(StreamMode::FULL);

    fsm.evaluate(/*kvs=*/true, /*webrtc=*/false);   // FULL → KVS_ONLY
    fsm.evaluate(/*kvs=*/false, /*webrtc=*/false);   // KVS_ONLY → DEGRADED

    auto last = fsm.last_transition();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->old_mode, StreamMode::KVS_ONLY);
    EXPECT_EQ(last->new_mode, StreamMode::DEGRADED);
}

TEST(StreamModeFSMTest, LastTransition_NotUpdatedWhenNoTransition) {
    StreamModeFSM fsm(StreamMode::FULL);

    fsm.evaluate(/*kvs=*/true, /*webrtc=*/false);   // FULL → KVS_ONLY
    fsm.evaluate(/*kvs=*/true, /*webrtc=*/false);   // no change

    auto last = fsm.last_transition();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->old_mode, StreamMode::FULL);
    EXPECT_EQ(last->new_mode, StreamMode::KVS_ONLY);
}

}  // namespace
}  // namespace sc
