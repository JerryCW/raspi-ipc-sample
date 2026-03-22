#include "core/shutdown_handler.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace sc {

// ============================================================
// Static members
// ============================================================

std::atomic<bool> ShutdownHandler::shutdown_requested_{false};

// ============================================================
// Signal handler (async-signal-safe: only sets atomic flag)
// ============================================================

void ShutdownHandler::signal_handler(int /*signum*/) {
    shutdown_requested_.store(true);
}

// ============================================================
// Constructor
// ============================================================

ShutdownHandler::ShutdownHandler(std::chrono::seconds timeout)
    : timeout_(timeout) {}

// ============================================================
// register_signal_handlers
// ============================================================

VoidResult ShutdownHandler::register_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = &ShutdownHandler::signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, nullptr) != 0) {
        return ErrVoid(ErrorCode::Unknown,
                       "Failed to register SIGINT handler",
                       "ShutdownHandler::register_signal_handlers");
    }
    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
        return ErrVoid(ErrorCode::Unknown,
                       "Failed to register SIGTERM handler",
                       "ShutdownHandler::register_signal_handlers");
    }

    return OkVoid();
}

// ============================================================
// add_step
// ============================================================

void ShutdownHandler::add_step(const std::string& name,
                                ShutdownCallback callback) {
    std::unique_lock lock(steps_mutex_);
    steps_.push_back({name, std::move(callback)});
}

// ============================================================
// initiate_shutdown
// ============================================================

ShutdownSummary ShutdownHandler::initiate_shutdown() {
    // Prevent re-entrant shutdown
    bool expected = false;
    if (!shutting_down_.compare_exchange_strong(expected, true)) {
        std::shared_lock lock(summary_mutex_);
        return summary_;
    }

    auto start_time = std::chrono::steady_clock::now();
    auto deadline = start_time + timeout_;

    ShutdownSummary summary;

    std::cerr << "[ShutdownHandler] INFO: Initiating graceful shutdown ("
              << timeout_.count() << "s timeout)" << std::endl;

    // Persist un-uploaded video data first
    {
        std::lock_guard lock(persist_mutex_);
        if (persist_callback_) {
            std::cerr << "[ShutdownHandler] INFO: Persisting un-uploaded video data"
                      << std::endl;
            persist_callback_();
        }
    }

    // Take a snapshot of steps
    std::vector<Step> steps_snapshot;
    {
        std::shared_lock lock(steps_mutex_);
        steps_snapshot = steps_;
    }

    // Execute each step in order
    for (const auto& step : steps_snapshot) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            // Timeout reached — mark remaining steps as skipped
            ShutdownStepStatus status;
            status.name = step.name;
            status.result = ShutdownStepResult::Skipped;
            status.error_message = "Global timeout reached";
            status.duration = std::chrono::milliseconds(0);
            summary.steps.push_back(std::move(status));

            std::cerr << "[ShutdownHandler] WARNING: Timeout reached, skipping step: "
                      << step.name << std::endl;
            continue;
        }

        ShutdownStepStatus status;
        status.name = step.name;

        auto step_start = std::chrono::steady_clock::now();

        std::cerr << "[ShutdownHandler] INFO: Executing shutdown step: "
                  << step.name << std::endl;

        try {
            bool success = step.callback();
            auto step_end = std::chrono::steady_clock::now();
            status.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                step_end - step_start);

            if (success) {
                status.result = ShutdownStepResult::Success;
                std::cerr << "[ShutdownHandler] INFO: Step completed: "
                          << step.name << " (" << status.duration.count()
                          << "ms)" << std::endl;
            } else {
                status.result = ShutdownStepResult::Error;
                status.error_message = "Step returned failure";
                std::cerr << "[ShutdownHandler] ERROR: Step failed: "
                          << step.name << " (" << status.duration.count()
                          << "ms)" << std::endl;
            }
        } catch (const std::exception& e) {
            auto step_end = std::chrono::steady_clock::now();
            status.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                step_end - step_start);
            status.result = ShutdownStepResult::Error;
            status.error_message = e.what();

            std::cerr << "[ShutdownHandler] ERROR: Step threw exception: "
                      << step.name << " - " << e.what() << std::endl;
        }

        // Check if this step caused us to exceed the deadline
        if (std::chrono::steady_clock::now() >= deadline) {
            status.result = ShutdownStepResult::Timeout;
            status.error_message = "Step exceeded global timeout";
            summary.timed_out = true;

            std::cerr << "[ShutdownHandler] WARNING: Global timeout exceeded during step: "
                      << step.name << std::endl;
        }

        summary.steps.push_back(std::move(status));
    }

    auto end_time = std::chrono::steady_clock::now();
    summary.total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);

    if (summary.total_duration > std::chrono::duration_cast<std::chrono::milliseconds>(timeout_)) {
        summary.timed_out = true;
    }

    summary.completed = true;

    // Log shutdown summary
    std::cerr << "[ShutdownHandler] INFO: Shutdown summary ("
              << summary.total_duration.count() << "ms total"
              << (summary.timed_out ? ", TIMED OUT" : "") << "):" << std::endl;

    for (const auto& s : summary.steps) {
        const char* result_str = "UNKNOWN";
        switch (s.result) {
            case ShutdownStepResult::Success: result_str = "SUCCESS"; break;
            case ShutdownStepResult::Timeout: result_str = "TIMEOUT"; break;
            case ShutdownStepResult::Error:   result_str = "ERROR";   break;
            case ShutdownStepResult::Skipped: result_str = "SKIPPED"; break;
        }
        std::cerr << "  [" << result_str << "] " << s.name
                  << " (" << s.duration.count() << "ms)";
        if (!s.error_message.empty()) {
            std::cerr << " - " << s.error_message;
        }
        std::cerr << std::endl;
    }

    // Store summary for wait_for_shutdown
    {
        std::unique_lock lock(summary_mutex_);
        summary_ = summary;
    }
    shutdown_complete_.store(true);

    return summary;
}

// ============================================================
// shutdown_requested / request_shutdown / reset
// ============================================================

bool ShutdownHandler::shutdown_requested() {
    return shutdown_requested_.load();
}

void ShutdownHandler::request_shutdown() {
    shutdown_requested_.store(true);
}

void ShutdownHandler::reset() {
    shutdown_requested_.store(false);
}

// ============================================================
// wait_for_shutdown
// ============================================================

ShutdownSummary ShutdownHandler::wait_for_shutdown() {
    auto deadline = std::chrono::steady_clock::now() + timeout_;

    // Wait for shutdown to be requested
    while (!shutdown_requested_.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // If shutdown was requested, run the sequence
    if (shutdown_requested_.load() && !shutdown_complete_.load()) {
        return initiate_shutdown();
    }

    // Return existing summary if already complete
    std::shared_lock lock(summary_mutex_);
    return summary_;
}

// ============================================================
// Persist and recovery callbacks
// ============================================================

void ShutdownHandler::on_persist_cache(PersistCallback cb) {
    std::lock_guard lock(persist_mutex_);
    persist_callback_ = std::move(cb);
}

void ShutdownHandler::on_stream_recovery(ResetCallback cb) {
    std::lock_guard lock(recovery_mutex_);
    recovery_callback_ = std::move(cb);
}

void ShutdownHandler::notify_stream_recovery() {
    std::lock_guard lock(recovery_mutex_);
    if (recovery_callback_) {
        std::cerr << "[ShutdownHandler] INFO: Stream recovery — resetting error counters"
                  << std::endl;
        recovery_callback_();
    }
}

// ============================================================
// Accessors
// ============================================================

std::chrono::seconds ShutdownHandler::timeout() const {
    return timeout_;
}

bool ShutdownHandler::is_shutting_down() const {
    return shutting_down_.load();
}

}  // namespace sc
