#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/stream_mode.h"
#include "core/types.h"

namespace sc {

// ============================================================
// CameraSourceType — video source selection
// ============================================================

enum class CameraSourceType : uint8_t {
    LIBCAMERA_CSI = 0,
    V4L2_USB = 1,
    VIDEOTESTSRC = 2,
};

// ============================================================
// IoTCertConfig — IoT certificate paths and endpoint info
// ============================================================

struct IoTCertConfig {
    std::string cert_path;
    std::string key_path;
    std::string root_ca_path;
    std::string credential_endpoint;
    std::string role_alias;
    std::string thing_name;
};

// ============================================================
// KVSStreamConfig — KVS stream parameters
// ============================================================

struct KVSStreamConfig {
    std::string stream_name;
    std::string region;
    uint32_t retention_hours = 168;
    bool auto_create_stream = true;
};

// ============================================================
// WebRTCConfig — WebRTC signaling parameters
// ============================================================

struct WebRTCConfig {
    std::string channel_name;
    std::string region;
    uint32_t max_viewers = 10;
    std::vector<std::string> stun_urls;
    std::vector<std::string> turn_urls;
    std::string turn_username;
    std::string turn_password;

    // IoT cert paths for SDK credential provider (createLwsIotCredentialProvider)
    std::string iot_credential_endpoint;
    std::string iot_cert_path;
    std::string iot_key_path;
    std::string iot_ca_cert_path;
    std::string iot_role_alias;
    std::string iot_thing_name;
};

// ============================================================
// AISummaryConfig — AI video summary parameters
// ============================================================

struct AISummaryConfig {
    // FrameExporter
    double export_fps = 2.0;
    std::string shm_name = "/smart_camera_frames";
    uint32_t shm_size_mb = 20;
    std::string socket_path = "/tmp/smart_camera_ai.sock";

    // Activity Detector
    std::string detect_classes = "person,cat,dog,bird";
    double confidence_threshold = 0.5;
    uint32_t session_timeout_sec = 60;
    std::string capture_dir = "/var/lib/smart-camera/captures/";
    uint32_t capture_max_files = 500;
    uint32_t capture_max_size_mb = 200;
    uint32_t disk_min_free_mb = 100;

    // S3 Uploader
    std::string s3_bucket = "smart-camera-captures";
    std::string s3_prefix = "captures";
    uint32_t upload_retry_count = 5;
    uint32_t upload_retry_interval_sec = 1;

    // DynamoDB
    std::string dynamodb_table = "smart-camera-events";
    uint32_t event_ttl_days = 90;
};

// ============================================================
// AppConfig — complete application configuration
// ============================================================

struct AppConfig {
    // IoT authentication
    IoTCertConfig iot;

    // Video parameters
    VideoPreset video_preset = PRESET_DEFAULT;
    CameraSourceType camera_source = CameraSourceType::VIDEOTESTSRC;
    std::string camera_device_path;

    // KVS
    KVSStreamConfig kvs;

    // WebRTC
    WebRTCConfig webrtc;

    // Network
    uint32_t bitrate_min_kbps = 256;
    uint32_t bitrate_max_kbps = 4096;
    uint32_t connection_check_interval_sec = 30;
    uint32_t connection_timeout_ms = 5000;

    // Logging
    std::string log_level = "INFO";
    std::string log_file_path = "/var/log/smart-camera/app.log";
    uint32_t log_max_file_size_mb = 10;
    uint32_t log_max_files = 5;
    bool log_to_stdout = true;

    // Watchdog
    uint32_t watchdog_interval_sec = 10;
    uint32_t watchdog_error_threshold = 10;
    uint32_t watchdog_stale_timeout_sec = 60;
    size_t memory_limit_mb = 512;
    uint32_t cpu_quota_percent = 80;

    // Frame buffer pool
    uint32_t frame_pool_size = 30;

    // AI
    uint32_t ai_model_timeout_sec = 5;

    // AI Summary
    AISummaryConfig ai_summary;

    // Profile
    std::string profile = "default";
};

// ============================================================
// IConfigManager — configuration management interface
// ============================================================

class IConfigManager {
public:
    virtual ~IConfigManager() = default;

    // Load configuration from a file
    virtual Result<AppConfig> load(const std::string& file_path) = 0;

    // Apply CLI argument overrides
    virtual Result<AppConfig> apply_cli_overrides(
        const AppConfig& base, int argc, char* argv[]) = 0;

    // Validate configuration
    virtual VoidResult validate(const AppConfig& config) = 0;

    // Pretty Printer: format config as key=value
    virtual std::string format(const AppConfig& config) const = 0;

    // Parse key=value string into config
    virtual Result<AppConfig> parse(const std::string& content) = 0;
};

// ============================================================
// ConfigManager — concrete implementation
// ============================================================

class ConfigManager : public IConfigManager {
public:
    ConfigManager() = default;

    Result<AppConfig> load(const std::string& file_path) override;
    Result<AppConfig> apply_cli_overrides(
        const AppConfig& base, int argc, char* argv[]) override;
    VoidResult validate(const AppConfig& config) override;
    std::string format(const AppConfig& config) const override;
    Result<AppConfig> parse(const std::string& content) override;

private:
    // Internal: parse raw text into section->key->value map
    using SectionMap = std::map<std::string, std::map<std::string, std::string>>;

    Result<SectionMap> parse_ini(const std::string& content) const;
    Result<AppConfig> build_config(const SectionMap& sections) const;
    Result<AppConfig> apply_preset(AppConfig config, const std::string& preset_name) const;
    void apply_section(AppConfig& config, const std::string& section,
                       const std::map<std::string, std::string>& kvs) const;
};

}  // namespace sc
