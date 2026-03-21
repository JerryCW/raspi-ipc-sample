#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "core/types.h"

namespace sc {

// ============================================================
// ShutdownStepStatus — result of a single shutdown step
// ============================================================

enum class ShutdownStepResult : uint8_t {
    Success = 0,
    Timeout = 1,
    Error = 2,
    Skipped = 3,
};

struct ShutdownStepStatus {
    std::string name;
    ShutdownStepResult result = ShutdownStepResult::Skipped;
    std::string error_message;
    std::chrono::milliseconds duration{0};
};

// ============================================================
// ShutdownSummary — tracks all steps' status
// ============================================================

struct ShutdownSummary {
    std::vector<ShutdownStepStatus> steps;
    std::chrono::milliseconds total_duration{0};
    bool timed_out = false;
    bool completed = false;
};

// ============================================================
// ShutdownHandler — graceful shutdown with signal handling
// ============================================================

class ShutdownHandler {
public:
    using ShutdownCallback = std::function<bool()>;  // returns true on success
    using PersistCallback = std::function<void()>;
    using ResetCallback = std::function<void()>;

    /// Construct with total shutdown timeout (default 15 seconds)
    explicit ShutdownHandler(
        std::chrono::seconds timeout = std::chrono::seconds(15));

    ~ShutdownHandler() = default;

    ShutdownHandler(const ShutdownHandler&) = delete;
    ShutdownHandler& operator=(const ShutdownHandler&) = delete;

    /// Register SIGINT and SIGTERM signal handlers.
    /// Sets the static shutdown_requested flag on signal receipt.
    VoidResult register_signal_handlers();

    /// Add a named shutdown step. Steps execute in registration order.
    void add_step(const std::string& name, ShutdownCallback callback);

    /// Initiate the shutdown sequence. Runs all steps in order.
    /// Returns the shutdown summary with each step's result.
    ShutdownSummary initiate_shutdown();

    /// Check if shutdown has been requested (via signal or manual trigger)
    static bool shutdown_requested();

    /// Manually request shutdown (for programmatic use)
    static void request_shutdown();

    /// Reset the shutdown flag (primarily for testing)
    static void reset();

    /// Block until shutdown completes or timeout expires.
    /// Returns the summary once done.
    ShutdownSummary wait_for_shutdown();

    /// Register callback to persist un-uploaded video data before shutdown
    void on_persist_cache(PersistCallback cb);

    /// Register callback to reset error/retry counters after stream recovery
    void on_stream_recovery(ResetCallback cb);

    /// Trigger stream recovery reset (called when stream reconnects)
    void notify_stream_recovery();

    /// Get the configured timeout
    std::chrono::seconds timeout() const;

    /// Check if shutdown is currently in progress
    bool is_shutting_down() const;

private:
    struct Step {
        std::string name;
        ShutdownCallback callback;
    };

    static void signal_handler(int signum);

    static std::atomic<bool> shutdown_requested_;

    std::chrono::seconds timeout_;
    std::atomic<bool> shutting_down_{false};

    mutable std::shared_mutex steps_mutex_;
    std::vector<Step> steps_;

    mutable std::shared_mutex summary_mutex_;
    ShutdownSummary summary_;
    std::atomic<bool> shutdown_complete_{false};

    std::mutex persist_mutex_;
    PersistCallback persist_callback_;

    std::mutex recovery_mutex_;
    ResetCallback recovery_callback_;
};

}  // namespace sc
