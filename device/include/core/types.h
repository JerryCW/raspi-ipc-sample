#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <variant>

namespace sc {

// ============================================================
// ErrorCode — categorized by subsystem
//   General: 0-99, Config: 100-199, Auth: 200-299,
//   Camera: 300-399, Pipeline: 400-499, Network: 500-599,
//   Resource: 600-699, AI: 700-799
// ============================================================

enum class ErrorCode : uint16_t {
    // General (0-99)
    OK = 0,
    Unknown = 1,
    InvalidArgument = 2,
    Timeout = 3,
    NotFound = 4,

    // Config (100-199)
    ConfigFileNotFound = 100,
    ConfigParseError = 101,
    ConfigValidationError = 102,
    ConfigPresetNotFound = 103,

    // Auth (200-299)
    CertificateNotFound = 200,
    CertificateInvalid = 201,
    CredentialRefreshFailed = 202,
    MTLSHandshakeFailed = 203,

    // Camera (300-399)
    CameraDeviceNotFound = 300,
    CameraOpenFailed = 301,
    CameraCapabilityQueryFailed = 302,

    // Pipeline (400-499)
    PipelineBuildFailed = 400,
    PipelineStateChangeFailed = 401,
    EncoderNotAvailable = 402,
    NALUnitInvalid = 403,

    // Network / Stream (500-599)
    KVSConnectionFailed = 500,
    KVSStreamCreateFailed = 501,
    WebRTCSignalingFailed = 502,
    WebRTCCapacityFull = 503,
    EndpointUnreachable = 504,

    // Resource (600-699)
    MemoryLimitExceeded = 600,
    BufferPoolExhausted = 601,

    // AI (700-799)
    AIModelTimeout = 700,
    AIModelLoadFailed = 701,
};

// ============================================================
// Error — carries code + human-readable message + context
// ============================================================

struct Error {
    ErrorCode code;
    std::string message;
    std::string context;  // e.g. file path, module name
};

// ============================================================
// Result<T> — typed error handling (no exceptions)
//   Internally uses std::variant<T, Error>
// ============================================================

template <typename T>
class Result {
public:
    static Result Ok(T value) { return Result(std::move(value)); }
    static Result Err(Error error) { return Result(std::move(error)); }

    bool is_ok() const { return std::holds_alternative<T>(data_); }
    bool is_err() const { return !is_ok(); }

    const T& value() const& { return std::get<T>(data_); }
    T& value() & { return std::get<T>(data_); }
    T&& value() && { return std::get<T>(std::move(data_)); }

    const Error& error() const& { return std::get<Error>(data_); }
    Error& error() & { return std::get<Error>(data_); }

    // Chain: and_then(f) — f(T) -> Result<U>
    template <typename F>
    auto and_then(F&& f) const -> decltype(f(std::declval<const T&>())) {
        using RetType = decltype(f(std::declval<const T&>()));
        if (is_ok()) return f(value());
        return RetType::Err(error());
    }

    // Chain: map(f) — f(T) -> U, wraps in Result<U>
    template <typename F>
    auto map(F&& f) const -> Result<decltype(f(std::declval<const T&>()))> {
        using U = decltype(f(std::declval<const T&>()));
        if (is_ok()) return Result<U>::Ok(f(value()));
        return Result<U>::Err(error());
    }

private:
    explicit Result(T value) : data_(std::move(value)) {}
    explicit Result(Error error) : data_(std::move(error)) {}

    std::variant<T, Error> data_;
};

// ============================================================
// VoidResult — Result with no payload
// ============================================================

using VoidResult = Result<std::monostate>;

// Convenience helpers for VoidResult
inline VoidResult OkVoid() {
    return VoidResult::Ok(std::monostate{});
}

inline VoidResult ErrVoid(ErrorCode code, std::string message, std::string context = "") {
    return VoidResult::Err(Error{code, std::move(message), std::move(context)});
}

}  // namespace sc
