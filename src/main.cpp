// Smart Camera — main entry point
// Initializes all modules in dependency order, registers shutdown steps,
// and runs the main event loop until a signal requests shutdown.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef HAS_GSTREAMER
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#endif

#include "ai/ai_pipeline.h"
#include "auth/iot_authenticator.h"
#include "buffer/frame_buffer_pool.h"
#include "camera/camera_source.h"
#include "config/config_manager.h"
#include "control/bitrate_controller.h"
#include "core/resource_guard.h"
#include "core/shutdown_handler.h"
#include "core/types.h"
#include "logging/log_manager.h"
#include "monitor/connection_monitor.h"
#include "monitor/health_monitor.h"
#include "monitor/watchdog.h"
#include "pipeline/gstreamer_pipeline.h"
#include "pipeline/pipeline_orchestrator.h"
#include "pipeline/video_producer.h"
#include "stream/stream_uploader.h"
#include "webrtc/webrtc_agent.h"

namespace sc {

static void print_version() {
    std::cerr << "smart-camera v" << PROJECT_VERSION
              << " (" << GIT_COMMIT_HASH << ")" << std::endl;
}

static void log_init_step(const std::string& step) {
    std::cerr << "[init] " << step << std::endl;
}

static void log_init_error(const std::string& step, const Error& err) {
    std::cerr << "[init] FAILED: " << step << " — " << err.message;
    if (!err.context.empty()) {
        std::cerr << " (" << err.context << ")";
    }
    std::cerr << std::endl;
}

}  // namespace sc

int main(int argc, char* argv[]) {
    using namespace sc;

    // ── Version banner ──────────────────────────────────────
    print_version();

    // ── 1. Config_Manager ───────────────────────────────────
    log_init_step("Config_Manager");
    auto config_mgr = std::make_unique<ConfigManager>();

    // Load config file if --config is provided, otherwise use defaults
    AppConfig config{};
    std::string config_path;
    bool webrtc_only = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--webrtc-only") {
            webrtc_only = true;
        }
    }

    if (!config_path.empty()) {
        auto load_res = config_mgr->load(config_path);
        if (load_res.is_err()) {
            log_init_error("Config_Manager load", load_res.error());
            return EXIT_FAILURE;
        }
        config = std::move(load_res).value();
    }

    auto cli_res = config_mgr->apply_cli_overrides(config, argc, argv);
    if (cli_res.is_err()) {
        log_init_error("Config_Manager CLI overrides", cli_res.error());
        return EXIT_FAILURE;
    }
    config = std::move(cli_res).value();

    auto validate_res = config_mgr->validate(config);
    if (validate_res.is_err()) {
        log_init_error("Config_Manager validate", validate_res.error());
        return EXIT_FAILURE;
    }

    // Populate WebRTC IoT cert fields from IoT config
    config.webrtc.iot_credential_endpoint = config.iot.credential_endpoint;
    config.webrtc.iot_cert_path = config.iot.cert_path;
    config.webrtc.iot_key_path = config.iot.key_path;
    config.webrtc.iot_ca_cert_path = config.iot.root_ca_path;
    config.webrtc.iot_role_alias = config.iot.role_alias;
    config.webrtc.iot_thing_name = config.iot.thing_name;

    // ── 2. Log_Manager ──────────────────────────────────────
    log_init_step("Log_Manager");
    auto log_mgr = std::make_shared<LogManager>();
    {
        auto res = log_mgr->initialize(
            config.log_file_path,
            log_level_from_string(config.log_level),
            config.log_max_file_size_mb,
            config.log_max_files,
            config.log_to_stdout);
        if (res.is_err()) {
            log_init_error("Log_Manager", res.error());
            return EXIT_FAILURE;
        }
    }
    log_mgr->log(LogLevel::INFO, "main", "Smart Camera starting v" +
        std::string(PROJECT_VERSION) + " (" + GIT_COMMIT_HASH + ")");

    // ── 3. IoT_Authenticator ────────────────────────────────
    log_init_step("IoT_Authenticator");
    auto iot_auth = std::make_shared<IoTAuthenticator>();
    {
        auto res = iot_auth->initialize(config.iot);
        if (res.is_err()) {
            log_mgr->log(LogLevel::ERROR, "main",
                "IoT_Authenticator init failed: " + res.error().message);
            // In development profile, allow continuing without IoT certs
            if (config.profile == "production") {
                return EXIT_FAILURE;
            }
            log_mgr->log(LogLevel::WARNING, "main",
                "Continuing without IoT authentication (non-production profile)");
        }
    }

    // ── WebRTC-only mode ───────────────────────────────────
    // When --webrtc-only is passed, skip GStreamer/KVS/monitors and only
    // run WebRTC signaling for debugging. Prints detailed step-by-step logs.
    if (webrtc_only) {
        log_mgr->log(LogLevel::INFO, "main", "=== WebRTC-only debug mode ===");
        std::cerr << "[webrtc-only] Channel:  " << config.webrtc.channel_name << std::endl;
        std::cerr << "[webrtc-only] Region:   " << config.webrtc.region << std::endl;
        std::cerr << "[webrtc-only] Thing:    " << config.iot.thing_name << std::endl;
        std::cerr << "[webrtc-only] Cert:     " << config.iot.cert_path << std::endl;
        std::cerr << "[webrtc-only] Key:      " << config.iot.key_path << std::endl;
        std::cerr << "[webrtc-only] CA:       " << config.iot.root_ca_path << std::endl;
        std::cerr << "[webrtc-only] Endpoint: " << config.iot.credential_endpoint << std::endl;
        std::cerr << "[webrtc-only] Role:     " << config.iot.role_alias << std::endl;

        log_init_step("WebRTC_Agent (webrtc-only)");
        auto webrtc = std::shared_ptr<IWebRTCAgent>(
            create_webrtc_agent().release());
        {
            auto res = webrtc->initialize(config.webrtc, iot_auth);
            if (res.is_err()) {
                std::cerr << "[webrtc-only] FAILED: WebRTC init — " << res.error().message << std::endl;
                return EXIT_FAILURE;
            }
            std::cerr << "[webrtc-only] OK: WebRTC initialized" << std::endl;
        }

        {
            auto res = webrtc->start_signaling();
            if (res.is_err()) {
                std::cerr << "[webrtc-only] FAILED: start_signaling — " << res.error().message << std::endl;
                return EXIT_FAILURE;
            }
            std::cerr << "[webrtc-only] OK: signaling started, connected="
                      << (webrtc->is_signaling_connected() ? "true" : "false") << std::endl;
        }

        std::cerr << "[webrtc-only] Waiting for viewer connections... (Ctrl+C to exit)" << std::endl;

        // Register signal handler for clean shutdown
        ShutdownHandler shutdown_handler;
        shutdown_handler.register_signal_handlers();

        while (!ShutdownHandler::shutdown_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::cerr << "[webrtc-only] heartbeat: connected="
                      << (webrtc->is_signaling_connected() ? "true" : "false")
                      << " viewers=" << webrtc->active_viewer_count() << std::endl;
        }

        std::cerr << "[webrtc-only] Shutting down..." << std::endl;
        webrtc->stop_signaling();
        std::cerr << "[webrtc-only] Done." << std::endl;
        return EXIT_SUCCESS;
    }

    // ── 4. Camera_Source ────────────────────────────────────
    log_init_step("Camera_Source");
    auto cam_res = create_camera_source(config.camera_source, config.camera_device_path);
    if (cam_res.is_err()) {
        log_mgr->log(LogLevel::ERROR, "main",
            "Camera_Source init failed: " + cam_res.error().message);
        return EXIT_FAILURE;
    }
    auto camera = std::move(cam_res).value();

    // ── 5. GStreamer_Pipeline ───────────────────────────────
    log_init_step("GStreamer_Pipeline");
    auto gst_pipeline = std::shared_ptr<IGStreamerPipeline>(
        create_gstreamer_pipeline().release());
    {
        PipelineConfig pipe_cfg;
        pipe_cfg.source_type = config.camera_source;
        pipe_cfg.device_path = config.camera_device_path;
        pipe_cfg.video_preset = config.video_preset;

        // Configure KVS upload if IoT certs and stream name are available
        if (!config.kvs.stream_name.empty() && !config.iot.cert_path.empty()) {
            pipe_cfg.kvs_enabled = true;
            pipe_cfg.kvs_stream_name = config.kvs.stream_name;
            pipe_cfg.kvs_retention_hours = config.kvs.retention_hours;
            pipe_cfg.kvs_region = config.kvs.region;
            pipe_cfg.iot_thing_name = config.iot.thing_name;
            pipe_cfg.iot_credential_endpoint = config.iot.credential_endpoint;
            pipe_cfg.iot_cert_path = config.iot.cert_path;
            pipe_cfg.iot_key_path = config.iot.key_path;
            pipe_cfg.iot_ca_path = config.iot.root_ca_path;
            pipe_cfg.iot_role_alias = config.iot.role_alias;
            log_mgr->log(LogLevel::INFO, "main",
                "KVS upload enabled: stream=" + config.kvs.stream_name);
        }

        auto res = gst_pipeline->build(pipe_cfg);
        if (res.is_err()) {
            log_mgr->log(LogLevel::ERROR, "main",
                "GStreamer_Pipeline build failed: " + res.error().message);
            return EXIT_FAILURE;
        }
    }

    // ── 6. Stream_Uploader ──────────────────────────────────
    log_init_step("Stream_Uploader");
    auto uploader = std::make_shared<StreamUploader>();
    {
        auto res = uploader->initialize(config.kvs, iot_auth);
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "main",
                "Stream_Uploader init failed: " + res.error().message);
        }
    }

    // ── 7. WebRTC_Agent ─────────────────────────────────────
    log_init_step("WebRTC_Agent");
    auto webrtc = std::shared_ptr<IWebRTCAgent>(
        create_webrtc_agent().release());
    {
        auto res = webrtc->initialize(config.webrtc, iot_auth);
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "main",
                "WebRTC_Agent init failed: " + res.error().message);
        }
    }

    // ── 8. Bitrate_Controller ───────────────────────────────
    log_init_step("Bitrate_Controller");
    auto bitrate_ctrl = std::make_unique<BitrateController>();
    {
        auto res = bitrate_ctrl->start(
            config.video_preset.bitrate_kbps,
            config.bitrate_min_kbps,
            config.bitrate_max_kbps);
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "main",
                "Bitrate_Controller start failed: " + res.error().message);
        }
    }
    // Wire bitrate adjustments to the pipeline
    bitrate_ctrl->on_adjustment([&gst_pipeline, &log_mgr](const BitrateAdjustment& adj) {
        auto res = gst_pipeline->set_bitrate(adj.new_bitrate_kbps);
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "BitrateController",
                "Failed to apply bitrate: " + res.error().message);
        }
    });

    // ── 9. Connection_Monitor ───────────────────────────────
    log_init_step("Connection_Monitor");
    auto conn_monitor = std::make_unique<ConnectionMonitor>();
    {
        std::string kvs_ep = "https://kinesisvideo." + config.kvs.region + ".amazonaws.com";
        std::string webrtc_ep = "https://kinesisvideo." + config.webrtc.region + ".amazonaws.com";
        auto res = conn_monitor->start(kvs_ep, webrtc_ep);
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "main",
                "Connection_Monitor start failed: " + res.error().message);
        }
    }

    // ── 10. Health_Monitor ──────────────────────────────────
    log_init_step("Health_Monitor");
    auto health_mon = std::make_shared<HealthMonitor>();
    {
        auto res = health_mon->start(gst_pipeline);
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "main",
                "Health_Monitor start failed: " + res.error().message);
        }
    }

    // ── 11. Watchdog ────────────────────────────────────────
    log_init_step("Watchdog");
    WatchdogConfig wd_cfg;
    wd_cfg.check_interval = std::chrono::seconds(config.watchdog_interval_sec);
    wd_cfg.error_threshold_per_min = config.watchdog_error_threshold;
    wd_cfg.stale_timeout = std::chrono::seconds(config.watchdog_stale_timeout_sec);
    wd_cfg.memory_limit_bytes = config.memory_limit_mb * 1024ULL * 1024ULL;
    auto watchdog = std::make_unique<Watchdog>(wd_cfg);
    {
        auto res = watchdog->start();
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "main",
                "Watchdog start failed: " + res.error().message);
        }
    }

    // ── 12. Frame_Buffer_Pool ───────────────────────────────
    log_init_step("Frame_Buffer_Pool");
    auto frame_pool = std::make_shared<FrameBufferPool>();
    {
        auto res = frame_pool->initialize(
            config.frame_pool_size,
            config.video_preset.width,
            config.video_preset.height);
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "main",
                "Frame_Buffer_Pool init failed: " + res.error().message);
        }
    }

    // ── 13. AI_Pipeline ─────────────────────────────────────
    log_init_step("AI_Pipeline");
    auto ai_pipeline = std::make_unique<AIPipeline>(
        std::chrono::seconds(config.ai_model_timeout_sec));
    {
        auto res = ai_pipeline->start(frame_pool);
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "main",
                "AI_Pipeline start failed: " + res.error().message);
        }
    }

    // ── 14. Pipeline_Orchestrator ───────────────────────────
    log_init_step("Pipeline_Orchestrator");
    auto orchestrator = std::make_unique<PipelineOrchestrator>();
    {
        auto res = orchestrator->add_pipeline("default",
            PipelineConfig{
                config.camera_source,
                config.camera_device_path,
                config.video_preset,
            },
            gst_pipeline,
            health_mon);
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "main",
                "Pipeline_Orchestrator add_pipeline failed: " + res.error().message);
        }
    }

    // ── Shutdown Handler ────────────────────────────────────
    log_init_step("ShutdownHandler");

    // Frame pull thread state — declared here so shutdown lambdas can capture them
    std::atomic<bool> frame_pull_running{false};
    std::unique_ptr<std::thread> frame_pull_thread;

    ShutdownHandler shutdown_handler;
    {
        auto res = shutdown_handler.register_signal_handlers();
        if (res.is_err()) {
            log_mgr->log(LogLevel::ERROR, "main",
                "Failed to register signal handlers: " + res.error().message);
            return EXIT_FAILURE;
        }
    }

    // Register shutdown steps in reverse initialization order
    shutdown_handler.add_step("Pipeline_Orchestrator", [&]() {
        orchestrator->stop_all();
        return true;
    });
    shutdown_handler.add_step("AI_Pipeline", [&]() {
        ai_pipeline->stop();
        return true;
    });
    shutdown_handler.add_step("Frame_Buffer_Pool", [&]() {
        // Pool is cleaned up via shared_ptr; nothing explicit needed
        return true;
    });
    shutdown_handler.add_step("Watchdog", [&]() {
        watchdog->stop();
        return true;
    });
    shutdown_handler.add_step("Health_Monitor", [&]() {
        health_mon->stop();
        return true;
    });
    shutdown_handler.add_step("Connection_Monitor", [&]() {
        conn_monitor->stop();
        return true;
    });
    shutdown_handler.add_step("Bitrate_Controller", [&]() {
        bitrate_ctrl->stop();
        return true;
    });
    shutdown_handler.add_step("WebRTC_FramePull", [&]() {
        // Stop frame pull thread BEFORE stopping WebRTC signaling
        // to ensure no frames are sent to a closed WebRTC_Agent
        frame_pull_running.store(false);
        if (frame_pull_thread && frame_pull_thread->joinable()) {
            frame_pull_thread->join();
        }
        return true;
    });
    shutdown_handler.add_step("WebRTC_Agent", [&]() {
        webrtc->stop_signaling();
        return true;
    });
    shutdown_handler.add_step("Stream_Uploader", [&]() {
        uploader->flush_buffer();
        uploader->stop();
        return true;
    });
    shutdown_handler.add_step("GStreamer_Pipeline", [&]() {
        gst_pipeline->stop();
        gst_pipeline->destroy();
        return true;
    });
    shutdown_handler.add_step("Camera_Source", [&]() {
        camera->close();
        return true;
    });

    // ── Start pipeline ──────────────────────────────────────
    {
        auto res = gst_pipeline->start();
        if (res.is_err()) {
            log_mgr->log(LogLevel::ERROR, "main",
                "GStreamer_Pipeline start failed: " + res.error().message);
            return EXIT_FAILURE;
        }
    }

    // ── Start WebRTC signaling ──────────────────────────────
    {
        auto res = webrtc->start_signaling();
        if (res.is_err()) {
            log_mgr->log(LogLevel::WARNING, "main",
                "WebRTC signaling start failed: " + res.error().message);
        } else {
            log_mgr->log(LogLevel::INFO, "main", "WebRTC signaling started");
        }
    }

    // ── Frame pull thread (GStreamer appsink → WebRTC) ──────
#ifdef HAS_GSTREAMER
    {
        // Get the webrtc_sink appsink element from the pipeline
        GstElement* pipeline_element = gst_pipeline->get_pipeline_element();
        GstElement* appsink = nullptr;
        if (pipeline_element) {
            appsink = gst_bin_get_by_name(GST_BIN(pipeline_element), "webrtc_sink");
        }

        if (appsink && GST_IS_APP_SINK(appsink)) {
            frame_pull_running.store(true);
            frame_pull_thread = std::make_unique<std::thread>(
                [&webrtc, &log_mgr, &frame_pull_running, appsink]() {
                    log_mgr->log(LogLevel::INFO, "main", "Frame pull thread started");

                    while (frame_pull_running.load() &&
                           !ShutdownHandler::shutdown_requested()) {
                        // Skip frame pull when no viewers are connected (save CPU)
                        if (webrtc->active_viewer_count() == 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            continue;
                        }

                        // Pull sample from appsink (100ms timeout)
                        GstSample* sample = gst_app_sink_try_pull_sample(
                            GST_APP_SINK(appsink), 100 * GST_MSECOND);
                        if (!sample) {
                            continue;
                        }

                        GstBuffer* buffer = gst_sample_get_buffer(sample);
                        if (buffer) {
                            GstMapInfo map;
                            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                                uint64_t timestamp_us = 0;
                                if (GST_BUFFER_PTS_IS_VALID(buffer)) {
                                    timestamp_us = GST_BUFFER_PTS(buffer) / 1000;  // ns → µs
                                }

                                auto res = webrtc->send_frame(
                                    map.data, map.size, timestamp_us);
                                if (res.is_err()) {
                                    log_mgr->log(LogLevel::WARNING, "main",
                                        "send_frame failed: " + res.error().message);
                                }

                                // RAII: always unmap
                                gst_buffer_unmap(buffer, &map);
                            }
                        }

                        gst_sample_unref(sample);
                    }

                    log_mgr->log(LogLevel::INFO, "main", "Frame pull thread stopped");
                });

            log_mgr->log(LogLevel::INFO, "main",
                "Frame pull thread launched for webrtc_sink appsink");
        } else {
            log_mgr->log(LogLevel::WARNING, "main",
                "webrtc_sink appsink not found — frame pull thread not started");
        }

        // Release our ref to appsink (pipeline still holds one)
        if (appsink) {
            gst_object_unref(appsink);
        }
    }
#else
    log_mgr->log(LogLevel::INFO, "main",
        "GStreamer not available — frame pull thread disabled");
#endif

    log_mgr->log(LogLevel::INFO, "main", "All modules initialized — entering main loop");
    std::cerr << "[init] All modules initialized — entering main loop" << std::endl;

    // ── Main event loop ─────────────────────────────────────
    // Simple sleep loop — GStreamer pipeline runs on its own threads.
    // kvssink handles putMedia internally, no GMainLoop needed.
    while (!ShutdownHandler::shutdown_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // ── Graceful shutdown ───────────────────────────────────
    log_mgr->log(LogLevel::INFO, "main", "Shutdown requested — initiating graceful shutdown");
    auto summary = shutdown_handler.initiate_shutdown();

    // Log shutdown summary
    for (const auto& step : summary.steps) {
        std::string status;
        switch (step.result) {
            case ShutdownStepResult::Success: status = "OK"; break;
            case ShutdownStepResult::Timeout: status = "TIMEOUT"; break;
            case ShutdownStepResult::Error:   status = "ERROR"; break;
            case ShutdownStepResult::Skipped: status = "SKIPPED"; break;
        }
        log_mgr->log(LogLevel::INFO, "shutdown",
            step.name + ": " + status + " (" +
            std::to_string(step.duration.count()) + "ms)");
    }

    log_mgr->log(LogLevel::INFO, "main",
        "Shutdown complete (" + std::to_string(summary.total_duration.count()) + "ms)");

    return summary.timed_out ? EXIT_FAILURE : EXIT_SUCCESS;
}
