#include "pipeline/pipeline_orchestrator.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <vector>

namespace sc {
namespace {

// ============================================================
// StubPipeline — minimal stub (no GStreamer dependency)
// ============================================================

class StubPipeline : public IGStreamerPipeline {
public:
    VoidResult build(const PipelineConfig&) override { return OkVoid(); }

    VoidResult start() override {
        if (fail_on_start_) {
            return ErrVoid(ErrorCode::PipelineStateChangeFailed,
                           "stub forced start failure",
                           "StubPipeline::start");
        }
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
        return OkVoid();
    }

    VoidResult set_bitrate(uint32_t) override { return OkVoid(); }

    State current_state() const override { return state_; }
    std::string encoder_name() const override { return encoder_name_; }

    // Test helpers
    void set_fail_on_start(bool fail) { fail_on_start_ = fail; }
    void set_encoder_name(const std::string& name) { encoder_name_ = name; }
    int start_count() const { return start_count_; }
    int stop_count() const { return stop_count_; }

private:
    State state_ = State::NULL_STATE;
    bool fail_on_start_ = false;
    std::string encoder_name_ = "stub_encoder";
    int start_count_ = 0;
    int stop_count_ = 0;
};

// ============================================================
// StubHealthMonitor — minimal stub for IHealthMonitor
// ============================================================

class StubHealthMonitor : public IHealthMonitor {
public:
    VoidResult start(std::shared_ptr<IGStreamerPipeline>) override {
        running_ = true;
        return OkVoid();
    }

    VoidResult stop() override {
        running_ = false;
        return OkVoid();
    }

    uint32_t restart_count_in_window() const override { return restart_count_; }
    bool is_restart_limit_reached() const override { return false; }
    void on_error(ErrorCallback) override {}

    // Test helpers
    void set_restart_count(uint32_t count) { restart_count_ = count; }
    bool is_running() const { return running_; }

private:
    bool running_ = false;
    uint32_t restart_count_ = 0;
};

// ============================================================
// Test: Add and remove pipelines
// ============================================================

TEST(PipelineOrchestratorTest, AddAndRemovePipelines) {
    PipelineOrchestrator orch;
    auto p1 = std::make_shared<StubPipeline>();
    auto p2 = std::make_shared<StubPipeline>();

    EXPECT_EQ(orch.pipeline_count(), 0u);

    auto r1 = orch.add_pipeline("cam1", PipelineConfig{}, p1);
    ASSERT_TRUE(r1.is_ok());
    EXPECT_EQ(orch.pipeline_count(), 1u);

    auto r2 = orch.add_pipeline("cam2", PipelineConfig{}, p2);
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(orch.pipeline_count(), 2u);

    auto r3 = orch.remove_pipeline("cam1");
    ASSERT_TRUE(r3.is_ok());
    EXPECT_EQ(orch.pipeline_count(), 1u);
}

TEST(PipelineOrchestratorTest, AddDuplicateIdFails) {
    PipelineOrchestrator orch;
    auto p1 = std::make_shared<StubPipeline>();
    auto p2 = std::make_shared<StubPipeline>();

    ASSERT_TRUE(orch.add_pipeline("cam1", PipelineConfig{}, p1).is_ok());

    auto result = orch.add_pipeline("cam1", PipelineConfig{}, p2);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(PipelineOrchestratorTest, AddEmptyIdFails) {
    PipelineOrchestrator orch;
    auto result = orch.add_pipeline("", PipelineConfig{}, std::make_shared<StubPipeline>());
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(PipelineOrchestratorTest, AddNullPipelineFails) {
    PipelineOrchestrator orch;
    auto result = orch.add_pipeline("cam1", PipelineConfig{}, nullptr);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(PipelineOrchestratorTest, RemoveNonexistentFails) {
    PipelineOrchestrator orch;
    auto result = orch.remove_pipeline("no_such_pipeline");
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
}

// ============================================================
// Test: start_all / stop_all
// ============================================================

TEST(PipelineOrchestratorTest, StartAllStopAll) {
    PipelineOrchestrator orch;
    auto p1 = std::make_shared<StubPipeline>();
    auto p2 = std::make_shared<StubPipeline>();

    orch.add_pipeline("cam1", PipelineConfig{}, p1);
    orch.add_pipeline("cam2", PipelineConfig{}, p2);

    auto start_result = orch.start_all();
    ASSERT_TRUE(start_result.is_ok());
    EXPECT_EQ(p1->current_state(), IGStreamerPipeline::State::PLAYING);
    EXPECT_EQ(p2->current_state(), IGStreamerPipeline::State::PLAYING);

    auto stop_result = orch.stop_all();
    ASSERT_TRUE(stop_result.is_ok());
    EXPECT_EQ(p1->current_state(), IGStreamerPipeline::State::READY);
    EXPECT_EQ(p2->current_state(), IGStreamerPipeline::State::READY);
}

// ============================================================
// Test: Fault isolation — one pipeline failure doesn't block others
// ============================================================

TEST(PipelineOrchestratorTest, FaultIsolationOnStartAll) {
    PipelineOrchestrator orch;
    auto p1 = std::make_shared<StubPipeline>();
    auto p2 = std::make_shared<StubPipeline>();
    auto p3 = std::make_shared<StubPipeline>();

    // p2 will fail on start
    p2->set_fail_on_start(true);

    orch.add_pipeline("cam1", PipelineConfig{}, p1);
    orch.add_pipeline("cam2", PipelineConfig{}, p2);
    orch.add_pipeline("cam3", PipelineConfig{}, p3);

    auto result = orch.start_all();
    ASSERT_TRUE(result.is_ok());  // start_all succeeds overall

    // p1 and p3 should be playing; p2 should still be NULL_STATE
    EXPECT_EQ(p1->current_state(), IGStreamerPipeline::State::PLAYING);
    EXPECT_EQ(p2->current_state(), IGStreamerPipeline::State::NULL_STATE);
    EXPECT_EQ(p3->current_state(), IGStreamerPipeline::State::PLAYING);
}

// ============================================================
// Test: restart_pipeline — independent restart
// ============================================================

TEST(PipelineOrchestratorTest, RestartSinglePipeline) {
    PipelineOrchestrator orch;
    auto p1 = std::make_shared<StubPipeline>();
    auto p2 = std::make_shared<StubPipeline>();

    orch.add_pipeline("cam1", PipelineConfig{}, p1);
    orch.add_pipeline("cam2", PipelineConfig{}, p2);

    orch.start_all();

    // Restart only cam1
    auto result = orch.restart_pipeline("cam1");
    ASSERT_TRUE(result.is_ok());

    // cam1: start_all called start once, restart called stop+start once
    EXPECT_EQ(p1->stop_count(), 1);
    EXPECT_EQ(p1->start_count(), 2);
    EXPECT_EQ(p1->current_state(), IGStreamerPipeline::State::PLAYING);

    // cam2 should be untouched after restart
    EXPECT_EQ(p2->stop_count(), 0);
    EXPECT_EQ(p2->start_count(), 1);
    EXPECT_EQ(p2->current_state(), IGStreamerPipeline::State::PLAYING);
}

TEST(PipelineOrchestratorTest, RestartNonexistentPipelineFails) {
    PipelineOrchestrator orch;
    auto result = orch.restart_pipeline("no_such");
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
}

// ============================================================
// Test: all_status — unified status query
// ============================================================

TEST(PipelineOrchestratorTest, AllStatusReturnsCorrectInfo) {
    PipelineOrchestrator orch;
    auto p1 = std::make_shared<StubPipeline>();
    auto p2 = std::make_shared<StubPipeline>();
    auto hm = std::make_shared<StubHealthMonitor>();

    p1->set_encoder_name("x264enc");
    p2->set_encoder_name("v4l2h264enc");
    hm->set_restart_count(3);

    orch.add_pipeline("cam1", PipelineConfig{}, p1, hm);
    orch.add_pipeline("cam2", PipelineConfig{}, p2);

    orch.start_all();

    auto statuses = orch.all_status();
    ASSERT_EQ(statuses.size(), 2u);

    // Find cam1 and cam2 in the results (map is ordered)
    const auto& s1 = statuses[0];  // cam1
    const auto& s2 = statuses[1];  // cam2

    EXPECT_EQ(s1.pipeline_id, "cam1");
    EXPECT_EQ(s1.state, IGStreamerPipeline::State::PLAYING);
    EXPECT_EQ(s1.restart_count, 3u);
    EXPECT_EQ(s1.encoder_name, "x264enc");

    EXPECT_EQ(s2.pipeline_id, "cam2");
    EXPECT_EQ(s2.state, IGStreamerPipeline::State::PLAYING);
    EXPECT_EQ(s2.restart_count, 0u);  // no health monitor
    EXPECT_EQ(s2.encoder_name, "v4l2h264enc");
}

TEST(PipelineOrchestratorTest, AllStatusEmptyWhenNoPipelines) {
    PipelineOrchestrator orch;
    auto statuses = orch.all_status();
    EXPECT_TRUE(statuses.empty());
}

// ============================================================
// Test: Health monitor integration
// ============================================================

TEST(PipelineOrchestratorTest, HealthMonitorStartedAndStopped) {
    PipelineOrchestrator orch;
    auto p1 = std::make_shared<StubPipeline>();
    auto hm = std::make_shared<StubHealthMonitor>();

    orch.add_pipeline("cam1", PipelineConfig{}, p1, hm);

    orch.start_all();
    EXPECT_TRUE(hm->is_running());

    orch.stop_all();
    EXPECT_FALSE(hm->is_running());
}

}  // namespace
}  // namespace sc
