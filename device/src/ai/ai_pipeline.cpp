#include "ai/ai_pipeline.h"

#include <chrono>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace sc {

// ============================================================
// Construction / Destruction
// ============================================================

AIPipeline::AIPipeline(std::chrono::seconds model_timeout,
                       const std::string& result_log_path)
    : model_timeout_(model_timeout),
      result_log_path_(result_log_path) {}

AIPipeline::~AIPipeline() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
}

// ============================================================
// start / stop
// ============================================================

VoidResult AIPipeline::start(std::shared_ptr<IFrameBufferPool> pool) {
    if (running_.load(std::memory_order_acquire)) {
        return ErrVoid(ErrorCode::InvalidArgument, "AI pipeline already running", "AIPipeline");
    }
    if (!pool) {
        return ErrVoid(ErrorCode::InvalidArgument, "Frame buffer pool is null", "AIPipeline");
    }

    pool_ = std::move(pool);
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&AIPipeline::worker_loop, this);

    return OkVoid();
}

VoidResult AIPipeline::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return ErrVoid(ErrorCode::InvalidArgument, "AI pipeline not running", "AIPipeline");
    }

    running_.store(false, std::memory_order_release);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    pool_.reset();

    return OkVoid();
}

// ============================================================
// Model registration
// ============================================================

VoidResult AIPipeline::register_model(std::shared_ptr<IAIModel> model) {
    if (!model) {
        return ErrVoid(ErrorCode::InvalidArgument, "Model is null", "AIPipeline");
    }

    std::unique_lock lock(models_mutex_);
    const auto& name = model->name();
    if (models_.count(name) > 0) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Model already registered: " + name, "AIPipeline");
    }
    models_.emplace(name, std::move(model));
    return OkVoid();
}

VoidResult AIPipeline::unregister_model(const std::string& model_name) {
    std::unique_lock lock(models_mutex_);
    auto it = models_.find(model_name);
    if (it == models_.end()) {
        return ErrVoid(ErrorCode::NotFound,
                       "Model not found: " + model_name, "AIPipeline");
    }
    models_.erase(it);
    return OkVoid();
}

std::vector<std::string> AIPipeline::registered_models() const {
    std::shared_lock lock(models_mutex_);
    std::vector<std::string> names;
    names.reserve(models_.size());
    for (const auto& [name, _] : models_) {
        names.push_back(name);
    }
    return names;
}

uint64_t AIPipeline::frames_analyzed() const {
    return frames_analyzed_.load(std::memory_order_relaxed);
}

uint64_t AIPipeline::frames_skipped() const {
    return frames_skipped_.load(std::memory_order_relaxed);
}

// ============================================================
// Worker loop — consumes frames from pool, runs models
// ============================================================

void AIPipeline::worker_loop() {
    while (running_.load(std::memory_order_acquire)) {
        // Check if any models are registered
        {
            std::shared_lock lock(models_mutex_);
            if (models_.empty()) {
                // No models — sleep briefly and continue
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
        }

        // Try to consume a frame
        auto frame_result = pool_->consume_latest();
        if (frame_result.is_err()) {
            // No frame available — sleep briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto frame = std::move(frame_result).value();
        uint64_t seq = frame->info().sequence_number;
        bool any_timeout = false;
        std::vector<AnalysisResult> all_results;

        // Take a snapshot of current models
        std::vector<std::shared_ptr<IAIModel>> model_snapshot;
        {
            std::shared_lock lock(models_mutex_);
            model_snapshot.reserve(models_.size());
            for (const auto& [_, m] : models_) {
                model_snapshot.push_back(m);
            }
        }

        // Run each model with timeout
        for (const auto& model : model_snapshot) {
            auto future = std::async(std::launch::async, [&model, &frame]() {
                return model->analyze(*frame);
            });

            auto status = future.wait_for(model_timeout_);
            if (status == std::future_status::timeout) {
                // Model timed out — skip this frame for this model
                std::cerr << "[WARNING] AI model '" << model->name()
                          << "' timed out on frame " << seq << ", skipping\n";
                any_timeout = true;
                // Note: the async task may still be running; we detach by letting
                // the future destructor handle it (blocks until complete in C++14+,
                // but we accept this trade-off for simplicity)
            } else {
                auto result = future.get();
                if (result.is_ok()) {
                    auto& detections = result.value();
                    all_results.insert(all_results.end(),
                                       std::make_move_iterator(detections.begin()),
                                       std::make_move_iterator(detections.end()));
                }
            }
        }

        if (any_timeout) {
            frames_skipped_.fetch_add(1, std::memory_order_relaxed);
        } else {
            frames_analyzed_.fetch_add(1, std::memory_order_relaxed);
        }

        // Write results to JSON log
        if (!all_results.empty()) {
            write_results_json(all_results, seq);
        }
    }
}

// ============================================================
// JSON output — simple string formatting (no JSON library)
// ============================================================

void AIPipeline::write_results_json(const std::vector<AnalysisResult>& results,
                                     uint64_t frame_sequence) {
    if (result_log_path_.empty()) {
        return;
    }

    // Format timestamp as ISO 8601
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::ostringstream ts;
    ts << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%S");
    ts << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";

    // Build JSON manually
    std::ostringstream json;
    json << "{\n";
    json << "  \"timestamp\": \"" << ts.str() << "\",\n";
    json << "  \"frame_sequence\": " << frame_sequence << ",\n";

    // Group detections by model
    std::map<std::string, std::vector<const AnalysisResult*>> by_model;
    for (const auto& r : results) {
        by_model[r.model_name].push_back(&r);
    }

    json << "  \"models\": [";
    bool first_model = true;
    for (const auto& [model_name, detections] : by_model) {
        if (!first_model) json << ",";
        first_model = false;

        json << "\n    {\n";
        json << "      \"model\": \"" << model_name << "\",\n";
        json << "      \"detections\": [";

        bool first_det = true;
        for (const auto* det : detections) {
            if (!first_det) json << ",";
            first_det = false;

            json << "\n        {\n";
            json << "          \"type\": \"" << det->detection_type << "\",\n";
            json << "          \"confidence\": " << std::fixed << std::setprecision(2)
                 << det->confidence << ",\n";
            json << "          \"bbox\": {"
                 << "\"x\": " << std::setprecision(2) << det->bbox.x << ", "
                 << "\"y\": " << std::setprecision(2) << det->bbox.y << ", "
                 << "\"width\": " << std::setprecision(2) << det->bbox.width << ", "
                 << "\"height\": " << std::setprecision(2) << det->bbox.height
                 << "}\n";
            json << "        }";
        }
        json << "\n      ]\n";
        json << "    }";
    }
    json << "\n  ]\n";
    json << "}\n";

    // Append to log file
    std::ofstream ofs(result_log_path_, std::ios::app);
    if (ofs.is_open()) {
        ofs << json.str();
    }
}

}  // namespace sc
