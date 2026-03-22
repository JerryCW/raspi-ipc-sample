#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"

namespace sc {

// ============================================================
// ModuleHealth — per-module health snapshot
// ============================================================

struct ModuleHealth {
    std::string module_name;
    uint32_t error_count_5min = 0;
    std::chrono::steady_clock::time_point last_heartbeat;
    size_t memory_usage_bytes = 0;
};

// ============================================================
// IWatchdog — interface for process-level watchdog
// ============================================================

class IWatchdog {
public:
    virtual ~IWatchdog() = default;

    virtual VoidResult start() = 0;
    virtual VoidResult stop() = 0;

    virtual void heartbeat(const std::string& module_name) = 0;
    virtual void report_error(const std::string& module_name) = 0;

    virtual bool is_protection_mode() const = 0;
    virtual uint32_t process_restart_count() const = 0;

    using RestartCallback = std::function<void(const std::string& module_name,
                                               const std::string& reason)>;
    virtual void on_restart_request(RestartCallback cb) = 0;
};

// ============================================================
// WatchdogConfig — tuneable parameters
// ============================================================

struct WatchdogConfig {
    std::chrono::seconds check_interval{10};
    std::chrono::seconds error_window{300};          // 5 min sliding window
    uint32_t error_threshold_per_min{10};             // 10 errors/min → 50 in 5 min
    std::chrono::seconds stale_timeout{60};
    size_t memory_limit_bytes{512ULL * 1024 * 1024};  // 512 MB
    uint32_t auth_failure_limit{10};
    std::chrono::seconds protection_window{3600};     // 1 hour
    uint32_t max_process_restarts{3};                 // ≤3 in 1 hour
};

// ============================================================
// Watchdog — concrete implementation
// ============================================================

class Watchdog : public IWatchdog {
public:
    explicit Watchdog(WatchdogConfig config = {});
    ~Watchdog() override;

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    Watchdog(Watchdog&&) = delete;
    Watchdog& operator=(Watchdog&&) = delete;

    VoidResult start() override;
    VoidResult stop() override;

    void heartbeat(const std::string& module_name) override;
    void report_error(const std::string& module_name) override;

    bool is_protection_mode() const override;
    uint32_t process_restart_count() const override;

    void on_restart_request(RestartCallback cb) override;

    // ---- Public for testing ----

    /// Run one health-check cycle over all tracked modules
    void check_modules();

    /// Report consecutive auth failures for IoT_Authenticator
    void report_auth_failure();

    /// Reset auth failure counter (on success)
    void reset_auth_failures();

    /// Set memory usage for a module (for testing / external reporting)
    void set_memory_usage(const std::string& module_name, size_t bytes);

    /// Get current error count in the sliding window for a module
    uint32_t error_count(const std::string& module_name) const;

    /// Record a process-level restart (for testing)
    void record_process_restart();

    /// Get module health snapshot
    std::vector<ModuleHealth> module_health_snapshot() const;

private:
    // Internal per-module tracking
    struct ModuleState {
        std::chrono::steady_clock::time_point last_heartbeat;
        std::deque<std::chrono::steady_clock::time_point> error_timestamps;
        size_t memory_usage_bytes = 0;
    };

    void prune_errors(ModuleState& state,
                      std::chrono::steady_clock::time_point now) const;
    void prune_process_restarts(std::chrono::steady_clock::time_point now) const;

    void trigger_module_restart(const std::string& module_name,
                                const std::string& reason,
                                const std::string& metrics);
    void trigger_process_restart(const std::string& reason,
                                 const std::string& metrics);

    void notify_systemd_watchdog();

    WatchdogConfig config_;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, ModuleState> modules_;

    std::atomic<bool> running_{false};

    // Process-level restart tracking
    mutable std::vector<std::chrono::steady_clock::time_point> process_restart_timestamps_;

    // Auth failure tracking
    std::atomic<uint32_t> consecutive_auth_failures_{0};

    // Callback
    std::mutex callback_mutex_;
    RestartCallback restart_callback_;
};

}  // namespace sc
