#include "monitor/watchdog.h"

#include <algorithm>
#include <iostream>
#include <sstream>

#ifdef HAS_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

namespace sc {

// ============================================================
// Construction / Destruction
// ============================================================

Watchdog::Watchdog(WatchdogConfig config)
    : config_(std::move(config)) {}

Watchdog::~Watchdog() {
    if (running_.load()) {
        stop();
    }
}

// ============================================================
// start / stop
// ============================================================

VoidResult Watchdog::start() {
    if (running_.load()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Watchdog already running",
                       "Watchdog::start");
    }

    {
        std::unique_lock lock(mutex_);
        modules_.clear();
        process_restart_timestamps_.clear();
    }
    consecutive_auth_failures_.store(0);
    running_.store(true);
    return OkVoid();
}

VoidResult Watchdog::stop() {
    if (!running_.load()) {
        return OkVoid();
    }
    running_.store(false);
    return OkVoid();
}

// ============================================================
// heartbeat / report_error
// ============================================================

void Watchdog::heartbeat(const std::string& module_name) {
    std::unique_lock lock(mutex_);
    auto& state = modules_[module_name];
    state.last_heartbeat = std::chrono::steady_clock::now();
}

void Watchdog::report_error(const std::string& module_name) {
    std::unique_lock lock(mutex_);
    auto& state = modules_[module_name];
    auto now = std::chrono::steady_clock::now();
    state.error_timestamps.push_back(now);

    // Initialise heartbeat if this is the first interaction
    if (state.last_heartbeat == std::chrono::steady_clock::time_point{}) {
        state.last_heartbeat = now;
    }
}

// ============================================================
// Auth failure tracking
// ============================================================

void Watchdog::report_auth_failure() {
    consecutive_auth_failures_.fetch_add(1);
}

void Watchdog::reset_auth_failures() {
    consecutive_auth_failures_.store(0);
}

// ============================================================
// Memory usage reporting
// ============================================================

void Watchdog::set_memory_usage(const std::string& module_name, size_t bytes) {
    std::unique_lock lock(mutex_);
    modules_[module_name].memory_usage_bytes = bytes;
}

// ============================================================
// error_count — sliding window query
// ============================================================

uint32_t Watchdog::error_count(const std::string& module_name) const {
    std::shared_lock lock(mutex_);
    auto it = modules_.find(module_name);
    if (it == modules_.end()) return 0;

    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - config_.error_window;
    uint32_t count = 0;
    for (const auto& ts : it->second.error_timestamps) {
        if (ts >= cutoff) ++count;
    }
    return count;
}

// ============================================================
// Protection mode & restart count
// ============================================================

bool Watchdog::is_protection_mode() const {
    std::shared_lock lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    prune_process_restarts(now);
    return process_restart_timestamps_.size() >= config_.max_process_restarts;
}

uint32_t Watchdog::process_restart_count() const {
    std::shared_lock lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    prune_process_restarts(now);
    return static_cast<uint32_t>(process_restart_timestamps_.size());
}

void Watchdog::record_process_restart() {
    std::unique_lock lock(mutex_);
    process_restart_timestamps_.push_back(std::chrono::steady_clock::now());
}

void Watchdog::on_restart_request(RestartCallback cb) {
    std::lock_guard lock(callback_mutex_);
    restart_callback_ = std::move(cb);
}

// ============================================================
// module_health_snapshot
// ============================================================

std::vector<ModuleHealth> Watchdog::module_health_snapshot() const {
    std::shared_lock lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    std::vector<ModuleHealth> result;
    result.reserve(modules_.size());

    for (const auto& [name, state] : modules_) {
        auto cutoff = now - config_.error_window;
        uint32_t err_count = 0;
        for (const auto& ts : state.error_timestamps) {
            if (ts >= cutoff) ++err_count;
        }
        result.push_back(ModuleHealth{
            name, err_count, state.last_heartbeat, state.memory_usage_bytes});
    }
    return result;
}

// ============================================================
// check_modules — one health-check cycle
// ============================================================

void Watchdog::check_modules() {
    auto now = std::chrono::steady_clock::now();

    // Notify systemd watchdog heartbeat
    notify_systemd_watchdog();

    // Snapshot module states under read lock
    std::vector<std::pair<std::string, ModuleState>> snapshot;
    {
        std::shared_lock lock(mutex_);
        snapshot.reserve(modules_.size());
        for (const auto& [name, state] : modules_) {
            snapshot.emplace_back(name, state);
        }
    }

    // Prune errors in snapshot copies (non-mutating on real data, just for counting)
    for (auto& [name, state] : snapshot) {
        prune_errors(state, now);
    }

    // --- Check 1: Error rate threshold ---
    // Threshold: error_threshold_per_min errors/min over error_window (5 min)
    // = error_threshold_per_min * (error_window_minutes) total errors in window
    uint32_t window_minutes = static_cast<uint32_t>(config_.error_window.count() / 60);
    uint32_t total_threshold = config_.error_threshold_per_min * window_minutes;

    for (const auto& [name, state] : snapshot) {
        uint32_t err_count = static_cast<uint32_t>(state.error_timestamps.size());
        if (err_count > total_threshold) {
            std::ostringstream metrics;
            metrics << "error_count=" << err_count
                    << " threshold=" << total_threshold
                    << " window=" << config_.error_window.count() << "s";
            trigger_module_restart(name,
                                   "Error rate exceeded threshold",
                                   metrics.str());
        }
    }

    // --- Check 2: Stale heartbeat ---
    for (const auto& [name, state] : snapshot) {
        // Skip modules that have never sent a heartbeat (time_point is epoch)
        if (state.last_heartbeat == std::chrono::steady_clock::time_point{}) {
            continue;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - state.last_heartbeat);
        auto stale_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            config_.stale_timeout);
        if (elapsed > stale_ms) {
            std::ostringstream metrics;
            metrics << "last_heartbeat_age=" << elapsed.count() << "ms"
                    << " stale_timeout=" << stale_ms.count() << "ms";
            trigger_module_restart(name,
                                   "Heartbeat stale",
                                   metrics.str());
        }
    }

    // --- Check 3: Memory usage ---
    size_t total_memory = 0;
    for (const auto& [name, state] : snapshot) {
        total_memory += state.memory_usage_bytes;
    }

    size_t warning_threshold = static_cast<size_t>(
        static_cast<double>(config_.memory_limit_bytes) * 0.9);

    if (total_memory > config_.memory_limit_bytes) {
        std::ostringstream metrics;
        metrics << "total_memory=" << (total_memory / (1024 * 1024)) << "MB"
                << " limit=" << (config_.memory_limit_bytes / (1024 * 1024)) << "MB";
        std::cerr << "[Watchdog] CRITICAL: Memory limit exceeded. "
                  << metrics.str() << std::endl;
        trigger_process_restart("Memory limit exceeded", metrics.str());
    } else if (total_memory > warning_threshold) {
        std::cerr << "[Watchdog] WARNING: Memory usage at 90% of limit. "
                  << "total=" << (total_memory / (1024 * 1024)) << "MB"
                  << " limit=" << (config_.memory_limit_bytes / (1024 * 1024)) << "MB"
                  << " — triggering memory cleanup" << std::endl;
        // Memory cleanup: notify via restart callback with special reason
        {
            std::lock_guard lock(callback_mutex_);
            if (restart_callback_) {
                restart_callback_("__memory_cleanup__", "Memory at 90% of limit");
            }
        }
    }

    // --- Check 4: Auth failures ---
    uint32_t auth_failures = consecutive_auth_failures_.load();
    if (auth_failures > config_.auth_failure_limit) {
        std::ostringstream metrics;
        metrics << "consecutive_auth_failures=" << auth_failures
                << " limit=" << config_.auth_failure_limit;
        std::cerr << "[Watchdog] CRITICAL: IoT_Authenticator consecutive failures exceeded. "
                  << metrics.str() << std::endl;
        trigger_process_restart("IoT_Authenticator consecutive auth failures exceeded",
                                metrics.str());
    }

    // Prune actual error timestamps under write lock
    {
        std::unique_lock lock(mutex_);
        for (auto& [name, state] : modules_) {
            prune_errors(state, now);
        }
    }
}

// ============================================================
// trigger_module_restart
// ============================================================

void Watchdog::trigger_module_restart(const std::string& module_name,
                                      const std::string& reason,
                                      const std::string& metrics) {
    // Diagnostic log before restart
    std::cerr << "[Watchdog] Module restart: module=" << module_name
              << " reason=\"" << reason << "\""
              << " metrics={" << metrics << "}" << std::endl;

    std::lock_guard lock(callback_mutex_);
    if (restart_callback_) {
        restart_callback_(module_name, reason);
    }
}

// ============================================================
// trigger_process_restart
// ============================================================

void Watchdog::trigger_process_restart(const std::string& reason,
                                       const std::string& metrics) {
    // Check protection mode first
    {
        std::unique_lock lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        prune_process_restarts(now);

        if (process_restart_timestamps_.size() >= config_.max_process_restarts) {
            std::cerr << "[Watchdog] PROTECTION MODE: Process restart suppressed. "
                      << "reason=\"" << reason << "\" "
                      << "metrics={" << metrics << "} "
                      << "restarts_in_window="
                      << process_restart_timestamps_.size()
                      << std::endl;
            return;
        }

        // Record this restart
        process_restart_timestamps_.push_back(now);
    }

    // Diagnostic log
    std::cerr << "[Watchdog] Process restart: reason=\"" << reason << "\""
              << " metrics={" << metrics << "}" << std::endl;

    std::lock_guard lock(callback_mutex_);
    if (restart_callback_) {
        restart_callback_("__process__", reason);
    }
}

// ============================================================
// prune helpers
// ============================================================

void Watchdog::prune_errors(ModuleState& state,
                            std::chrono::steady_clock::time_point now) const {
    auto cutoff = now - config_.error_window;
    while (!state.error_timestamps.empty() &&
           state.error_timestamps.front() < cutoff) {
        state.error_timestamps.pop_front();
    }
}

void Watchdog::prune_process_restarts(
    std::chrono::steady_clock::time_point now) const {
    auto cutoff = now - config_.protection_window;
    process_restart_timestamps_.erase(
        std::remove_if(process_restart_timestamps_.begin(),
                       process_restart_timestamps_.end(),
                       [&cutoff](const auto& ts) { return ts < cutoff; }),
        process_restart_timestamps_.end());
}

// ============================================================
// systemd watchdog heartbeat
// ============================================================

void Watchdog::notify_systemd_watchdog() {
#ifdef HAS_SYSTEMD
    sd_notify(0, "WATCHDOG=1");
#endif
}

}  // namespace sc
