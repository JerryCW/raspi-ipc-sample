#include "config/config_manager.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace sc {

// ============================================================
// Helpers
// ============================================================

namespace {

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

CameraSourceType parse_camera_source(const std::string& s) {
    auto lower = to_lower(s);
    if (lower == "libcamera_csi" || lower == "libcamera") return CameraSourceType::LIBCAMERA_CSI;
    if (lower == "v4l2_usb" || lower == "v4l2") return CameraSourceType::V4L2_USB;
    return CameraSourceType::VIDEOTESTSRC;
}

std::string camera_source_to_string(CameraSourceType t) {
    switch (t) {
        case CameraSourceType::LIBCAMERA_CSI: return "libcamera_csi";
        case CameraSourceType::V4L2_USB:      return "v4l2_usb";
        case CameraSourceType::VIDEOTESTSRC:  return "videotestsrc";
    }
    return "videotestsrc";
}

bool parse_bool(const std::string& s) {
    auto lower = to_lower(s);
    return lower == "true" || lower == "1" || lower == "yes";
}

std::string bool_to_string(bool v) {
    return v ? "true" : "false";
}

// Find a built-in preset by name (case-insensitive)
const VideoPreset* find_preset(const std::string& name) {
    auto lower = to_lower(name);
    for (const auto& p : builtin_presets()) {
        if (to_lower(p.name) == lower) {
            // Return pointer to the inline constant
            if (lower == "default") return &PRESET_DEFAULT;
            if (lower == "hd") return &PRESET_HD;
            if (lower == "low_bandwidth") return &PRESET_LOW_BW;
        }
    }
    return nullptr;
}

std::string available_presets_string() {
    std::string result;
    for (const auto& p : builtin_presets()) {
        if (!result.empty()) result += ", ";
        result += p.name;
    }
    return result;
}

}  // namespace

// ============================================================
// parse_ini — parse raw text into section map
// ============================================================

Result<ConfigManager::SectionMap> ConfigManager::parse_ini(const std::string& content) const {
    SectionMap sections;
    std::string current_section;
    std::istringstream stream(content);
    std::string line;
    int line_num = 0;

    while (std::getline(stream, line)) {
        line_num++;
        auto trimmed = trim(line);

        // Skip empty lines and comments
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // Section header
        if (trimmed[0] == '[') {
            auto close = trimmed.find(']');
            if (close == std::string::npos) {
                return Result<SectionMap>::Err(Error{
                    ErrorCode::ConfigParseError,
                    "Unclosed section header at line " + std::to_string(line_num),
                    "line " + std::to_string(line_num)});
            }
            current_section = trim(trimmed.substr(1, close - 1));
            continue;
        }

        // key=value
        auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            return Result<SectionMap>::Err(Error{
                ErrorCode::ConfigParseError,
                "Missing '=' in key=value pair at line " + std::to_string(line_num),
                "line " + std::to_string(line_num)});
        }

        auto key = trim(trimmed.substr(0, eq));
        auto value = trim(trimmed.substr(eq + 1));

        if (key.empty()) {
            return Result<SectionMap>::Err(Error{
                ErrorCode::ConfigParseError,
                "Empty key at line " + std::to_string(line_num),
                "line " + std::to_string(line_num)});
        }

        sections[current_section][key] = value;
    }

    return Result<SectionMap>::Ok(std::move(sections));
}

// ============================================================
// apply_section — apply a section's key-value pairs to config
// ============================================================

void ConfigManager::apply_section(AppConfig& config, const std::string& section,
                                   const std::map<std::string, std::string>& kvs) const {
    auto get = [&](const std::string& key) -> std::string {
        auto it = kvs.find(key);
        return (it != kvs.end()) ? it->second : "";
    };

    auto get_uint = [&](const std::string& key, uint32_t def) -> uint32_t {
        auto v = get(key);
        if (v.empty()) return def;
        try { return static_cast<uint32_t>(std::stoul(v)); }
        catch (...) { return def; }
    };

    auto get_size = [&](const std::string& key, size_t def) -> size_t {
        auto v = get(key);
        if (v.empty()) return def;
        try { return static_cast<size_t>(std::stoull(v)); }
        catch (...) { return def; }
    };

    if (section == "iot") {
        if (!get("cert_path").empty()) config.iot.cert_path = get("cert_path");
        if (!get("key_path").empty()) config.iot.key_path = get("key_path");
        if (!get("root_ca_path").empty()) config.iot.root_ca_path = get("root_ca_path");
        if (!get("credential_endpoint").empty()) config.iot.credential_endpoint = get("credential_endpoint");
        if (!get("role_alias").empty()) config.iot.role_alias = get("role_alias");
        if (!get("thing_name").empty()) config.iot.thing_name = get("thing_name");
    } else if (section == "video") {
        if (!get("width").empty()) config.video_preset.width = get_uint("width", config.video_preset.width);
        if (!get("height").empty()) config.video_preset.height = get_uint("height", config.video_preset.height);
        if (!get("fps").empty()) config.video_preset.fps = get_uint("fps", config.video_preset.fps);
        if (!get("bitrate_kbps").empty()) config.video_preset.bitrate_kbps = get_uint("bitrate_kbps", config.video_preset.bitrate_kbps);
        if (!get("camera_source").empty()) config.camera_source = parse_camera_source(get("camera_source"));
        if (!get("camera_device_path").empty()) config.camera_device_path = get("camera_device_path");
        // preset is handled separately in build_config
    } else if (section == "kvs") {
        if (!get("stream_name").empty()) config.kvs.stream_name = get("stream_name");
        if (!get("region").empty()) config.kvs.region = get("region");
        config.kvs.retention_hours = get_uint("retention_hours", config.kvs.retention_hours);
        if (!get("auto_create_stream").empty()) config.kvs.auto_create_stream = parse_bool(get("auto_create_stream"));
    } else if (section == "webrtc") {
        if (!get("channel_name").empty()) config.webrtc.channel_name = get("channel_name");
        if (!get("region").empty()) config.webrtc.region = get("region");
        config.webrtc.max_viewers = get_uint("max_viewers", config.webrtc.max_viewers);
    } else if (section == "network") {
        config.bitrate_min_kbps = get_uint("bitrate_min_kbps", config.bitrate_min_kbps);
        config.bitrate_max_kbps = get_uint("bitrate_max_kbps", config.bitrate_max_kbps);
        config.connection_check_interval_sec = get_uint("connection_check_interval_sec", config.connection_check_interval_sec);
        config.connection_timeout_ms = get_uint("connection_timeout_ms", config.connection_timeout_ms);
    } else if (section == "logging") {
        if (!get("level").empty()) config.log_level = get("level");
        if (!get("file_path").empty()) config.log_file_path = get("file_path");
        config.log_max_file_size_mb = get_uint("max_file_size_mb", config.log_max_file_size_mb);
        config.log_max_files = get_uint("max_files", config.log_max_files);
        if (!get("log_to_stdout").empty()) config.log_to_stdout = parse_bool(get("log_to_stdout"));
    } else if (section == "watchdog") {
        config.watchdog_interval_sec = get_uint("interval_sec", config.watchdog_interval_sec);
        config.watchdog_error_threshold = get_uint("error_threshold", config.watchdog_error_threshold);
        config.watchdog_stale_timeout_sec = get_uint("stale_timeout_sec", config.watchdog_stale_timeout_sec);
        config.memory_limit_mb = get_size("memory_limit_mb", config.memory_limit_mb);
        config.cpu_quota_percent = get_uint("cpu_quota_percent", config.cpu_quota_percent);
    } else if (section == "frame_pool") {
        config.frame_pool_size = get_uint("pool_size", config.frame_pool_size);
    } else if (section == "ai") {
        config.ai_model_timeout_sec = get_uint("model_timeout_sec", config.ai_model_timeout_sec);
    }
}

// ============================================================
// build_config — convert section map to AppConfig
// ============================================================

Result<AppConfig> ConfigManager::build_config(const SectionMap& sections) const {
    AppConfig config;

    // Check for preset in [video] section first
    auto video_it = sections.find("video");
    if (video_it != sections.end()) {
        auto preset_it = video_it->second.find("preset");
        if (preset_it != video_it->second.end() && !preset_it->second.empty()) {
            auto result = apply_preset(config, preset_it->second);
            if (result.is_err()) return result;
            config = std::move(result).value();
        }
    }

    // Check for profile in root section
    auto root_it = sections.find("");
    if (root_it != sections.end()) {
        auto profile_it = root_it->second.find("profile");
        if (profile_it != root_it->second.end()) {
            config.profile = profile_it->second;
        }
    }

    // Apply all sections (individual params override preset values)
    for (const auto& [section, kvs] : sections) {
        if (section.empty()) continue;  // skip root-level keys (already handled profile)
        apply_section(config, section, kvs);
    }

    return Result<AppConfig>::Ok(std::move(config));
}

// ============================================================
// apply_preset — load a video preset by name
// ============================================================

Result<AppConfig> ConfigManager::apply_preset(AppConfig config, const std::string& preset_name) const {
    const auto* preset = find_preset(preset_name);
    if (!preset) {
        return Result<AppConfig>::Err(Error{
            ErrorCode::ConfigPresetNotFound,
            "Preset '" + preset_name + "' not found. Available presets: " + available_presets_string(),
            preset_name});
    }
    config.video_preset = *preset;
    return Result<AppConfig>::Ok(std::move(config));
}

// ============================================================
// load — read config file and parse
// ============================================================

Result<AppConfig> ConfigManager::load(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return Result<AppConfig>::Err(Error{
            ErrorCode::ConfigFileNotFound,
            "Configuration file not found: " + file_path,
            file_path});
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return parse(ss.str());
}

// ============================================================
// parse — parse key=value content string
// ============================================================

Result<AppConfig> ConfigManager::parse(const std::string& content) {
    auto sections_result = parse_ini(content);
    if (sections_result.is_err()) {
        return Result<AppConfig>::Err(sections_result.error());
    }
    return build_config(sections_result.value());
}

// ============================================================
// apply_cli_overrides — CLI arguments override config values
// ============================================================

Result<AppConfig> ConfigManager::apply_cli_overrides(
    const AppConfig& base, int argc, char* argv[]) {
    AppConfig config = base;

    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);

        auto consume_next = [&]() -> std::string {
            if (i + 1 < argc) return std::string(argv[++i]);
            return "";
        };

        if (arg == "--profile") {
            config.profile = consume_next();
        } else if (arg == "--preset") {
            auto preset_name = consume_next();
            auto result = apply_preset(config, preset_name);
            if (result.is_err()) return result;
            config = std::move(result).value();
        } else if (arg == "--width") {
            try { config.video_preset.width = static_cast<uint32_t>(std::stoul(consume_next())); }
            catch (...) {}
        } else if (arg == "--height") {
            try { config.video_preset.height = static_cast<uint32_t>(std::stoul(consume_next())); }
            catch (...) {}
        } else if (arg == "--fps") {
            try { config.video_preset.fps = static_cast<uint32_t>(std::stoul(consume_next())); }
            catch (...) {}
        } else if (arg == "--bitrate") {
            try { config.video_preset.bitrate_kbps = static_cast<uint32_t>(std::stoul(consume_next())); }
            catch (...) {}
        } else if (arg == "--camera-source") {
            config.camera_source = parse_camera_source(consume_next());
        } else if (arg == "--camera-device") {
            config.camera_device_path = consume_next();
        } else if (arg == "--cert-path") {
            config.iot.cert_path = consume_next();
        } else if (arg == "--key-path") {
            config.iot.key_path = consume_next();
        } else if (arg == "--root-ca-path") {
            config.iot.root_ca_path = consume_next();
        } else if (arg == "--credential-endpoint") {
            config.iot.credential_endpoint = consume_next();
        } else if (arg == "--role-alias") {
            config.iot.role_alias = consume_next();
        } else if (arg == "--thing-name") {
            config.iot.thing_name = consume_next();
        } else if (arg == "--stream-name") {
            config.kvs.stream_name = consume_next();
        } else if (arg == "--kvs-region") {
            config.kvs.region = consume_next();
        } else if (arg == "--channel-name") {
            config.webrtc.channel_name = consume_next();
        } else if (arg == "--webrtc-region") {
            config.webrtc.region = consume_next();
        } else if (arg == "--log-level") {
            config.log_level = consume_next();
        } else if (arg == "--log-file") {
            config.log_file_path = consume_next();
        } else if (arg == "--log-to-stdout") {
            config.log_to_stdout = parse_bool(consume_next());
        }
    }

    return Result<AppConfig>::Ok(std::move(config));
}

// ============================================================
// validate — check configuration validity
// ============================================================

VoidResult ConfigManager::validate(const AppConfig& config) {
    // Video parameter sanity checks
    if (config.video_preset.width > 1920) {
        return ErrVoid(ErrorCode::ConfigValidationError,
                       "Width " + std::to_string(config.video_preset.width) +
                       " exceeds maximum 1920",
                       "video.width");
    }
    if (config.video_preset.height > 1080) {
        return ErrVoid(ErrorCode::ConfigValidationError,
                       "Height " + std::to_string(config.video_preset.height) +
                       " exceeds maximum 1080",
                       "video.height");
    }
    if (config.video_preset.fps > 30) {
        return ErrVoid(ErrorCode::ConfigValidationError,
                       "FPS " + std::to_string(config.video_preset.fps) +
                       " exceeds maximum 30",
                       "video.fps");
    }

    // Profile-specific validation
    if (config.profile == "production") {
        // Production: all required fields must be present
        if (config.iot.cert_path.empty()) {
            return ErrVoid(ErrorCode::ConfigValidationError,
                           "Production profile requires iot.cert_path",
                           "iot.cert_path");
        }
        if (config.iot.key_path.empty()) {
            return ErrVoid(ErrorCode::ConfigValidationError,
                           "Production profile requires iot.key_path",
                           "iot.key_path");
        }
        if (config.iot.root_ca_path.empty()) {
            return ErrVoid(ErrorCode::ConfigValidationError,
                           "Production profile requires iot.root_ca_path",
                           "iot.root_ca_path");
        }
        if (config.kvs.stream_name.empty()) {
            return ErrVoid(ErrorCode::ConfigValidationError,
                           "Production profile requires kvs.stream_name",
                           "kvs.stream_name");
        }
        if (config.webrtc.channel_name.empty()) {
            return ErrVoid(ErrorCode::ConfigValidationError,
                           "Production profile requires webrtc.channel_name",
                           "webrtc.channel_name");
        }
    } else if (config.profile == "development") {
        // Development: missing IoT cert path is a warning (allow mock credentials)
        // We return Ok but the caller should log a WARNING if cert_path is empty.
        // Required fields for development: kvs.stream_name and webrtc.channel_name
        // are not strictly required in development mode.
    }

    return OkVoid();
}

// ============================================================
// format — Pretty Printer: config → key=value string
// ============================================================

std::string ConfigManager::format(const AppConfig& config) const {
    std::ostringstream out;

    out << "# Smart Camera Configuration\n";
    out << "# Profile: " << config.profile << "\n\n";

    out << "[iot]\n";
    out << "cert_path = " << config.iot.cert_path << "\n";
    out << "key_path = " << config.iot.key_path << "\n";
    out << "root_ca_path = " << config.iot.root_ca_path << "\n";
    out << "credential_endpoint = " << config.iot.credential_endpoint << "\n";
    out << "role_alias = " << config.iot.role_alias << "\n";
    out << "thing_name = " << config.iot.thing_name << "\n";

    out << "\n[video]\n";
    out << "preset = " << config.video_preset.name << "\n";
    out << "width = " << config.video_preset.width << "\n";
    out << "height = " << config.video_preset.height << "\n";
    out << "fps = " << config.video_preset.fps << "\n";
    out << "bitrate_kbps = " << config.video_preset.bitrate_kbps << "\n";
    out << "camera_source = " << camera_source_to_string(config.camera_source) << "\n";
    out << "camera_device_path = " << config.camera_device_path << "\n";

    out << "\n[kvs]\n";
    out << "stream_name = " << config.kvs.stream_name << "\n";
    out << "region = " << config.kvs.region << "\n";
    out << "retention_hours = " << config.kvs.retention_hours << "\n";
    out << "auto_create_stream = " << bool_to_string(config.kvs.auto_create_stream) << "\n";

    out << "\n[webrtc]\n";
    out << "channel_name = " << config.webrtc.channel_name << "\n";
    out << "region = " << config.webrtc.region << "\n";
    out << "max_viewers = " << config.webrtc.max_viewers << "\n";

    out << "\n[network]\n";
    out << "bitrate_min_kbps = " << config.bitrate_min_kbps << "\n";
    out << "bitrate_max_kbps = " << config.bitrate_max_kbps << "\n";
    out << "connection_check_interval_sec = " << config.connection_check_interval_sec << "\n";
    out << "connection_timeout_ms = " << config.connection_timeout_ms << "\n";

    out << "\n[logging]\n";
    out << "level = " << config.log_level << "\n";
    out << "file_path = " << config.log_file_path << "\n";
    out << "max_file_size_mb = " << config.log_max_file_size_mb << "\n";
    out << "max_files = " << config.log_max_files << "\n";
    out << "log_to_stdout = " << bool_to_string(config.log_to_stdout) << "\n";

    out << "\n[watchdog]\n";
    out << "interval_sec = " << config.watchdog_interval_sec << "\n";
    out << "error_threshold = " << config.watchdog_error_threshold << "\n";
    out << "stale_timeout_sec = " << config.watchdog_stale_timeout_sec << "\n";
    out << "memory_limit_mb = " << config.memory_limit_mb << "\n";
    out << "cpu_quota_percent = " << config.cpu_quota_percent << "\n";

    out << "\n[frame_pool]\n";
    out << "pool_size = " << config.frame_pool_size << "\n";

    out << "\n[ai]\n";
    out << "model_timeout_sec = " << config.ai_model_timeout_sec << "\n";

    return out.str();
}

}  // namespace sc
