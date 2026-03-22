#include <gtest/gtest.h>
#include "config/config_manager.h"

#include <string>
#include <vector>

using namespace sc;

// ============================================================
// Helper: build argv from vector of strings
// ============================================================

namespace {

struct ArgvHelper {
    std::vector<std::string> args;
    std::vector<char*> ptrs;

    explicit ArgvHelper(std::initializer_list<std::string> list)
        : args(list) {
        for (auto& a : args) ptrs.push_back(a.data());
    }

    int argc() const { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

}  // namespace

// ============================================================
// parse() — key=value, [section], # comments
// ============================================================

TEST(ConfigManagerParse, BasicKeyValue) {
    ConfigManager cm;
    auto result = cm.parse("[video]\nwidth = 800\nheight = 600\nfps = 20\n");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().video_preset.width, 800u);
    EXPECT_EQ(result.value().video_preset.height, 600u);
    EXPECT_EQ(result.value().video_preset.fps, 20u);
}

TEST(ConfigManagerParse, SectionHeaders) {
    ConfigManager cm;
    std::string content =
        "[iot]\n"
        "cert_path = /tmp/cert.pem\n"
        "key_path = /tmp/key.pem\n"
        "\n"
        "[kvs]\n"
        "stream_name = my-stream\n"
        "region = us-east-1\n";
    auto result = cm.parse(content);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().iot.cert_path, "/tmp/cert.pem");
    EXPECT_EQ(result.value().iot.key_path, "/tmp/key.pem");
    EXPECT_EQ(result.value().kvs.stream_name, "my-stream");
    EXPECT_EQ(result.value().kvs.region, "us-east-1");
}

TEST(ConfigManagerParse, CommentsAndBlankLines) {
    ConfigManager cm;
    std::string content =
        "# This is a comment\n"
        "\n"
        "[video]\n"
        "# Another comment\n"
        "width = 640\n"
        "\n"
        "# fps = 99\n"
        "fps = 10\n";
    auto result = cm.parse(content);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().video_preset.width, 640u);
    EXPECT_EQ(result.value().video_preset.fps, 10u);
}

TEST(ConfigManagerParse, EmptyContent) {
    ConfigManager cm;
    auto result = cm.parse("");
    ASSERT_TRUE(result.is_ok());
    // Should produce default config
    EXPECT_EQ(result.value().video_preset.width, PRESET_DEFAULT.width);
}

// ============================================================
// parse() — error cases
// ============================================================

TEST(ConfigManagerParse, MissingEqualsSign) {
    ConfigManager cm;
    auto result = cm.parse("[video]\nwidth 800\n");
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigParseError);
    EXPECT_NE(result.error().message.find("Missing '='"), std::string::npos);
}

TEST(ConfigManagerParse, UnclosedSectionBracket) {
    ConfigManager cm;
    auto result = cm.parse("[video\nwidth = 800\n");
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigParseError);
    EXPECT_NE(result.error().message.find("Unclosed section"), std::string::npos);
}

TEST(ConfigManagerParse, EmptyKey) {
    ConfigManager cm;
    auto result = cm.parse("[video]\n = 800\n");
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigParseError);
    EXPECT_NE(result.error().message.find("Empty key"), std::string::npos);
}

// ============================================================
// apply_cli_overrides() — basic CLI args
// ============================================================

TEST(ConfigManagerCLI, ProfileOverride) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app", "--profile", "production"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().profile, "production");
}

TEST(ConfigManagerCLI, VideoParamsOverride) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app", "--width", "1920", "--height", "1080", "--fps", "30", "--bitrate", "4096"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().video_preset.width, 1920u);
    EXPECT_EQ(result.value().video_preset.height, 1080u);
    EXPECT_EQ(result.value().video_preset.fps, 30u);
    EXPECT_EQ(result.value().video_preset.bitrate_kbps, 4096u);
}

TEST(ConfigManagerCLI, IoTParamsOverride) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app",
        "--cert-path", "/new/cert.pem",
        "--key-path", "/new/key.pem",
        "--root-ca-path", "/new/ca.pem",
        "--stream-name", "cli-stream",
        "--channel-name", "cli-channel"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().iot.cert_path, "/new/cert.pem");
    EXPECT_EQ(result.value().iot.key_path, "/new/key.pem");
    EXPECT_EQ(result.value().iot.root_ca_path, "/new/ca.pem");
    EXPECT_EQ(result.value().kvs.stream_name, "cli-stream");
    EXPECT_EQ(result.value().webrtc.channel_name, "cli-channel");
}

// ============================================================
// CLI args override config file values
// ============================================================

TEST(ConfigManagerCLI, CLIOverridesConfigFile) {
    ConfigManager cm;
    std::string content =
        "[video]\n"
        "width = 640\n"
        "fps = 10\n"
        "[kvs]\n"
        "stream_name = file-stream\n";
    auto parse_result = cm.parse(content);
    ASSERT_TRUE(parse_result.is_ok());

    ArgvHelper args({"app", "--width", "1280", "--stream-name", "cli-stream"});
    auto result = cm.apply_cli_overrides(parse_result.value(), args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    // CLI overrides file values
    EXPECT_EQ(result.value().video_preset.width, 1280u);
    EXPECT_EQ(result.value().kvs.stream_name, "cli-stream");
    // File value preserved when not overridden by CLI
    EXPECT_EQ(result.value().video_preset.fps, 10u);
}

// ============================================================
// Preset loading
// ============================================================

TEST(ConfigManagerPreset, PresetDefault) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app", "--preset", "Default"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().video_preset.width, PRESET_DEFAULT.width);
    EXPECT_EQ(result.value().video_preset.height, PRESET_DEFAULT.height);
    EXPECT_EQ(result.value().video_preset.fps, PRESET_DEFAULT.fps);
    EXPECT_EQ(result.value().video_preset.bitrate_kbps, PRESET_DEFAULT.bitrate_kbps);
}

TEST(ConfigManagerPreset, PresetHD) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app", "--preset", "HD"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().video_preset.width, PRESET_HD.width);
    EXPECT_EQ(result.value().video_preset.height, PRESET_HD.height);
    EXPECT_EQ(result.value().video_preset.fps, PRESET_HD.fps);
    EXPECT_EQ(result.value().video_preset.bitrate_kbps, PRESET_HD.bitrate_kbps);
}

TEST(ConfigManagerPreset, PresetLowBandwidth) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app", "--preset", "Low_Bandwidth"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().video_preset.width, PRESET_LOW_BW.width);
    EXPECT_EQ(result.value().video_preset.height, PRESET_LOW_BW.height);
    EXPECT_EQ(result.value().video_preset.fps, PRESET_LOW_BW.fps);
    EXPECT_EQ(result.value().video_preset.bitrate_kbps, PRESET_LOW_BW.bitrate_kbps);
}

TEST(ConfigManagerPreset, PresetCaseInsensitive) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app", "--preset", "hd"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().video_preset.width, PRESET_HD.width);
}

TEST(ConfigManagerPreset, PresetNotFound) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app", "--preset", "UltraHD"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigPresetNotFound);
    // Error message should list available presets
    EXPECT_NE(result.error().message.find("Default"), std::string::npos);
    EXPECT_NE(result.error().message.find("HD"), std::string::npos);
    EXPECT_NE(result.error().message.find("Low_Bandwidth"), std::string::npos);
}

// ============================================================
// Individual params override preset values
// ============================================================

TEST(ConfigManagerPreset, IndividualParamsOverridePreset) {
    ConfigManager cm;
    AppConfig base;
    // Load HD preset then override fps
    ArgvHelper args({"app", "--preset", "HD", "--fps", "15"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    // Width/height from HD preset
    EXPECT_EQ(result.value().video_preset.width, PRESET_HD.width);
    EXPECT_EQ(result.value().video_preset.height, PRESET_HD.height);
    // FPS overridden by CLI
    EXPECT_EQ(result.value().video_preset.fps, 15u);
}

TEST(ConfigManagerPreset, PresetInConfigFileWithOverride) {
    ConfigManager cm;
    std::string content =
        "[video]\n"
        "preset = HD\n"
        "fps = 20\n";
    auto result = cm.parse(content);
    ASSERT_TRUE(result.is_ok());
    // HD preset values for width/height
    EXPECT_EQ(result.value().video_preset.width, PRESET_HD.width);
    EXPECT_EQ(result.value().video_preset.height, PRESET_HD.height);
    // fps overridden by individual param in config file
    EXPECT_EQ(result.value().video_preset.fps, 20u);
}

// ============================================================
// validate() — production profile
// ============================================================

TEST(ConfigManagerValidate, ProductionMissingCertPath) {
    ConfigManager cm;
    AppConfig config;
    config.profile = "production";
    // Leave cert_path empty
    config.iot.key_path = "/key.pem";
    config.iot.root_ca_path = "/ca.pem";
    config.kvs.stream_name = "stream";
    config.webrtc.channel_name = "channel";

    auto result = cm.validate(config);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigValidationError);
    EXPECT_NE(result.error().message.find("cert_path"), std::string::npos);
}

TEST(ConfigManagerValidate, ProductionMissingKeyPath) {
    ConfigManager cm;
    AppConfig config;
    config.profile = "production";
    config.iot.cert_path = "/cert.pem";
    // Leave key_path empty
    config.iot.root_ca_path = "/ca.pem";
    config.kvs.stream_name = "stream";
    config.webrtc.channel_name = "channel";

    auto result = cm.validate(config);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigValidationError);
    EXPECT_NE(result.error().message.find("key_path"), std::string::npos);
}

TEST(ConfigManagerValidate, ProductionMissingRootCA) {
    ConfigManager cm;
    AppConfig config;
    config.profile = "production";
    config.iot.cert_path = "/cert.pem";
    config.iot.key_path = "/key.pem";
    // Leave root_ca_path empty
    config.kvs.stream_name = "stream";
    config.webrtc.channel_name = "channel";

    auto result = cm.validate(config);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigValidationError);
    EXPECT_NE(result.error().message.find("root_ca_path"), std::string::npos);
}

TEST(ConfigManagerValidate, ProductionMissingStreamName) {
    ConfigManager cm;
    AppConfig config;
    config.profile = "production";
    config.iot.cert_path = "/cert.pem";
    config.iot.key_path = "/key.pem";
    config.iot.root_ca_path = "/ca.pem";
    // Leave stream_name empty
    config.webrtc.channel_name = "channel";

    auto result = cm.validate(config);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigValidationError);
    EXPECT_NE(result.error().message.find("stream_name"), std::string::npos);
}

TEST(ConfigManagerValidate, ProductionMissingChannelName) {
    ConfigManager cm;
    AppConfig config;
    config.profile = "production";
    config.iot.cert_path = "/cert.pem";
    config.iot.key_path = "/key.pem";
    config.iot.root_ca_path = "/ca.pem";
    config.kvs.stream_name = "stream";
    // Leave channel_name empty

    auto result = cm.validate(config);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigValidationError);
    EXPECT_NE(result.error().message.find("channel_name"), std::string::npos);
}

TEST(ConfigManagerValidate, ProductionAllFieldsPresent) {
    ConfigManager cm;
    AppConfig config;
    config.profile = "production";
    config.iot.cert_path = "/cert.pem";
    config.iot.key_path = "/key.pem";
    config.iot.root_ca_path = "/ca.pem";
    config.kvs.stream_name = "stream";
    config.webrtc.channel_name = "channel";

    auto result = cm.validate(config);
    EXPECT_TRUE(result.is_ok());
}

// ============================================================
// validate() — development profile
// ============================================================

TEST(ConfigManagerValidate, DevelopmentAllowsMissingCertPath) {
    ConfigManager cm;
    AppConfig config;
    config.profile = "development";
    // All IoT fields empty — development allows this

    auto result = cm.validate(config);
    EXPECT_TRUE(result.is_ok());
}

TEST(ConfigManagerValidate, DevelopmentAllowsMissingAllFields) {
    ConfigManager cm;
    AppConfig config;
    config.profile = "development";
    // Everything empty

    auto result = cm.validate(config);
    EXPECT_TRUE(result.is_ok());
}

// ============================================================
// validate() — parameter sanity checks (resolution/fps limits)
// ============================================================

TEST(ConfigManagerValidate, WidthExceedsMax) {
    ConfigManager cm;
    AppConfig config;
    config.video_preset.width = 2560;

    auto result = cm.validate(config);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigValidationError);
    EXPECT_NE(result.error().message.find("Width"), std::string::npos);
    EXPECT_NE(result.error().message.find("1920"), std::string::npos);
}

TEST(ConfigManagerValidate, HeightExceedsMax) {
    ConfigManager cm;
    AppConfig config;
    config.video_preset.height = 1440;

    auto result = cm.validate(config);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigValidationError);
    EXPECT_NE(result.error().message.find("Height"), std::string::npos);
    EXPECT_NE(result.error().message.find("1080"), std::string::npos);
}

TEST(ConfigManagerValidate, FPSExceedsMax) {
    ConfigManager cm;
    AppConfig config;
    config.video_preset.fps = 60;

    auto result = cm.validate(config);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigValidationError);
    EXPECT_NE(result.error().message.find("FPS"), std::string::npos);
    EXPECT_NE(result.error().message.find("30"), std::string::npos);
}

TEST(ConfigManagerValidate, MaxBoundaryValuesPass) {
    ConfigManager cm;
    AppConfig config;
    config.video_preset.width = 1920;
    config.video_preset.height = 1080;
    config.video_preset.fps = 30;

    auto result = cm.validate(config);
    EXPECT_TRUE(result.is_ok());
}

// ============================================================
// Round-trip consistency: parse → format → parse
// ============================================================

TEST(ConfigManagerRoundTrip, ParseFormatParseEquivalent) {
    ConfigManager cm;

    // Build a config with non-default values
    AppConfig original;
    original.profile = "production";
    original.iot.cert_path = "/etc/certs/device.pem";
    original.iot.key_path = "/etc/certs/key.pem";
    original.iot.root_ca_path = "/etc/certs/ca.pem";
    original.iot.credential_endpoint = "endpoint.iot.amazonaws.com";
    original.iot.role_alias = "MyRole";
    original.iot.thing_name = "thing-001";
    original.video_preset = PRESET_HD;
    original.camera_source = CameraSourceType::V4L2_USB;
    original.camera_device_path = "/dev/video0";
    original.kvs.stream_name = "test-stream";
    original.kvs.region = "us-west-2";
    original.kvs.retention_hours = 48;
    original.kvs.auto_create_stream = false;
    original.webrtc.channel_name = "test-channel";
    original.webrtc.region = "us-west-2";
    original.webrtc.max_viewers = 5;
    original.bitrate_min_kbps = 128;
    original.bitrate_max_kbps = 8192;
    original.log_level = "DEBUG";
    original.log_to_stdout = false;
    original.frame_pool_size = 50;
    original.ai_model_timeout_sec = 10;

    // format → string
    std::string formatted = cm.format(original);

    // parse the formatted string
    auto result = cm.parse(formatted);
    ASSERT_TRUE(result.is_ok()) << "Parse failed: " << result.error().message;
    const auto& roundtrip = result.value();

    // Verify key fields are equivalent
    EXPECT_EQ(roundtrip.iot.cert_path, original.iot.cert_path);
    EXPECT_EQ(roundtrip.iot.key_path, original.iot.key_path);
    EXPECT_EQ(roundtrip.iot.root_ca_path, original.iot.root_ca_path);
    EXPECT_EQ(roundtrip.iot.credential_endpoint, original.iot.credential_endpoint);
    EXPECT_EQ(roundtrip.iot.role_alias, original.iot.role_alias);
    EXPECT_EQ(roundtrip.iot.thing_name, original.iot.thing_name);

    EXPECT_EQ(roundtrip.video_preset.width, original.video_preset.width);
    EXPECT_EQ(roundtrip.video_preset.height, original.video_preset.height);
    EXPECT_EQ(roundtrip.video_preset.fps, original.video_preset.fps);
    EXPECT_EQ(roundtrip.video_preset.bitrate_kbps, original.video_preset.bitrate_kbps);
    EXPECT_EQ(roundtrip.camera_device_path, original.camera_device_path);

    EXPECT_EQ(roundtrip.kvs.stream_name, original.kvs.stream_name);
    EXPECT_EQ(roundtrip.kvs.region, original.kvs.region);
    EXPECT_EQ(roundtrip.kvs.retention_hours, original.kvs.retention_hours);
    EXPECT_EQ(roundtrip.kvs.auto_create_stream, original.kvs.auto_create_stream);

    EXPECT_EQ(roundtrip.webrtc.channel_name, original.webrtc.channel_name);
    EXPECT_EQ(roundtrip.webrtc.region, original.webrtc.region);
    EXPECT_EQ(roundtrip.webrtc.max_viewers, original.webrtc.max_viewers);

    EXPECT_EQ(roundtrip.bitrate_min_kbps, original.bitrate_min_kbps);
    EXPECT_EQ(roundtrip.bitrate_max_kbps, original.bitrate_max_kbps);

    EXPECT_EQ(roundtrip.log_level, original.log_level);
    EXPECT_EQ(roundtrip.log_to_stdout, original.log_to_stdout);

    EXPECT_EQ(roundtrip.frame_pool_size, original.frame_pool_size);
    EXPECT_EQ(roundtrip.ai_model_timeout_sec, original.ai_model_timeout_sec);
}

TEST(ConfigManagerRoundTrip, DefaultConfigRoundTrip) {
    ConfigManager cm;
    AppConfig original;  // all defaults

    std::string formatted = cm.format(original);
    auto result = cm.parse(formatted);
    ASSERT_TRUE(result.is_ok());
    const auto& roundtrip = result.value();

    EXPECT_EQ(roundtrip.video_preset.width, original.video_preset.width);
    EXPECT_EQ(roundtrip.video_preset.height, original.video_preset.height);
    EXPECT_EQ(roundtrip.video_preset.fps, original.video_preset.fps);
    EXPECT_EQ(roundtrip.video_preset.bitrate_kbps, original.video_preset.bitrate_kbps);
    EXPECT_EQ(roundtrip.log_level, original.log_level);
    EXPECT_EQ(roundtrip.log_to_stdout, original.log_to_stdout);
    EXPECT_EQ(roundtrip.watchdog_interval_sec, original.watchdog_interval_sec);
    EXPECT_EQ(roundtrip.memory_limit_mb, original.memory_limit_mb);
}

// ============================================================
// Camera source parsing
// ============================================================

TEST(ConfigManagerParse, CameraSourceTypes) {
    ConfigManager cm;

    auto r1 = cm.parse("[video]\ncamera_source = libcamera_csi\n");
    ASSERT_TRUE(r1.is_ok());
    EXPECT_EQ(r1.value().camera_source, CameraSourceType::LIBCAMERA_CSI);

    auto r2 = cm.parse("[video]\ncamera_source = v4l2_usb\n");
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value().camera_source, CameraSourceType::V4L2_USB);

    auto r3 = cm.parse("[video]\ncamera_source = videotestsrc\n");
    ASSERT_TRUE(r3.is_ok());
    EXPECT_EQ(r3.value().camera_source, CameraSourceType::VIDEOTESTSRC);
}

// ============================================================
// Config file not found
// ============================================================

TEST(ConfigManagerLoad, FileNotFound) {
    ConfigManager cm;
    auto result = cm.load("/nonexistent/path/config.ini");
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::ConfigFileNotFound);
}


// ============================================================
// AppConfig — default values
// ============================================================

TEST(AppConfigDefaults, AllDefaultsCorrect) {
    AppConfig config;
    EXPECT_EQ(config.video_preset.width, PRESET_DEFAULT.width);
    EXPECT_EQ(config.video_preset.height, PRESET_DEFAULT.height);
    EXPECT_EQ(config.video_preset.fps, PRESET_DEFAULT.fps);
    EXPECT_EQ(config.video_preset.bitrate_kbps, PRESET_DEFAULT.bitrate_kbps);
    EXPECT_EQ(config.camera_source, CameraSourceType::VIDEOTESTSRC);
    EXPECT_TRUE(config.camera_device_path.empty());
    EXPECT_EQ(config.bitrate_min_kbps, 256u);
    EXPECT_EQ(config.bitrate_max_kbps, 4096u);
    EXPECT_EQ(config.connection_check_interval_sec, 30u);
    EXPECT_EQ(config.connection_timeout_ms, 5000u);
    EXPECT_EQ(config.log_level, "INFO");
    EXPECT_EQ(config.log_max_file_size_mb, 10u);
    EXPECT_EQ(config.log_max_files, 5u);
    EXPECT_TRUE(config.log_to_stdout);
    EXPECT_EQ(config.watchdog_interval_sec, 10u);
    EXPECT_EQ(config.watchdog_error_threshold, 10u);
    EXPECT_EQ(config.watchdog_stale_timeout_sec, 60u);
    EXPECT_EQ(config.memory_limit_mb, 512u);
    EXPECT_EQ(config.cpu_quota_percent, 80u);
    EXPECT_EQ(config.frame_pool_size, 30u);
    EXPECT_EQ(config.ai_model_timeout_sec, 5u);
    EXPECT_EQ(config.profile, "default");
    EXPECT_EQ(config.kvs.retention_hours, 168u);
    EXPECT_TRUE(config.kvs.auto_create_stream);
    EXPECT_EQ(config.webrtc.max_viewers, 10u);
}

// ============================================================
// parse() — all section types
// ============================================================

TEST(ConfigManagerParse, NetworkSection) {
    ConfigManager cm;
    std::string content =
        "[network]\n"
        "bitrate_min_kbps = 128\n"
        "bitrate_max_kbps = 8192\n"
        "connection_check_interval_sec = 60\n"
        "connection_timeout_ms = 10000\n";
    auto result = cm.parse(content);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().bitrate_min_kbps, 128u);
    EXPECT_EQ(result.value().bitrate_max_kbps, 8192u);
    EXPECT_EQ(result.value().connection_check_interval_sec, 60u);
    EXPECT_EQ(result.value().connection_timeout_ms, 10000u);
}

TEST(ConfigManagerParse, LoggingSection) {
    ConfigManager cm;
    std::string content =
        "[logging]\n"
        "level = DEBUG\n"
        "file_path = /tmp/test.log\n"
        "max_file_size_mb = 20\n"
        "max_files = 3\n"
        "log_to_stdout = false\n";
    auto result = cm.parse(content);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().log_level, "DEBUG");
    EXPECT_EQ(result.value().log_file_path, "/tmp/test.log");
    EXPECT_EQ(result.value().log_max_file_size_mb, 20u);
    EXPECT_EQ(result.value().log_max_files, 3u);
    EXPECT_FALSE(result.value().log_to_stdout);
}

TEST(ConfigManagerParse, WatchdogSection) {
    ConfigManager cm;
    std::string content =
        "[watchdog]\n"
        "interval_sec = 20\n"
        "error_threshold = 5\n"
        "stale_timeout_sec = 120\n"
        "memory_limit_mb = 256\n"
        "cpu_quota_percent = 50\n";
    auto result = cm.parse(content);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().watchdog_interval_sec, 20u);
    EXPECT_EQ(result.value().watchdog_error_threshold, 5u);
    EXPECT_EQ(result.value().watchdog_stale_timeout_sec, 120u);
    EXPECT_EQ(result.value().memory_limit_mb, 256u);
    EXPECT_EQ(result.value().cpu_quota_percent, 50u);
}

TEST(ConfigManagerParse, FramePoolSection) {
    ConfigManager cm;
    auto result = cm.parse("[frame_pool]\npool_size = 60\n");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().frame_pool_size, 60u);
}

TEST(ConfigManagerParse, AISection) {
    ConfigManager cm;
    auto result = cm.parse("[ai]\nmodel_timeout_sec = 10\n");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().ai_model_timeout_sec, 10u);
}

TEST(ConfigManagerParse, WebRTCSection) {
    ConfigManager cm;
    std::string content =
        "[webrtc]\n"
        "channel_name = my-channel\n"
        "region = eu-west-1\n"
        "max_viewers = 5\n";
    auto result = cm.parse(content);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().webrtc.channel_name, "my-channel");
    EXPECT_EQ(result.value().webrtc.region, "eu-west-1");
    EXPECT_EQ(result.value().webrtc.max_viewers, 5u);
}

TEST(ConfigManagerParse, KVSSectionAllFields) {
    ConfigManager cm;
    std::string content =
        "[kvs]\n"
        "stream_name = test-stream\n"
        "region = ap-northeast-1\n"
        "retention_hours = 48\n"
        "auto_create_stream = false\n";
    auto result = cm.parse(content);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().kvs.stream_name, "test-stream");
    EXPECT_EQ(result.value().kvs.region, "ap-northeast-1");
    EXPECT_EQ(result.value().kvs.retention_hours, 48u);
    EXPECT_FALSE(result.value().kvs.auto_create_stream);
}

TEST(ConfigManagerParse, ProfileInRootSection) {
    ConfigManager cm;
    auto result = cm.parse("profile = production\n[video]\nwidth = 640\n");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().profile, "production");
}

// ============================================================
// Camera source alias parsing
// ============================================================

TEST(ConfigManagerParse, CameraSourceShortAliases) {
    ConfigManager cm;

    auto r1 = cm.parse("[video]\ncamera_source = libcamera\n");
    ASSERT_TRUE(r1.is_ok());
    EXPECT_EQ(r1.value().camera_source, CameraSourceType::LIBCAMERA_CSI);

    auto r2 = cm.parse("[video]\ncamera_source = v4l2\n");
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value().camera_source, CameraSourceType::V4L2_USB);
}

TEST(ConfigManagerParse, CameraSourceCaseInsensitive) {
    ConfigManager cm;
    auto r = cm.parse("[video]\ncamera_source = LIBCAMERA_CSI\n");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().camera_source, CameraSourceType::LIBCAMERA_CSI);
}

TEST(ConfigManagerParse, CameraSourceUnknownDefaultsToVideoTestSrc) {
    ConfigManager cm;
    auto r = cm.parse("[video]\ncamera_source = unknown_source\n");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().camera_source, CameraSourceType::VIDEOTESTSRC);
}

// ============================================================
// parse() — invalid numeric values fallback to defaults
// ============================================================

TEST(ConfigManagerParse, InvalidNumericFallsBackToDefault) {
    ConfigManager cm;
    auto result = cm.parse("[video]\nwidth = abc\nfps = not_a_number\n");
    ASSERT_TRUE(result.is_ok());
    // Invalid values should fall back to defaults (from PRESET_DEFAULT)
    EXPECT_EQ(result.value().video_preset.width, PRESET_DEFAULT.width);
    EXPECT_EQ(result.value().video_preset.fps, PRESET_DEFAULT.fps);
}

// ============================================================
// parse() — boolean parsing edge cases
// ============================================================

TEST(ConfigManagerParse, BooleanParsingVariants) {
    ConfigManager cm;

    // "true", "1", "yes" → true
    auto r1 = cm.parse("[logging]\nlog_to_stdout = true\n");
    ASSERT_TRUE(r1.is_ok());
    EXPECT_TRUE(r1.value().log_to_stdout);

    auto r2 = cm.parse("[logging]\nlog_to_stdout = 1\n");
    ASSERT_TRUE(r2.is_ok());
    EXPECT_TRUE(r2.value().log_to_stdout);

    auto r3 = cm.parse("[logging]\nlog_to_stdout = yes\n");
    ASSERT_TRUE(r3.is_ok());
    EXPECT_TRUE(r3.value().log_to_stdout);

    // "false", "0", "no" → false
    auto r4 = cm.parse("[logging]\nlog_to_stdout = false\n");
    ASSERT_TRUE(r4.is_ok());
    EXPECT_FALSE(r4.value().log_to_stdout);

    auto r5 = cm.parse("[logging]\nlog_to_stdout = 0\n");
    ASSERT_TRUE(r5.is_ok());
    EXPECT_FALSE(r5.value().log_to_stdout);

    auto r6 = cm.parse("[logging]\nlog_to_stdout = no\n");
    ASSERT_TRUE(r6.is_ok());
    EXPECT_FALSE(r6.value().log_to_stdout);
}

// ============================================================
// apply_cli_overrides() — additional CLI args
// ============================================================

TEST(ConfigManagerCLI, CredentialEndpointAndRoleAlias) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app",
        "--credential-endpoint", "endpoint.iot.amazonaws.com",
        "--role-alias", "MyRole",
        "--thing-name", "thing-001"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().iot.credential_endpoint, "endpoint.iot.amazonaws.com");
    EXPECT_EQ(result.value().iot.role_alias, "MyRole");
    EXPECT_EQ(result.value().iot.thing_name, "thing-001");
}

TEST(ConfigManagerCLI, RegionOverrides) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app",
        "--kvs-region", "us-west-2",
        "--webrtc-region", "eu-west-1"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().kvs.region, "us-west-2");
    EXPECT_EQ(result.value().webrtc.region, "eu-west-1");
}

TEST(ConfigManagerCLI, LoggingOverrides) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app",
        "--log-level", "DEBUG",
        "--log-file", "/tmp/test.log",
        "--log-to-stdout", "false"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().log_level, "DEBUG");
    EXPECT_EQ(result.value().log_file_path, "/tmp/test.log");
    EXPECT_FALSE(result.value().log_to_stdout);
}

TEST(ConfigManagerCLI, CameraSourceAndDevice) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app",
        "--camera-source", "v4l2",
        "--camera-device", "/dev/video0"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().camera_source, CameraSourceType::V4L2_USB);
    EXPECT_EQ(result.value().camera_device_path, "/dev/video0");
}

TEST(ConfigManagerCLI, UnknownArgsIgnored) {
    ConfigManager cm;
    AppConfig base;
    ArgvHelper args({"app", "--unknown-flag", "value", "--width", "800"});
    auto result = cm.apply_cli_overrides(base, args.argc(), args.argv());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().video_preset.width, 800u);
}

// ============================================================
// format() — output structure
// ============================================================

TEST(ConfigManagerFormat, OutputContainsAllSections) {
    ConfigManager cm;
    AppConfig config;
    config.profile = "test-profile";
    config.iot.cert_path = "/cert.pem";
    config.kvs.stream_name = "my-stream";
    config.webrtc.channel_name = "my-channel";

    std::string output = cm.format(config);

    EXPECT_NE(output.find("[iot]"), std::string::npos);
    EXPECT_NE(output.find("[video]"), std::string::npos);
    EXPECT_NE(output.find("[kvs]"), std::string::npos);
    EXPECT_NE(output.find("[webrtc]"), std::string::npos);
    EXPECT_NE(output.find("[network]"), std::string::npos);
    EXPECT_NE(output.find("[logging]"), std::string::npos);
    EXPECT_NE(output.find("[watchdog]"), std::string::npos);
    EXPECT_NE(output.find("[frame_pool]"), std::string::npos);
    EXPECT_NE(output.find("[ai]"), std::string::npos);
    EXPECT_NE(output.find("test-profile"), std::string::npos);
    EXPECT_NE(output.find("/cert.pem"), std::string::npos);
    EXPECT_NE(output.find("my-stream"), std::string::npos);
    EXPECT_NE(output.find("my-channel"), std::string::npos);
}

// ============================================================
// Struct default construction
// ============================================================

TEST(IoTCertConfigDefaults, AllFieldsEmpty) {
    IoTCertConfig iot;
    EXPECT_TRUE(iot.cert_path.empty());
    EXPECT_TRUE(iot.key_path.empty());
    EXPECT_TRUE(iot.root_ca_path.empty());
    EXPECT_TRUE(iot.credential_endpoint.empty());
    EXPECT_TRUE(iot.role_alias.empty());
    EXPECT_TRUE(iot.thing_name.empty());
}

TEST(KVSStreamConfigDefaults, CorrectDefaults) {
    KVSStreamConfig kvs;
    EXPECT_TRUE(kvs.stream_name.empty());
    EXPECT_TRUE(kvs.region.empty());
    EXPECT_EQ(kvs.retention_hours, 168u);
    EXPECT_TRUE(kvs.auto_create_stream);
}

TEST(WebRTCConfigDefaults, CorrectDefaults) {
    WebRTCConfig webrtc;
    EXPECT_TRUE(webrtc.channel_name.empty());
    EXPECT_TRUE(webrtc.region.empty());
    EXPECT_EQ(webrtc.max_viewers, 10u);
    EXPECT_TRUE(webrtc.stun_urls.empty());
    EXPECT_TRUE(webrtc.turn_urls.empty());
}

TEST(CameraSourceTypeEnum, ValuesCorrect) {
    EXPECT_EQ(static_cast<uint8_t>(CameraSourceType::LIBCAMERA_CSI), 0);
    EXPECT_EQ(static_cast<uint8_t>(CameraSourceType::V4L2_USB), 1);
    EXPECT_EQ(static_cast<uint8_t>(CameraSourceType::VIDEOTESTSRC), 2);
}

// ============================================================
// parse() — full multi-section config
// ============================================================

TEST(ConfigManagerParse, FullMultiSectionConfig) {
    ConfigManager cm;
    std::string content =
        "profile = production\n"
        "\n"
        "[iot]\n"
        "cert_path = /certs/device.pem\n"
        "key_path = /certs/key.pem\n"
        "root_ca_path = /certs/ca.pem\n"
        "credential_endpoint = ep.iot.amazonaws.com\n"
        "role_alias = CameraRole\n"
        "thing_name = cam-001\n"
        "\n"
        "[video]\n"
        "width = 1280\n"
        "height = 720\n"
        "fps = 15\n"
        "bitrate_kbps = 2048\n"
        "camera_source = videotestsrc\n"
        "\n"
        "[kvs]\n"
        "stream_name = prod-stream\n"
        "region = us-east-1\n"
        "\n"
        "[webrtc]\n"
        "channel_name = prod-channel\n"
        "region = us-east-1\n"
        "\n"
        "[network]\n"
        "bitrate_min_kbps = 512\n"
        "\n"
        "[logging]\n"
        "level = WARNING\n"
        "\n"
        "[watchdog]\n"
        "memory_limit_mb = 1024\n"
        "\n"
        "[frame_pool]\n"
        "pool_size = 50\n"
        "\n"
        "[ai]\n"
        "model_timeout_sec = 3\n";

    auto result = cm.parse(content);
    ASSERT_TRUE(result.is_ok());
    const auto& c = result.value();

    EXPECT_EQ(c.profile, "production");
    EXPECT_EQ(c.iot.cert_path, "/certs/device.pem");
    EXPECT_EQ(c.iot.thing_name, "cam-001");
    EXPECT_EQ(c.video_preset.width, 1280u);
    EXPECT_EQ(c.kvs.stream_name, "prod-stream");
    EXPECT_EQ(c.webrtc.channel_name, "prod-channel");
    EXPECT_EQ(c.bitrate_min_kbps, 512u);
    EXPECT_EQ(c.log_level, "WARNING");
    EXPECT_EQ(c.memory_limit_mb, 1024u);
    EXPECT_EQ(c.frame_pool_size, 50u);
    EXPECT_EQ(c.ai_model_timeout_sec, 3u);
}
