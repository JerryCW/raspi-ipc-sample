#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "core/types.h"
#include "monitor/health_monitor.h"
#include "pipeline/gstreamer_pipeline.h"

namespace sc {

// ============================================================
// PipelineInstance — one managed pipeline + its health monitor
// ============================================================

struct PipelineInstance {
    std::string pipeline_id;
    PipelineConfig config;
    std::shared_ptr<IGStreamerPipeline> pipeline;
    std::shared_ptr<IHealthMonitor> health_monitor;
};

// ============================================================
// IPipelineOrchestrator — abstract interface
// ============================================================

class IPipelineOrchestrator {
public:
    virtual ~IPipelineOrchestrator() = default;

    virtual VoidResult start_all() = 0;
    virtual VoidResult stop_all() = 0;

    virtual VoidResult restart_pipeline(const std::string& pipeline_id) = 0;

    struct PipelineStatus {
        std::string pipeline_id;
        IGStreamerPipeline::State state;
        uint32_t restart_count;
        std::string encoder_name;
    };

    virtual VoidResult add_pipeline(const std::string& id,
                                    const PipelineConfig& config,
                                    std::shared_ptr<IGStreamerPipeline> pipeline,
                                    std::shared_ptr<IHealthMonitor> monitor = nullptr) = 0;

    virtual VoidResult remove_pipeline(const std::string& id) = 0;

    virtual std::vector<PipelineStatus> all_status() const = 0;

    virtual size_t pipeline_count() const = 0;
};

// ============================================================
// PipelineOrchestrator — concrete implementation
// ============================================================

class PipelineOrchestrator : public IPipelineOrchestrator {
public:
    PipelineOrchestrator() = default;
    ~PipelineOrchestrator() override = default;

    // Non-copyable, non-movable
    PipelineOrchestrator(const PipelineOrchestrator&) = delete;
    PipelineOrchestrator& operator=(const PipelineOrchestrator&) = delete;
    PipelineOrchestrator(PipelineOrchestrator&&) = delete;
    PipelineOrchestrator& operator=(PipelineOrchestrator&&) = delete;

    VoidResult add_pipeline(const std::string& id,
                            const PipelineConfig& config,
                            std::shared_ptr<IGStreamerPipeline> pipeline,
                            std::shared_ptr<IHealthMonitor> monitor = nullptr) override;

    VoidResult remove_pipeline(const std::string& id) override;

    VoidResult start_all() override;
    VoidResult stop_all() override;

    VoidResult restart_pipeline(const std::string& pipeline_id) override;

    std::vector<PipelineStatus> all_status() const override;

    size_t pipeline_count() const override;

private:
    mutable std::shared_mutex mutex_;
    std::map<std::string, PipelineInstance> pipelines_;
};

}  // namespace sc
