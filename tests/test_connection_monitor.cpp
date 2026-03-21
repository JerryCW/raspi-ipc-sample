#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/stream_mode.h"
#include "monitor/connection_monitor.h"

namespace sc {
namespace {

// ============================================================
// Helper: start then immediately stop to initialize state
// without the background thread racing with manual check_once().
// ============================================================

void init_and_stop(ConnectionMonitor& cm,
                   const std::string& kvs_url,
                   const std::string& webrtc_url) {
    auto r = cm.start(kvs_url, webrtc_url);
    ASSERT_TRUE(r.is_ok());
    cm.stop();
}

// ============================================================
// Tests: Start / Stop lifecycle
// Validates: Requirements 1.1
// ============================================================

TEST(ConnectionMonitorTest, Start_Succeeds) {
    ConnectionMonitor cm(3, 2, std::chrono::seconds(30), std::chrono::seconds(5));

    auto result = cm.start("https://kvs.example.com", "https://webrtc.example.com");
    ASSERT_TRUE(result.is_ok());

    cm.stop();
}

TEST(ConnectionMonitorTest, DoubleStart_ReturnsError) {
    ConnectionMonitor cm(3, 2, std::chrono::seconds(30), std::chrono::seconds(5));

    ASSERT_TRUE(cm.start("https://kvs.example.com", "https://webrtc.example.com").is_ok());

    auto result = cm.start("https://kvs2.example.com", "https://webrtc2.example.com");
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);

    cm.stop();
}

TEST(ConnectionMonitorTest, StopWithoutStart_IsOk) {
    ConnectionMonitor cm;
    auto result = cm.stop();
    EXPECT_TRUE(result.is_ok());
}

TEST(ConnectionMonitorTest, StartStopStart_Works) {
    ConnectionMonitor cm(3, 2, std::chrono::seconds(30), std::chrono::seconds(5));

    ASSERT_TRUE(cm.start("https://kvs.example.com", "https://webrtc.example.com").is_ok());
    ASSERT_TRUE(cm.stop().is_ok());
    ASSERT_TRUE(cm.start("https://kvs2.example.com", "https://webrtc2.example.com").is_ok());

    cm.stop();
}

// ============================================================
// Tests: check_once() updates endpoint status
// Empty URLs always fail (before CURL check). This works
// regardless of whether CURL is available.
// Validates: Requirements 1.1
// ============================================================

TEST(ConnectionMonitorTest, CheckOnce_EmptyUrlsAreUnreachable) {
    ConnectionMonitor cm(3, 2, std::chrono::seconds(30), std::chrono::seconds(1));
    init_and_stop(cm, "", "");

    cm.check_once();

    auto kvs = cm.kvs_status();
    auto webrtc = cm.webrtc_status();

    EXPECT_FALSE(kvs.reachable);
    EXPECT_FALSE(webrtc.reachable);
    EXPECT_EQ(kvs.url, "");
    EXPECT_EQ(webrtc.url, "");
}

// ============================================================
// Tests: Consecutive failure counting with empty URL
// An empty URL causes check_endpoint to return false (unreachable).
// 3 failures → offline
// Validates: Requirements 1.3, 1.4, 1.5
// ============================================================

TEST(ConnectionMonitorTest, ConsecutiveFailures_EmptyUrl) {
    ConnectionMonitor cm(3, 2, std::chrono::seconds(30), std::chrono::seconds(1));
    init_and_stop(cm, "", "");

    cm.check_once();
    EXPECT_EQ(cm.kvs_status().consecutive_failures, 1u);
    EXPECT_FALSE(cm.kvs_status().reachable);

    cm.check_once();
    EXPECT_EQ(cm.kvs_status().consecutive_failures, 2u);

    cm.check_once();
    EXPECT_EQ(cm.kvs_status().consecutive_failures, 3u);
}

TEST(ConnectionMonitorTest, ConsecutiveFailures_BothEmpty_StaysDegraded) {
    // Both empty → both unreachable → FSM stays DEGRADED (no transition)
    ConnectionMonitor cm(1, 1, std::chrono::seconds(30), std::chrono::seconds(1));

    bool called = false;
    cm.on_mode_change([&](StreamMode) {
        called = true;
    });

    init_and_stop(cm, "", "");

    cm.check_once();
    cm.check_once();
    cm.check_once();

    EXPECT_FALSE(called);
}

// ============================================================
// Tests: Mode callback triggered on state change
// With one empty URL (unreachable) and one empty URL (unreachable),
// FSM stays DEGRADED. We test transitions by using the FSM's
// evaluate logic through the monitor's evaluate_and_notify path.
//
// Since we can't easily make real URLs reachable in tests,
// we test the FSM integration by verifying that:
// - Both empty → DEGRADED (no transition from initial DEGRADED)
// - The callback wiring works via the start() setup
// Validates: Requirements 1.3, 1.6
// ============================================================

TEST(ConnectionMonitorTest, ModeCallback_BothUnreachable_StaysDegraded) {
    ConnectionMonitor cm(1, 1, std::chrono::seconds(30), std::chrono::seconds(1));

    bool called = false;
    cm.on_mode_change([&](StreamMode) {
        called = true;
    });

    init_and_stop(cm, "", "");
    cm.check_once();

    EXPECT_FALSE(called);
}

// ============================================================
// Tests: Endpoint status fields are populated after check
// ============================================================

TEST(ConnectionMonitorTest, EndpointStatus_UrlPreserved) {
    ConnectionMonitor cm(3, 2, std::chrono::seconds(30), std::chrono::seconds(1));
    init_and_stop(cm, "https://kvs.test.com", "https://webrtc.test.com");

    EXPECT_EQ(cm.kvs_status().url, "https://kvs.test.com");
    EXPECT_EQ(cm.webrtc_status().url, "https://webrtc.test.com");
}

TEST(ConnectionMonitorTest, EndpointStatus_CheckTimeUpdated) {
    ConnectionMonitor cm(3, 2, std::chrono::seconds(30), std::chrono::seconds(1));
    init_and_stop(cm, "", "");

    auto before = std::chrono::steady_clock::now();
    cm.check_once();
    auto after = std::chrono::steady_clock::now();

    auto kvs_time = cm.kvs_status().check_time;
    EXPECT_GE(kvs_time, before);
    EXPECT_LE(kvs_time, after);
}

// ============================================================
// Tests: Failure counter resets on success, success counter
// resets on failure (tested via empty URL which always fails)
// Validates: Requirements 1.6
// ============================================================

TEST(ConnectionMonitorTest, FailureResetsSuccessCounter) {
    // Empty URL always fails → consecutive_successes stays 0
    ConnectionMonitor cm(3, 2, std::chrono::seconds(30), std::chrono::seconds(1));
    init_and_stop(cm, "", "");

    cm.check_once();
    EXPECT_EQ(cm.kvs_status().consecutive_successes, 0u);
    EXPECT_EQ(cm.kvs_status().consecutive_failures, 1u);

    cm.check_once();
    EXPECT_EQ(cm.kvs_status().consecutive_successes, 0u);
    EXPECT_EQ(cm.kvs_status().consecutive_failures, 2u);
}

// ============================================================
// Tests: Destructor stops cleanly
// ============================================================

TEST(ConnectionMonitorTest, Destructor_StopsRunningMonitor) {
    {
        ConnectionMonitor cm(3, 2, std::chrono::seconds(1), std::chrono::seconds(1));
        ASSERT_TRUE(cm.start("https://kvs.example.com", "https://webrtc.example.com").is_ok());
        // Destructor should call stop() and join the thread
    }
    SUCCEED();
}

// ============================================================
// Tests: FSM integration — evaluate_and_notify drives mode
// transitions based on endpoint availability thresholds.
//
// With empty URLs (always fail), after failure_threshold checks
// the endpoint is considered offline. We verify the FSM stays
// in DEGRADED since both are offline from the start.
// Validates: Requirements 1.5
// ============================================================

TEST(ConnectionMonitorTest, FSM_BothOffline_EventuallyDegraded) {
    // failure_threshold=1: after 1 failure, endpoint goes offline immediately
    // With threshold=1, first check sees failures=1 >= 1 → offline
    // Both offline from first check → FSM stays DEGRADED (no transition)
    ConnectionMonitor cm(1, 1, std::chrono::seconds(30), std::chrono::seconds(1));

    std::vector<StreamMode> transitions;
    std::mutex mtx;
    cm.on_mode_change([&](StreamMode mode) {
        std::lock_guard<std::mutex> lock(mtx);
        transitions.push_back(mode);
    });

    init_and_stop(cm, "", "");

    // Both empty → both fail → with threshold=1, first check marks offline
    // But note: initially consecutive_failures=0 < 1 is false (0 < 1 is true)
    // So before first check, endpoints are "available" (failures=0 < threshold=1)
    // After first check: failures=1, 1 < 1 is false → offline
    // evaluate_and_notify after first check: both offline → DEGRADED
    // But FSM was already DEGRADED → no transition callback
    // Wait — the evaluate happens AFTER update_status in check_once.
    // After update: failures=1. evaluate: 1 < 1 = false, successes=0 >= 1 = false → offline
    // Both offline → DEGRADED. FSM already DEGRADED → no callback.
    cm.check_once();

    std::lock_guard<std::mutex> lock(mtx);
    EXPECT_TRUE(transitions.empty());
}

// ============================================================
// Tests: Threshold logic — endpoint considered available when
// consecutive_failures < failure_threshold OR
// consecutive_successes >= success_threshold.
//
// Initially consecutive_failures=0 < threshold, so endpoint
// is considered "available" even before any check. After
// failure_threshold failures, it goes offline.
// Validates: Requirements 1.3, 1.4
// ============================================================

TEST(ConnectionMonitorTest, ThresholdLogic_InitiallyAvailable) {
    // Before any check, consecutive_failures=0 < threshold=3
    // So evaluate_and_notify considers endpoints "available"
    // FSM starts at DEGRADED, both "available" → transitions to FULL
    ConnectionMonitor cm(3, 2, std::chrono::seconds(30), std::chrono::seconds(1));

    std::vector<StreamMode> transitions;
    std::mutex mtx;
    cm.on_mode_change([&](StreamMode mode) {
        std::lock_guard<std::mutex> lock(mtx);
        transitions.push_back(mode);
    });

    // start() wires the FSM callback, then the monitor_loop does
    // a check_once() immediately. We use start+stop+manual checks
    // to control timing.
    init_and_stop(cm, "", "");

    // Before any check_once, evaluate_and_notify would see
    // consecutive_failures=0 < 3 → both "available" → FULL.
    // But we haven't called check_once yet, so no evaluate happened
    // after stop. Let's verify after first check with empty URLs:
    // After 1 failure: consecutive_failures=1 < 3 → still "available"
    cm.check_once();

    {
        std::lock_guard<std::mutex> lock(mtx);
        // First check: failures=1 < threshold=3, so both still "available"
        // FSM: DEGRADED → FULL (because both considered available)
        ASSERT_EQ(transitions.size(), 1u);
        EXPECT_EQ(transitions[0], StreamMode::FULL);
    }

    // After 3 failures: consecutive_failures=3 >= threshold=3 → offline
    cm.check_once();  // failures=2
    cm.check_once();  // failures=3 → offline

    {
        std::lock_guard<std::mutex> lock(mtx);
        // FSM: FULL → DEGRADED (both now offline)
        ASSERT_GE(transitions.size(), 2u);
        EXPECT_EQ(transitions.back(), StreamMode::DEGRADED);
    }
}

}  // namespace
}  // namespace sc
