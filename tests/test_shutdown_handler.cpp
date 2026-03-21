#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/shutdown_handler.h"

using namespace sc;
using namespace std::chrono_literals;

// ============================================================
// Fixture — resets static shutdown flag between tests
// ============================================================

class ShutdownHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ShutdownHandler::reset();
    }
    void TearDown() override {
        ShutdownHandler::reset();
    }
};

// ============================================================
// Test: Shutdown steps execute in registration order
// ============================================================

TEST_F(ShutdownHandlerTest, StepsExecuteInRegistrationOrder) {
    ShutdownHandler handler(15s);

    std::vector<std::string> execution_order;
    std::mutex order_mutex;

    handler.add_step("stop_video_capture", [&]() {
        std::lock_guard lock(order_mutex);
        execution_order.push_back("stop_video_capture");
        return true;
    });
    handler.add_step("flush_upload_buffer", [&]() {
        std::lock_guard lock(order_mutex);
        execution_order.push_back("flush_upload_buffer");
        return true;
    });
    handler.add_step("close_webrtc", [&]() {
        std::lock_guard lock(order_mutex);
        execution_order.push_back("close_webrtc");
        return true;
    });
    handler.add_step("close_kvs", [&]() {
        std::lock_guard lock(order_mutex);
        execution_order.push_back("close_kvs");
        return true;
    });
    handler.add_step("destroy_pipeline", [&]() {
        std::lock_guard lock(order_mutex);
        execution_order.push_back("destroy_pipeline");
        return true;
    });
    handler.add_step("release_buffer_pool", [&]() {
        std::lock_guard lock(order_mutex);
        execution_order.push_back("release_buffer_pool");
        return true;
    });

    auto summary = handler.initiate_shutdown();

    ASSERT_EQ(execution_order.size(), 6u);
    EXPECT_EQ(execution_order[0], "stop_video_capture");
    EXPECT_EQ(execution_order[1], "flush_upload_buffer");
    EXPECT_EQ(execution_order[2], "close_webrtc");
    EXPECT_EQ(execution_order[3], "close_kvs");
    EXPECT_EQ(execution_order[4], "destroy_pipeline");
    EXPECT_EQ(execution_order[5], "release_buffer_pool");

    EXPECT_TRUE(summary.completed);
    EXPECT_FALSE(summary.timed_out);
    ASSERT_EQ(summary.steps.size(), 6u);

    for (const auto& step : summary.steps) {
        EXPECT_EQ(step.result, ShutdownStepResult::Success);
    }
}

// ============================================================
// Test: Summary tracks each step's status correctly
// ============================================================

TEST_F(ShutdownHandlerTest, SummaryTracksStepStatus) {
    ShutdownHandler handler(15s);

    handler.add_step("success_step", []() { return true; });
    handler.add_step("failure_step", []() { return false; });
    handler.add_step("after_failure", []() { return true; });

    auto summary = handler.initiate_shutdown();

    ASSERT_EQ(summary.steps.size(), 3u);
    EXPECT_EQ(summary.steps[0].name, "success_step");
    EXPECT_EQ(summary.steps[0].result, ShutdownStepResult::Success);

    EXPECT_EQ(summary.steps[1].name, "failure_step");
    EXPECT_EQ(summary.steps[1].result, ShutdownStepResult::Error);

    EXPECT_EQ(summary.steps[2].name, "after_failure");
    EXPECT_EQ(summary.steps[2].result, ShutdownStepResult::Success);

    EXPECT_TRUE(summary.completed);
}

// ============================================================
// Test: Timeout forces remaining steps to be skipped
// ============================================================

TEST_F(ShutdownHandlerTest, TimeoutForcesTermination) {
    // Use a very short timeout (1 second)
    ShutdownHandler handler(1s);

    std::atomic<bool> slow_step_started{false};

    handler.add_step("fast_step", []() { return true; });
    handler.add_step("slow_step", [&]() {
        slow_step_started.store(true);
        // Sleep longer than the total timeout
        std::this_thread::sleep_for(2s);
        return true;
    });
    handler.add_step("should_be_skipped", []() { return true; });

    auto summary = handler.initiate_shutdown();

    EXPECT_TRUE(summary.timed_out);
    EXPECT_TRUE(summary.completed);
    EXPECT_TRUE(slow_step_started.load());

    ASSERT_EQ(summary.steps.size(), 3u);

    // First step should succeed
    EXPECT_EQ(summary.steps[0].name, "fast_step");
    EXPECT_EQ(summary.steps[0].result, ShutdownStepResult::Success);

    // Slow step should be marked as timeout
    EXPECT_EQ(summary.steps[1].name, "slow_step");
    EXPECT_EQ(summary.steps[1].result, ShutdownStepResult::Timeout);

    // Last step should be skipped
    EXPECT_EQ(summary.steps[2].name, "should_be_skipped");
    EXPECT_EQ(summary.steps[2].result, ShutdownStepResult::Skipped);
}

// ============================================================
// Test: Static signal flag works correctly
// ============================================================

TEST_F(ShutdownHandlerTest, SignalFlagRequestAndReset) {
    EXPECT_FALSE(ShutdownHandler::shutdown_requested());

    ShutdownHandler::request_shutdown();
    EXPECT_TRUE(ShutdownHandler::shutdown_requested());

    ShutdownHandler::reset();
    EXPECT_FALSE(ShutdownHandler::shutdown_requested());
}

// ============================================================
// Test: Re-entrant shutdown returns existing summary
// ============================================================

TEST_F(ShutdownHandlerTest, ReentrantShutdownReturnsSummary) {
    ShutdownHandler handler(15s);

    std::atomic<int> call_count{0};
    handler.add_step("step1", [&]() {
        call_count.fetch_add(1);
        return true;
    });

    auto summary1 = handler.initiate_shutdown();
    auto summary2 = handler.initiate_shutdown();

    // Step should only execute once
    EXPECT_EQ(call_count.load(), 1);
    EXPECT_TRUE(summary1.completed);
    EXPECT_TRUE(summary2.completed);
}

// ============================================================
// Test: Persist callback is invoked during shutdown
// ============================================================

TEST_F(ShutdownHandlerTest, PersistCallbackInvokedDuringShutdown) {
    ShutdownHandler handler(15s);

    bool persist_called = false;
    handler.on_persist_cache([&]() {
        persist_called = true;
    });

    handler.add_step("step1", []() { return true; });

    handler.initiate_shutdown();

    EXPECT_TRUE(persist_called);
}

// ============================================================
// Test: Stream recovery resets counters
// ============================================================

TEST_F(ShutdownHandlerTest, StreamRecoveryResetsCounters) {
    ShutdownHandler handler(15s);

    bool recovery_called = false;
    handler.on_stream_recovery([&]() {
        recovery_called = true;
    });

    handler.notify_stream_recovery();

    EXPECT_TRUE(recovery_called);
}

// ============================================================
// Test: Exception in step is caught and reported as error
// ============================================================

TEST_F(ShutdownHandlerTest, ExceptionInStepReportedAsError) {
    ShutdownHandler handler(15s);

    handler.add_step("throwing_step", []() -> bool {
        throw std::runtime_error("test exception");
    });
    handler.add_step("after_throw", []() { return true; });

    auto summary = handler.initiate_shutdown();

    ASSERT_EQ(summary.steps.size(), 2u);
    EXPECT_EQ(summary.steps[0].result, ShutdownStepResult::Error);
    EXPECT_NE(summary.steps[0].error_message.find("test exception"),
              std::string::npos);

    // Step after exception should still run
    EXPECT_EQ(summary.steps[1].result, ShutdownStepResult::Success);
}

// ============================================================
// Test: is_shutting_down reflects state
// ============================================================

TEST_F(ShutdownHandlerTest, IsShuttingDownReflectsState) {
    ShutdownHandler handler(15s);

    EXPECT_FALSE(handler.is_shutting_down());

    handler.add_step("step1", []() { return true; });
    handler.initiate_shutdown();

    EXPECT_TRUE(handler.is_shutting_down());
}

// ============================================================
// Test: Timeout value is configurable
// ============================================================

TEST_F(ShutdownHandlerTest, TimeoutIsConfigurable) {
    ShutdownHandler handler1(10s);
    EXPECT_EQ(handler1.timeout(), 10s);

    ShutdownHandler handler2(30s);
    EXPECT_EQ(handler2.timeout(), 30s);

    // Default is 15s
    ShutdownHandler handler3;
    EXPECT_EQ(handler3.timeout(), 15s);
}

// ============================================================
// Test: Empty shutdown (no steps) completes immediately
// ============================================================

TEST_F(ShutdownHandlerTest, EmptyShutdownCompletesImmediately) {
    ShutdownHandler handler(15s);

    auto summary = handler.initiate_shutdown();

    EXPECT_TRUE(summary.completed);
    EXPECT_FALSE(summary.timed_out);
    EXPECT_TRUE(summary.steps.empty());
}

// ============================================================
// Test: Step duration is tracked
// ============================================================

TEST_F(ShutdownHandlerTest, StepDurationIsTracked) {
    ShutdownHandler handler(15s);

    handler.add_step("timed_step", []() {
        std::this_thread::sleep_for(100ms);
        return true;
    });

    auto summary = handler.initiate_shutdown();

    ASSERT_EQ(summary.steps.size(), 1u);
    // Should be at least 100ms (with some tolerance)
    EXPECT_GE(summary.steps[0].duration.count(), 80);
    EXPECT_GT(summary.total_duration.count(), 0);
}
