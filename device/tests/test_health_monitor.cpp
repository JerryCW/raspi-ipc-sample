#include "monitor/health_monitor.h"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/detail/TestParams.h>
#include <rapidcheck/detail/TestMetadata.h>
#include <rapidcheck/detail/Results.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace sc {
namespace {

// ============================================================
// Minimal stub pipeline for testing (no GStreamer dependency)
// ============================================================

class StubPipeline : public IGStreamerPipeline {
public:
    VoidResult build(const PipelineConfig&) override {
        state_ = State::READY;
        build_count_++;
        return OkVoid();
    }

    VoidResult start() override {
        state_ = State::PLAYING;
        start_count_++;
        return OkVoid();
    }

    VoidResult stop() override {
        state_ = State::READY;
        stop_count_++;
        return OkVoid();
    }

    VoidResult destroy() override {
        state_ = State::NULL_STATE;
        destroy_count_++;
        return OkVoid();
    }

    VoidResult set_bitrate(uint32_t) override { return OkVoid(); }

    State current_state() const override { return state_; }
    std::string encoder_name() const override { return "stub"; }

    int start_count() const { return start_count_; }
    int stop_count() const { return stop_count_; }
    int destroy_count() const { return destroy_count_; }
    int build_count() const { return build_count_; }

private:
    State state_ = State::NULL_STATE;
    int start_count_ = 0;
    int stop_count_ = 0;
    int destroy_count_ = 0;
    int build_count_ = 0;
};

// Helper to create a PipelineError with current timestamp
PipelineError make_error(const std::string& element = "fakesrc",
                         uint32_t domain = 1,
                         int32_t code = 42,
                         const std::string& debug = "test error") {
    return PipelineError{element, domain, code, debug,
                         std::chrono::steady_clock::now()};
}

// ============================================================
// Test: Start / Stop lifecycle
// ============================================================

TEST(HealthMonitorTest, StartStopLifecycle) {
    HealthMonitor monitor;
    auto pipeline = std::make_shared<StubPipeline>();

    auto result = monitor.start(pipeline);
    ASSERT_TRUE(result.is_ok());

    // Starting again should fail
    auto result2 = monitor.start(pipeline);
    EXPECT_TRUE(result2.is_err());

    auto stop_result = monitor.stop();
    EXPECT_TRUE(stop_result.is_ok());

    // Stopping again is idempotent
    auto stop_result2 = monitor.stop();
    EXPECT_TRUE(stop_result2.is_ok());
}

TEST(HealthMonitorTest, StartWithNullPipelineFails) {
    HealthMonitor monitor;
    auto result = monitor.start(nullptr);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

// ============================================================
// Test: Restart count increments on error
// ============================================================

TEST(HealthMonitorTest, RestartCountIncrementsOnError) {
    // Use a large window so nothing expires
    HealthMonitor monitor(std::chrono::seconds(600), 5);
    auto pipeline = std::make_shared<StubPipeline>();
    monitor.start(pipeline);

    EXPECT_EQ(monitor.restart_count_in_window(), 0u);
    EXPECT_FALSE(monitor.is_restart_limit_reached());

    monitor.report_error(make_error());
    EXPECT_EQ(monitor.restart_count_in_window(), 1u);

    monitor.report_error(make_error());
    EXPECT_EQ(monitor.restart_count_in_window(), 2u);

    // Pipeline should have been stopped and started for each restart
    EXPECT_EQ(pipeline->start_count(), 2);
    EXPECT_EQ(pipeline->stop_count(), 2);
}

// ============================================================
// Test: Restart limit (max 5 in 10 min window)
// ============================================================

TEST(HealthMonitorTest, RestartLimitEnforced) {
    HealthMonitor monitor(std::chrono::seconds(600), 5);
    auto pipeline = std::make_shared<StubPipeline>();
    monitor.start(pipeline);

    // Trigger 5 restarts — all should succeed
    for (int i = 0; i < 5; ++i) {
        bool ok = monitor.attempt_restart();
        EXPECT_TRUE(ok) << "Restart " << (i + 1) << " should succeed";
    }

    EXPECT_EQ(monitor.restart_count_in_window(), 5u);
    EXPECT_TRUE(monitor.is_restart_limit_reached());

    // 6th restart should be blocked
    bool ok = monitor.attempt_restart();
    EXPECT_FALSE(ok);

    // Count should still be 5 (6th was rejected)
    EXPECT_EQ(monitor.restart_count_in_window(), 5u);

    // Pipeline start/stop should have been called exactly 5 times
    EXPECT_EQ(pipeline->start_count(), 5);
    EXPECT_EQ(pipeline->stop_count(), 5);
}

// ============================================================
// Test: is_restart_limit_reached returns true at limit
// ============================================================

TEST(HealthMonitorTest, IsRestartLimitReachedAtExactLimit) {
    HealthMonitor monitor(std::chrono::seconds(600), 3);
    auto pipeline = std::make_shared<StubPipeline>();
    monitor.start(pipeline);

    EXPECT_FALSE(monitor.is_restart_limit_reached());

    monitor.attempt_restart();
    EXPECT_FALSE(monitor.is_restart_limit_reached());

    monitor.attempt_restart();
    EXPECT_FALSE(monitor.is_restart_limit_reached());

    monitor.attempt_restart();
    EXPECT_TRUE(monitor.is_restart_limit_reached());
}

// ============================================================
// Test: Window expiry resets counter
// ============================================================

TEST(HealthMonitorTest, WindowExpiryResetsCounter) {
    // Use a very short window (1 second) for testing
    HealthMonitor monitor(std::chrono::seconds(1), 5);
    auto pipeline = std::make_shared<StubPipeline>();
    monitor.start(pipeline);

    // Fill up 3 restarts
    monitor.attempt_restart();
    monitor.attempt_restart();
    monitor.attempt_restart();
    EXPECT_EQ(monitor.restart_count_in_window(), 3u);

    // Wait for the window to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Counter should have been pruned — old restarts expired
    EXPECT_EQ(monitor.restart_count_in_window(), 0u);
    EXPECT_FALSE(monitor.is_restart_limit_reached());

    // Should be able to restart again
    bool ok = monitor.attempt_restart();
    EXPECT_TRUE(ok);
    EXPECT_EQ(monitor.restart_count_in_window(), 1u);
}

TEST(HealthMonitorTest, WindowExpiryAfterLimitAllowsNewRestarts) {
    // 1-second window, max 2 restarts
    HealthMonitor monitor(std::chrono::seconds(1), 2);
    auto pipeline = std::make_shared<StubPipeline>();
    monitor.start(pipeline);

    // Hit the limit
    EXPECT_TRUE(monitor.attempt_restart());
    EXPECT_TRUE(monitor.attempt_restart());
    EXPECT_TRUE(monitor.is_restart_limit_reached());
    EXPECT_FALSE(monitor.attempt_restart());

    // Wait for window to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Should be able to restart again
    EXPECT_FALSE(monitor.is_restart_limit_reached());
    EXPECT_TRUE(monitor.attempt_restart());
    EXPECT_EQ(monitor.restart_count_in_window(), 1u);
}

// ============================================================
// Test: Error callback notification
// ============================================================

TEST(HealthMonitorTest, ErrorCallbackNotification) {
    HealthMonitor monitor(std::chrono::seconds(600), 5);
    auto pipeline = std::make_shared<StubPipeline>();
    monitor.start(pipeline);

    PipelineError received_error;
    bool callback_called = false;

    monitor.on_error([&](const PipelineError& err) {
        received_error = err;
        callback_called = true;
    });

    auto error = make_error("encoder0", 7, 99, "encoder stalled");
    monitor.report_error(error);

    EXPECT_TRUE(callback_called);
    EXPECT_EQ(received_error.element_name, "encoder0");
    EXPECT_EQ(received_error.error_domain, 7u);
    EXPECT_EQ(received_error.error_code, 99);
    EXPECT_EQ(received_error.debug_info, "encoder stalled");
}

// ============================================================
// Test: Watchdog alert triggered at restart limit
// ============================================================

TEST(HealthMonitorTest, WatchdogAlertAtRestartLimit) {
    HealthMonitor monitor(std::chrono::seconds(600), 2);
    auto pipeline = std::make_shared<StubPipeline>();
    monitor.start(pipeline);

    bool alert_triggered = false;
    std::string alert_reason;

    monitor.on_watchdog_alert([&](const std::string& reason) {
        alert_triggered = true;
        alert_reason = reason;
    });

    // Fill up the limit
    monitor.attempt_restart();
    monitor.attempt_restart();
    EXPECT_FALSE(alert_triggered);

    // Next attempt should trigger the alert
    monitor.attempt_restart();
    EXPECT_TRUE(alert_triggered);
    EXPECT_FALSE(alert_reason.empty());
}

// ============================================================
// Test: report_error triggers restart attempt
// ============================================================

TEST(HealthMonitorTest, ReportErrorTriggersRestart) {
    HealthMonitor monitor(std::chrono::seconds(600), 5);
    auto pipeline = std::make_shared<StubPipeline>();
    monitor.start(pipeline);

    monitor.report_error(make_error());

    // Pipeline should have been restarted (stop + start)
    EXPECT_EQ(pipeline->stop_count(), 1);
    EXPECT_EQ(pipeline->start_count(), 1);
    EXPECT_EQ(monitor.restart_count_in_window(), 1u);
}

// ============================================================
// Property Test: Bug Condition — Frame stall not detected
// Validates: Requirements 1.1, 2.1, 2.2
//
// Bug condition: elapsed > stall_timeout AND pipeline_state == PLAYING
//                AND gstreamerBusErrorCount == 0
//                AND healthMonitorDetected == false
//
// This test asserts the EXPECTED behavior: after a frame stall
// exceeding stall_timeout (30s), HealthMonitor SHOULD detect
// the stall and attempt recovery (restart_count_in_window > 0).
//
// On UNFIXED code this will FAIL — proving the bug exists:
// HealthMonitor has NO frame stall detection capability.
// ============================================================

TEST(HealthMonitorBugCondition, FrameStallShouldBeDetected) {
    rc::detail::TestParams params;
    params.maxSuccess = 20;
    rc::detail::TestMetadata metadata;
    metadata.id = "FrameStallShouldBeDetected";
    auto result = rc::detail::checkTestable([]() {
        const auto stall_seconds = *rc::gen::inRange(31, 121);

        HealthMonitor monitor(std::chrono::seconds(600), 5,
                              std::chrono::seconds(0), 3);
        auto pipeline = std::make_shared<StubPipeline>();
        PipelineConfig config;

        pipeline->build(config);
        pipeline->start();
        RC_ASSERT(pipeline->current_state() == IGStreamerPipeline::State::PLAYING);

        auto start_result = monitor.start(pipeline, config);
        RC_ASSERT(start_result.is_ok());

        monitor.report_frame_produced();
        monitor.check_frame_stall();

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        monitor.check_frame_stall();

        RC_ASSERT(pipeline->destroy_count() > 0);
        RC_ASSERT(pipeline->build_count() > 0);
        RC_TAG(stall_seconds);
    }, metadata, params);
    ASSERT_TRUE(result.template is<rc::detail::SuccessResult>());
}

// ============================================================
// Property 2a: Preservation — report_error() triggers
// attempt_restart() behavior consistently
// Validates: Requirements 3.1, 3.2
//
// For any restart_window (10-600s) and max_restarts (1-10),
// report_error() increments restart_count, and after reaching
// the limit, attempt_restart() returns false.
// ============================================================

TEST(HealthMonitorPreservation, ReportErrorTriggersRestartConsistently) {
    rc::detail::TestParams params;
    params.maxSuccess = 20;
    rc::detail::TestMetadata metadata;
    metadata.id = "ReportErrorTriggersRestartConsistently";
    auto result = rc::detail::checkTestable([]() {
        const auto restart_window = *rc::gen::inRange(10, 601);
        const auto max_restarts = *rc::gen::inRange(1, 11);

        HealthMonitor monitor(std::chrono::seconds(restart_window),
                              static_cast<uint32_t>(max_restarts));
        auto pipeline = std::make_shared<StubPipeline>();
        auto start_result = monitor.start(pipeline);
        RC_ASSERT(start_result.is_ok());

        for (int i = 1; i <= max_restarts; ++i) {
            monitor.report_error(make_error());
            RC_ASSERT(monitor.restart_count_in_window() == static_cast<uint32_t>(i));
        }

        RC_ASSERT(monitor.is_restart_limit_reached());
        bool ok = monitor.attempt_restart();
        RC_ASSERT(!ok);
        RC_ASSERT(monitor.restart_count_in_window() == static_cast<uint32_t>(max_restarts));
    }, metadata, params);
    ASSERT_TRUE(result.template is<rc::detail::SuccessResult>());
}

// ============================================================
// Property 2b: Preservation — pipeline stop/start counts
// equal successful restarts
// Validates: Requirements 3.1, 3.2
//
// For any error sequence length (1-20), pipeline->stop_count()
// and pipeline->start_count() equal min(error_count, max_restarts).
// ============================================================

TEST(HealthMonitorPreservation, PipelineStopStartCountsEqualSuccessfulRestarts) {
    rc::detail::TestParams params;
    params.maxSuccess = 20;
    rc::detail::TestMetadata metadata;
    metadata.id = "PipelineStopStartCountsEqualSuccessfulRestarts";
    auto result = rc::detail::checkTestable([]() {
        const auto error_count = *rc::gen::inRange(1, 21);
        const auto max_restarts = *rc::gen::inRange(1, 11);

        HealthMonitor monitor(std::chrono::seconds(600),
                              static_cast<uint32_t>(max_restarts));
        auto pipeline = std::make_shared<StubPipeline>();
        auto start_result = monitor.start(pipeline);
        RC_ASSERT(start_result.is_ok());

        for (int i = 0; i < error_count; ++i) {
            monitor.report_error(make_error());
        }

        const int expected = std::min(error_count, max_restarts);
        RC_ASSERT(pipeline->stop_count() == expected);
        RC_ASSERT(pipeline->start_count() == expected);
    }, metadata, params);
    ASSERT_TRUE(result.template is<rc::detail::SuccessResult>());
}

}  // namespace
}  // namespace sc
