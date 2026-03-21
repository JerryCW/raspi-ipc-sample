#include "monitor/watchdog.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace sc {
namespace {

// ============================================================
// Helpers
// ============================================================

/// Build a WatchdogConfig with short windows for fast tests
WatchdogConfig fast_config() {
    WatchdogConfig cfg;
    cfg.check_interval = std::chrono::seconds(1);
    cfg.error_window = std::chrono::seconds(5);       // 5 s sliding window
    cfg.error_threshold_per_min = 10;                  // 10/min → but window is 5s
    cfg.stale_timeout = std::chrono::seconds(2);       // 2 s stale
    cfg.memory_limit_bytes = 512ULL * 1024 * 1024;
    cfg.auth_failure_limit = 10;
    cfg.protection_window = std::chrono::seconds(5);   // 5 s protection window
    cfg.max_process_restarts = 3;
    return cfg;
}

// ============================================================
// Start / Stop lifecycle
// ============================================================

TEST(WatchdogTest, StartStop) {
    Watchdog wd;
    auto r1 = wd.start();
    ASSERT_TRUE(r1.is_ok());

    // Double start → error
    auto r2 = wd.start();
    EXPECT_TRUE(r2.is_err());

    auto r3 = wd.stop();
    EXPECT_TRUE(r3.is_ok());

    // Stop when already stopped → ok (idempotent)
    auto r4 = wd.stop();
    EXPECT_TRUE(r4.is_ok());
}

// ============================================================
// Heartbeat updates last_heartbeat
// ============================================================

TEST(WatchdogTest, HeartbeatUpdatesTimestamp) {
    Watchdog wd;
    wd.start();

    wd.heartbeat("Camera");
    auto snap1 = wd.module_health_snapshot();
    ASSERT_EQ(snap1.size(), 1u);
    EXPECT_EQ(snap1[0].module_name, "Camera");

    auto t1 = snap1[0].last_heartbeat;

    // Small delay then heartbeat again
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    wd.heartbeat("Camera");

    auto snap2 = wd.module_health_snapshot();
    ASSERT_EQ(snap2.size(), 1u);
    EXPECT_GT(snap2[0].last_heartbeat, t1);

    wd.stop();
}

// ============================================================
// Error rate sliding window calculation
// ============================================================

TEST(WatchdogTest, ErrorRateSlidingWindow) {
    // Use a 5-second error window, threshold 10/min
    // With 5s window that's effectively ~0.83 errors allowed
    // but the formula is: threshold_per_min * (window_seconds / 60)
    // = 10 * (5/60) = 0 (integer). So we use a config where
    // the window is large enough to be meaningful.
    WatchdogConfig cfg;
    cfg.error_window = std::chrono::seconds(300);  // 5 min
    cfg.error_threshold_per_min = 10;               // 10/min → 50 total in 5 min
    cfg.stale_timeout = std::chrono::seconds(600);  // large, won't trigger
    cfg.memory_limit_bytes = 1024ULL * 1024 * 1024; // 1 GB, won't trigger
    cfg.protection_window = std::chrono::seconds(3600);
    cfg.max_process_restarts = 3;

    Watchdog wd(cfg);
    wd.start();
    wd.heartbeat("Pipeline");

    // Report 50 errors (at threshold)
    for (int i = 0; i < 50; ++i) {
        wd.report_error("Pipeline");
    }
    EXPECT_EQ(wd.error_count("Pipeline"), 50u);

    // check_modules should NOT trigger restart at exactly 50 (threshold is 50)
    std::string restarted_module;
    wd.on_restart_request([&](const std::string& mod, const std::string&) {
        restarted_module = mod;
    });
    wd.check_modules();
    EXPECT_TRUE(restarted_module.empty())
        << "Should not restart at exactly threshold";

    // One more error → 51 > 50 → triggers restart
    wd.report_error("Pipeline");
    EXPECT_EQ(wd.error_count("Pipeline"), 51u);
    wd.check_modules();
    EXPECT_EQ(restarted_module, "Pipeline");

    wd.stop();
}

TEST(WatchdogTest, ErrorCountForUnknownModuleIsZero) {
    Watchdog wd;
    wd.start();
    EXPECT_EQ(wd.error_count("NonExistent"), 0u);
    wd.stop();
}

// ============================================================
// Stale heartbeat detection
// ============================================================

TEST(WatchdogTest, StaleHeartbeatDetection) {
    WatchdogConfig cfg;
    cfg.stale_timeout = std::chrono::seconds(0);  // immediate staleness
    cfg.error_window = std::chrono::seconds(300);
    cfg.error_threshold_per_min = 10;
    cfg.memory_limit_bytes = 1024ULL * 1024 * 1024;
    cfg.protection_window = std::chrono::seconds(3600);
    cfg.max_process_restarts = 3;

    Watchdog wd(cfg);
    wd.start();

    // Send heartbeat, then wait for it to become stale
    wd.heartbeat("Encoder");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::string restarted_module;
    std::string restart_reason;
    wd.on_restart_request([&](const std::string& mod, const std::string& reason) {
        restarted_module = mod;
        restart_reason = reason;
    });

    wd.check_modules();
    EXPECT_EQ(restarted_module, "Encoder");
    EXPECT_NE(restart_reason.find("stale"), std::string::npos)
        << "Reason should mention staleness: " << restart_reason;

    wd.stop();
}

TEST(WatchdogTest, HeartbeatRefreshPreventsStale) {
    WatchdogConfig cfg;
    cfg.stale_timeout = std::chrono::seconds(60);  // 60s — won't expire
    cfg.error_window = std::chrono::seconds(300);
    cfg.error_threshold_per_min = 10;
    cfg.memory_limit_bytes = 1024ULL * 1024 * 1024;
    cfg.protection_window = std::chrono::seconds(3600);
    cfg.max_process_restarts = 3;

    Watchdog wd(cfg);
    wd.start();
    wd.heartbeat("Camera");

    std::string restarted_module;
    wd.on_restart_request([&](const std::string& mod, const std::string&) {
        restarted_module = mod;
    });

    wd.check_modules();
    EXPECT_TRUE(restarted_module.empty())
        << "Fresh heartbeat should not trigger restart";

    wd.stop();
}

// ============================================================
// Protection mode (≥3 process restarts in 1 hour)
// ============================================================

TEST(WatchdogTest, ProtectionModeAfterThreeRestarts) {
    WatchdogConfig cfg;
    cfg.protection_window = std::chrono::seconds(3600);  // 1 hour
    cfg.max_process_restarts = 3;
    cfg.error_window = std::chrono::seconds(300);
    cfg.error_threshold_per_min = 10;
    cfg.stale_timeout = std::chrono::seconds(600);
    cfg.memory_limit_bytes = 1024ULL * 1024 * 1024;

    Watchdog wd(cfg);
    wd.start();

    EXPECT_FALSE(wd.is_protection_mode());
    EXPECT_EQ(wd.process_restart_count(), 0u);

    // Record 3 process restarts
    wd.record_process_restart();
    EXPECT_EQ(wd.process_restart_count(), 1u);
    EXPECT_FALSE(wd.is_protection_mode());

    wd.record_process_restart();
    EXPECT_EQ(wd.process_restart_count(), 2u);
    EXPECT_FALSE(wd.is_protection_mode());

    wd.record_process_restart();
    EXPECT_EQ(wd.process_restart_count(), 3u);
    EXPECT_TRUE(wd.is_protection_mode());

    wd.stop();
}

TEST(WatchdogTest, ProtectionModeSuppressesProcessRestart) {
    WatchdogConfig cfg;
    cfg.protection_window = std::chrono::seconds(3600);
    cfg.max_process_restarts = 3;
    cfg.memory_limit_bytes = 100;  // Very low → will trigger
    cfg.error_window = std::chrono::seconds(300);
    cfg.error_threshold_per_min = 10;
    cfg.stale_timeout = std::chrono::seconds(600);

    Watchdog wd(cfg);
    wd.start();

    // Pre-fill 3 restarts → protection mode
    wd.record_process_restart();
    wd.record_process_restart();
    wd.record_process_restart();
    ASSERT_TRUE(wd.is_protection_mode());

    // Set memory over limit
    wd.heartbeat("Pipeline");
    wd.set_memory_usage("Pipeline", 200);  // 200 bytes > 100 byte limit

    std::vector<std::string> restart_targets;
    wd.on_restart_request([&](const std::string& mod, const std::string&) {
        restart_targets.push_back(mod);
    });

    wd.check_modules();

    // In protection mode, process restart should be suppressed
    // (no "__process__" callback)
    for (const auto& t : restart_targets) {
        EXPECT_NE(t, "__process__")
            << "Process restart should be suppressed in protection mode";
    }

    wd.stop();
}

TEST(WatchdogTest, ProtectionModeExpiresAfterWindow) {
    WatchdogConfig cfg;
    cfg.protection_window = std::chrono::seconds(0);  // instant expiry
    cfg.max_process_restarts = 3;
    cfg.error_window = std::chrono::seconds(300);
    cfg.error_threshold_per_min = 10;
    cfg.stale_timeout = std::chrono::seconds(600);
    cfg.memory_limit_bytes = 1024ULL * 1024 * 1024;

    Watchdog wd(cfg);
    wd.start();

    wd.record_process_restart();
    wd.record_process_restart();
    wd.record_process_restart();

    // With 0s window, restarts should be pruned immediately
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(wd.is_protection_mode());
    EXPECT_EQ(wd.process_restart_count(), 0u);

    wd.stop();
}

// ============================================================
// Restart callback notification
// ============================================================

TEST(WatchdogTest, RestartCallbackNotification) {
    WatchdogConfig cfg;
    cfg.error_window = std::chrono::seconds(300);
    cfg.error_threshold_per_min = 1;  // 1/min → 5 total in 5 min window
    cfg.stale_timeout = std::chrono::seconds(600);
    cfg.memory_limit_bytes = 1024ULL * 1024 * 1024;
    cfg.protection_window = std::chrono::seconds(3600);
    cfg.max_process_restarts = 3;

    Watchdog wd(cfg);
    wd.start();
    wd.heartbeat("Uploader");

    std::string cb_module;
    std::string cb_reason;
    wd.on_restart_request([&](const std::string& mod, const std::string& reason) {
        cb_module = mod;
        cb_reason = reason;
    });

    // Report 6 errors (> 5 threshold)
    for (int i = 0; i < 6; ++i) {
        wd.report_error("Uploader");
    }

    wd.check_modules();
    EXPECT_EQ(cb_module, "Uploader");
    EXPECT_FALSE(cb_reason.empty());

    wd.stop();
}

// ============================================================
// Memory limit triggers process restart
// ============================================================

TEST(WatchdogTest, MemoryLimitTriggersProcessRestart) {
    WatchdogConfig cfg;
    cfg.memory_limit_bytes = 1000;  // 1000 bytes
    cfg.error_window = std::chrono::seconds(300);
    cfg.error_threshold_per_min = 10;
    cfg.stale_timeout = std::chrono::seconds(600);
    cfg.protection_window = std::chrono::seconds(3600);
    cfg.max_process_restarts = 3;

    Watchdog wd(cfg);
    wd.start();
    wd.heartbeat("Pipeline");
    wd.set_memory_usage("Pipeline", 1500);  // Over limit

    std::string cb_module;
    std::string cb_reason;
    wd.on_restart_request([&](const std::string& mod, const std::string& reason) {
        cb_module = mod;
        cb_reason = reason;
    });

    wd.check_modules();
    EXPECT_EQ(cb_module, "__process__");
    EXPECT_NE(cb_reason.find("Memory"), std::string::npos);

    wd.stop();
}

// ============================================================
// Memory warning at 90% triggers cleanup callback
// ============================================================

TEST(WatchdogTest, MemoryWarningTriggersCleanup) {
    WatchdogConfig cfg;
    cfg.memory_limit_bytes = 1000;
    cfg.error_window = std::chrono::seconds(300);
    cfg.error_threshold_per_min = 10;
    cfg.stale_timeout = std::chrono::seconds(600);
    cfg.protection_window = std::chrono::seconds(3600);
    cfg.max_process_restarts = 3;

    Watchdog wd(cfg);
    wd.start();
    wd.heartbeat("Pipeline");
    wd.set_memory_usage("Pipeline", 950);  // 95% > 90% but < 100%

    std::string cb_module;
    wd.on_restart_request([&](const std::string& mod, const std::string&) {
        cb_module = mod;
    });

    wd.check_modules();
    EXPECT_EQ(cb_module, "__memory_cleanup__");

    wd.stop();
}

// ============================================================
// Auth failure triggers process restart
// ============================================================

TEST(WatchdogTest, AuthFailureTriggersProcessRestart) {
    WatchdogConfig cfg;
    cfg.auth_failure_limit = 5;
    cfg.memory_limit_bytes = 1024ULL * 1024 * 1024;
    cfg.error_window = std::chrono::seconds(300);
    cfg.error_threshold_per_min = 10;
    cfg.stale_timeout = std::chrono::seconds(600);
    cfg.protection_window = std::chrono::seconds(3600);
    cfg.max_process_restarts = 3;

    Watchdog wd(cfg);
    wd.start();

    // Report 6 auth failures (> 5 limit)
    for (int i = 0; i < 6; ++i) {
        wd.report_auth_failure();
    }

    std::string cb_module;
    std::string cb_reason;
    wd.on_restart_request([&](const std::string& mod, const std::string& reason) {
        cb_module = mod;
        cb_reason = reason;
    });

    wd.check_modules();
    EXPECT_EQ(cb_module, "__process__");
    EXPECT_NE(cb_reason.find("auth"), std::string::npos);

    wd.stop();
}

TEST(WatchdogTest, AuthFailureResetPreventsRestart) {
    WatchdogConfig cfg;
    cfg.auth_failure_limit = 5;
    cfg.memory_limit_bytes = 1024ULL * 1024 * 1024;
    cfg.error_window = std::chrono::seconds(300);
    cfg.error_threshold_per_min = 10;
    cfg.stale_timeout = std::chrono::seconds(600);
    cfg.protection_window = std::chrono::seconds(3600);
    cfg.max_process_restarts = 3;

    Watchdog wd(cfg);
    wd.start();

    for (int i = 0; i < 4; ++i) {
        wd.report_auth_failure();
    }
    wd.reset_auth_failures();

    std::string cb_module;
    wd.on_restart_request([&](const std::string& mod, const std::string&) {
        cb_module = mod;
    });

    wd.check_modules();
    EXPECT_TRUE(cb_module.empty())
        << "Reset auth failures should prevent restart";

    wd.stop();
}

}  // namespace
}  // namespace sc
