#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ai/ai_pipeline.h"
#include "buffer/frame_buffer_pool.h"

using namespace sc;

// ============================================================
// Helpers
// ============================================================

static FrameInfo make_info(uint64_t seq, uint32_t w = 640, uint32_t h = 480) {
    FrameInfo fi{};
    fi.width = w;
    fi.height = h;
    fi.stride = w * 3;
    fi.timestamp = std::chrono::steady_clock::now();
    fi.sequence_number = seq;
    return fi;
}

static std::vector<uint8_t> make_data(size_t size = 128) {
    return std::vector<uint8_t>(size, 0xAB);
}

// ============================================================
// Stub AI model — returns a fixed detection instantly
// ============================================================

class StubModel : public IAIModel {
public:
    explicit StubModel(const std::string& name,
                       const std::string& detection_type = "person",
                       float confidence = 0.95f)
        : name_(name), detection_type_(detection_type), confidence_(confidence) {}

    std::string name() const override { return name_; }

    Result<std::vector<AnalysisResult>> analyze(const FrameBuffer& frame) override {
        analyze_count_.fetch_add(1, std::memory_order_relaxed);
        AnalysisResult r;
        r.model_name = name_;
        r.detection_type = detection_type_;
        r.confidence = confidence_;
        r.bbox = {0.1f, 0.2f, 0.3f, 0.4f};
        r.timestamp = std::chrono::system_clock::now();
        r.frame_sequence = frame.info().sequence_number;
        return Result<std::vector<AnalysisResult>>::Ok({r});
    }

    int analyze_count() const { return analyze_count_.load(std::memory_order_relaxed); }

private:
    std::string name_;
    std::string detection_type_;
    float confidence_;
    std::atomic<int> analyze_count_{0};
};

// ============================================================
// Slow model — sleeps longer than timeout to trigger skip
// ============================================================

class SlowModel : public IAIModel {
public:
    explicit SlowModel(const std::string& name,
                       std::chrono::milliseconds delay = std::chrono::milliseconds(6000))
        : name_(name), delay_(delay) {}

    std::string name() const override { return name_; }

    Result<std::vector<AnalysisResult>> analyze(const FrameBuffer& /*frame*/) override {
        std::this_thread::sleep_for(delay_);
        return Result<std::vector<AnalysisResult>>::Ok({});
    }

private:
    std::string name_;
    std::chrono::milliseconds delay_;
};

// ============================================================
// Model registration / unregistration
// ============================================================

TEST(AIPipelineTest, RegisterModel) {
    AIPipeline pipeline;
    auto model = std::make_shared<StubModel>("detector_a");

    auto result = pipeline.register_model(model);
    EXPECT_TRUE(result.is_ok());

    auto names = pipeline.registered_models();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "detector_a");
}

TEST(AIPipelineTest, RegisterMultipleModels) {
    AIPipeline pipeline;
    EXPECT_TRUE(pipeline.register_model(std::make_shared<StubModel>("model_a")).is_ok());
    EXPECT_TRUE(pipeline.register_model(std::make_shared<StubModel>("model_b")).is_ok());

    auto names = pipeline.registered_models();
    EXPECT_EQ(names.size(), 2u);
}

TEST(AIPipelineTest, RegisterDuplicateFails) {
    AIPipeline pipeline;
    auto model1 = std::make_shared<StubModel>("same_name");
    auto model2 = std::make_shared<StubModel>("same_name");

    EXPECT_TRUE(pipeline.register_model(model1).is_ok());
    auto result = pipeline.register_model(model2);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(AIPipelineTest, RegisterNullFails) {
    AIPipeline pipeline;
    auto result = pipeline.register_model(nullptr);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(AIPipelineTest, UnregisterModel) {
    AIPipeline pipeline;
    pipeline.register_model(std::make_shared<StubModel>("to_remove"));

    auto result = pipeline.unregister_model("to_remove");
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(pipeline.registered_models().empty());
}

TEST(AIPipelineTest, UnregisterNonExistentFails) {
    AIPipeline pipeline;
    auto result = pipeline.unregister_model("does_not_exist");
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
}

// ============================================================
// No models — start/stop works, frames_analyzed stays 0
// ============================================================

TEST(AIPipelineTest, NoModelsRunsCleanly) {
    auto pool = std::make_shared<FrameBufferPool>();
    pool->initialize(5, 640, 480);

    // Submit a frame so the pool has something
    {
        auto acq = pool->acquire();
        ASSERT_TRUE(acq.is_ok());
        auto buf = std::move(acq).value();
        buf->reset(make_data(64), make_info(1));
        pool->submit(buf);
    }

    AIPipeline pipeline;
    auto start_result = pipeline.start(pool);
    EXPECT_TRUE(start_result.is_ok());

    // Let it run briefly with no models
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto stop_result = pipeline.stop();
    EXPECT_TRUE(stop_result.is_ok());

    EXPECT_EQ(pipeline.frames_analyzed(), 0u);
    EXPECT_EQ(pipeline.frames_skipped(), 0u);
}

// ============================================================
// Start/stop lifecycle
// ============================================================

TEST(AIPipelineTest, DoubleStartFails) {
    auto pool = std::make_shared<FrameBufferPool>();
    pool->initialize(5, 640, 480);

    AIPipeline pipeline;
    EXPECT_TRUE(pipeline.start(pool).is_ok());
    EXPECT_TRUE(pipeline.start(pool).is_err());

    pipeline.stop();
}

TEST(AIPipelineTest, StopWithoutStartFails) {
    AIPipeline pipeline;
    EXPECT_TRUE(pipeline.stop().is_err());
}

TEST(AIPipelineTest, StartWithNullPoolFails) {
    AIPipeline pipeline;
    auto result = pipeline.start(nullptr);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

// ============================================================
// Model timeout → frame skipped
// ============================================================

TEST(AIPipelineTest, ModelTimeoutSkipsFrame) {
    auto pool = std::make_shared<FrameBufferPool>();
    pool->initialize(5, 640, 480);

    // Use a very short timeout (200ms) so the test runs fast
    AIPipeline pipeline(std::chrono::seconds(1));

    // Register a slow model that takes 3 seconds
    auto slow = std::make_shared<SlowModel>("slow_model", std::chrono::milliseconds(3000));
    EXPECT_TRUE(pipeline.register_model(slow).is_ok());

    EXPECT_TRUE(pipeline.start(pool).is_ok());

    // Submit a frame for the slow model to process
    {
        auto acq = pool->acquire();
        ASSERT_TRUE(acq.is_ok());
        auto buf = std::move(acq).value();
        buf->reset(make_data(64), make_info(100));
        pool->submit(buf);
    }

    // Wait long enough for the timeout to trigger but not for the model to finish
    // The timeout is 1s, the model takes 3s, so after ~1.5s the frame should be skipped
    // But std::async future destructor blocks, so we need to wait for the model to finish too
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));

    pipeline.stop();

    EXPECT_GE(pipeline.frames_skipped(), 1u);
}

// ============================================================
// JSON output format validation
// ============================================================

TEST(AIPipelineTest, JsonOutputFormat) {
    // Create a temp file for results
    std::string tmp_path = "test_ai_results_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";

    auto pool = std::make_shared<FrameBufferPool>();
    pool->initialize(5, 640, 480);

    AIPipeline pipeline(std::chrono::seconds(5), tmp_path);

    auto model = std::make_shared<StubModel>("person_detector", "person", 0.92f);
    EXPECT_TRUE(pipeline.register_model(model).is_ok());

    EXPECT_TRUE(pipeline.start(pool).is_ok());

    // Submit a frame
    {
        auto acq = pool->acquire();
        ASSERT_TRUE(acq.is_ok());
        auto buf = std::move(acq).value();
        buf->reset(make_data(64), make_info(12345));
        pool->submit(buf);
    }

    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    pipeline.stop();

    // Read the output file
    std::ifstream ifs(tmp_path);
    ASSERT_TRUE(ifs.is_open()) << "Result log file should exist";

    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();
    ifs.close();

    // Validate JSON structure contains expected fields
    EXPECT_NE(content.find("\"timestamp\""), std::string::npos);
    EXPECT_NE(content.find("\"frame_sequence\""), std::string::npos);
    EXPECT_NE(content.find("\"models\""), std::string::npos);
    EXPECT_NE(content.find("\"person_detector\""), std::string::npos);
    EXPECT_NE(content.find("\"type\": \"person\""), std::string::npos);
    EXPECT_NE(content.find("\"confidence\""), std::string::npos);
    EXPECT_NE(content.find("\"bbox\""), std::string::npos);
    EXPECT_NE(content.find("\"x\""), std::string::npos);
    EXPECT_NE(content.find("\"y\""), std::string::npos);
    EXPECT_NE(content.find("\"width\""), std::string::npos);
    EXPECT_NE(content.find("\"height\""), std::string::npos);

    // Verify frame_sequence value
    EXPECT_NE(content.find("12345"), std::string::npos);

    // Cleanup
    std::remove(tmp_path.c_str());
}

// ============================================================
// Registered models list is sorted (map ordering)
// ============================================================

TEST(AIPipelineTest, RegisteredModelsListOrder) {
    AIPipeline pipeline;
    pipeline.register_model(std::make_shared<StubModel>("zebra_model"));
    pipeline.register_model(std::make_shared<StubModel>("alpha_model"));
    pipeline.register_model(std::make_shared<StubModel>("mid_model"));

    auto names = pipeline.registered_models();
    ASSERT_EQ(names.size(), 3u);
    // std::map is sorted by key
    EXPECT_EQ(names[0], "alpha_model");
    EXPECT_EQ(names[1], "mid_model");
    EXPECT_EQ(names[2], "zebra_model");
}

// ============================================================
// Model analyzes frames when registered
// ============================================================

TEST(AIPipelineTest, ModelAnalyzesFrames) {
    auto pool = std::make_shared<FrameBufferPool>();
    pool->initialize(5, 640, 480);

    AIPipeline pipeline(std::chrono::seconds(5));

    auto model = std::make_shared<StubModel>("fast_model");
    EXPECT_TRUE(pipeline.register_model(model).is_ok());

    EXPECT_TRUE(pipeline.start(pool).is_ok());

    // Submit a few frames
    for (uint64_t i = 1; i <= 3; ++i) {
        auto acq = pool->acquire();
        ASSERT_TRUE(acq.is_ok());
        auto buf = std::move(acq).value();
        buf->reset(make_data(64), make_info(i));
        pool->submit(buf);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    pipeline.stop();

    // At least some frames should have been analyzed
    EXPECT_GT(pipeline.frames_analyzed(), 0u);
    EXPECT_GT(model->analyze_count(), 0);
}
