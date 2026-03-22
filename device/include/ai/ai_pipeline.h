#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "buffer/frame_buffer_pool.h"
#include "core/types.h"

namespace sc {

// ============================================================
// BoundingBox — normalized coordinates (0.0 - 1.0)
// ============================================================

struct BoundingBox {
    float x;       // top-left x
    float y;       // top-left y
    float width;
    float height;
};

// ============================================================
// AnalysisResult — single detection from a model
// ============================================================

struct AnalysisResult {
    std::string model_name;
    std::string detection_type;    // "person", "bird", etc.
    float confidence;              // 0.0 - 1.0
    BoundingBox bbox;
    std::chrono::system_clock::time_point timestamp;
    uint64_t frame_sequence;
};

// ============================================================
// IAIModel — interface for pluggable analysis models
// ============================================================

class IAIModel {
public:
    virtual ~IAIModel() = default;
    virtual std::string name() const = 0;
    virtual Result<std::vector<AnalysisResult>> analyze(
        const FrameBuffer& frame) = 0;
};

// ============================================================
// IAIPipeline — interface for AI analysis pipeline
// ============================================================

class IAIPipeline {
public:
    virtual ~IAIPipeline() = default;

    virtual VoidResult start(std::shared_ptr<IFrameBufferPool> pool) = 0;
    virtual VoidResult stop() = 0;

    // Dynamic model registration/unregistration
    virtual VoidResult register_model(std::shared_ptr<IAIModel> model) = 0;
    virtual VoidResult unregister_model(const std::string& model_name) = 0;

    virtual std::vector<std::string> registered_models() const = 0;
    virtual uint64_t frames_analyzed() const = 0;
    virtual uint64_t frames_skipped() const = 0;
};

// ============================================================
// AIPipeline — concrete implementation
// ============================================================

class AIPipeline : public IAIPipeline {
public:
    explicit AIPipeline(std::chrono::seconds model_timeout = std::chrono::seconds(5),
                        const std::string& result_log_path = "");
    ~AIPipeline() override;

    AIPipeline(const AIPipeline&) = delete;
    AIPipeline& operator=(const AIPipeline&) = delete;

    VoidResult start(std::shared_ptr<IFrameBufferPool> pool) override;
    VoidResult stop() override;

    VoidResult register_model(std::shared_ptr<IAIModel> model) override;
    VoidResult unregister_model(const std::string& model_name) override;

    std::vector<std::string> registered_models() const override;
    uint64_t frames_analyzed() const override;
    uint64_t frames_skipped() const override;

private:
    void worker_loop();
    void write_results_json(const std::vector<AnalysisResult>& results,
                            uint64_t frame_sequence);

    std::chrono::seconds model_timeout_;
    std::string result_log_path_;

    mutable std::shared_mutex models_mutex_;
    std::map<std::string, std::shared_ptr<IAIModel>> models_;

    std::shared_ptr<IFrameBufferPool> pool_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> frames_analyzed_{0};
    std::atomic<uint64_t> frames_skipped_{0};
};

}  // namespace sc
