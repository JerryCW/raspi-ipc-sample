#include "pipeline/pipeline_orchestrator.h"

namespace sc {

VoidResult PipelineOrchestrator::add_pipeline(
    const std::string& id,
    const PipelineConfig& config,
    std::shared_ptr<IGStreamerPipeline> pipeline,
    std::shared_ptr<IHealthMonitor> monitor) {

    if (id.empty()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Pipeline ID must not be empty",
                       "PipelineOrchestrator::add_pipeline");
    }
    if (!pipeline) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Pipeline pointer must not be null",
                       "PipelineOrchestrator::add_pipeline");
    }

    std::unique_lock lock(mutex_);

    if (pipelines_.count(id)) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Pipeline with ID '" + id + "' already exists",
                       "PipelineOrchestrator::add_pipeline");
    }

    PipelineInstance instance;
    instance.pipeline_id = id;
    instance.config = config;
    instance.pipeline = std::move(pipeline);
    instance.health_monitor = std::move(monitor);

    pipelines_.emplace(id, std::move(instance));
    return OkVoid();
}

VoidResult PipelineOrchestrator::remove_pipeline(const std::string& id) {
    std::unique_lock lock(mutex_);

    auto it = pipelines_.find(id);
    if (it == pipelines_.end()) {
        return ErrVoid(ErrorCode::NotFound,
                       "Pipeline '" + id + "' not found",
                       "PipelineOrchestrator::remove_pipeline");
    }

    // Stop the pipeline before removing
    auto& inst = it->second;
    if (inst.pipeline) {
        inst.pipeline->stop();
    }

    pipelines_.erase(it);
    return OkVoid();
}

VoidResult PipelineOrchestrator::start_all() {
    std::shared_lock lock(mutex_);

    for (auto& [id, inst] : pipelines_) {
        if (!inst.pipeline) continue;

        auto result = inst.pipeline->start();
        if (result.is_err()) {
            // Log but continue — fault isolation: one failure doesn't block others
            continue;
        }

        // Start health monitor if present
        if (inst.health_monitor) {
            inst.health_monitor->start(inst.pipeline);
        }
    }

    return OkVoid();
}

VoidResult PipelineOrchestrator::stop_all() {
    std::shared_lock lock(mutex_);

    for (auto& [id, inst] : pipelines_) {
        if (inst.health_monitor) {
            inst.health_monitor->stop();
        }
        if (inst.pipeline) {
            inst.pipeline->stop();
        }
    }

    return OkVoid();
}

VoidResult PipelineOrchestrator::restart_pipeline(const std::string& pipeline_id) {
    std::shared_lock lock(mutex_);

    auto it = pipelines_.find(pipeline_id);
    if (it == pipelines_.end()) {
        return ErrVoid(ErrorCode::NotFound,
                       "Pipeline '" + pipeline_id + "' not found",
                       "PipelineOrchestrator::restart_pipeline");
    }

    auto& inst = it->second;
    if (!inst.pipeline) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Pipeline '" + pipeline_id + "' has null pipeline pointer",
                       "PipelineOrchestrator::restart_pipeline");
    }

    // Stop then start — independent of other pipelines
    auto stop_result = inst.pipeline->stop();
    if (stop_result.is_err()) {
        return stop_result;
    }

    auto start_result = inst.pipeline->start();
    if (start_result.is_err()) {
        return start_result;
    }

    return OkVoid();
}

std::vector<IPipelineOrchestrator::PipelineStatus>
PipelineOrchestrator::all_status() const {
    std::shared_lock lock(mutex_);

    std::vector<PipelineStatus> statuses;
    statuses.reserve(pipelines_.size());

    for (const auto& [id, inst] : pipelines_) {
        PipelineStatus status;
        status.pipeline_id = id;
        status.state = inst.pipeline ? inst.pipeline->current_state()
                                     : IGStreamerPipeline::State::NULL_STATE;
        status.restart_count = inst.health_monitor
                                   ? inst.health_monitor->restart_count_in_window()
                                   : 0;
        status.encoder_name = inst.pipeline ? inst.pipeline->encoder_name() : "";
        statuses.push_back(std::move(status));
    }

    return statuses;
}

size_t PipelineOrchestrator::pipeline_count() const {
    std::shared_lock lock(mutex_);
    return pipelines_.size();
}

}  // namespace sc
