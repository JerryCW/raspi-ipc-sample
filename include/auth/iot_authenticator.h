#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "config/config_manager.h"
#include "core/types.h"

namespace sc {

// ============================================================
// AWSCredentials — STS temporary credentials
// ============================================================

struct AWSCredentials {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
    std::chrono::system_clock::time_point expiration;
};

// ============================================================
// IIoTAuthenticator — IoT X.509 mTLS authentication interface
// ============================================================

class IIoTAuthenticator {
public:
    virtual ~IIoTAuthenticator() = default;

    virtual VoidResult initialize(const IoTCertConfig& config) = 0;
    virtual Result<AWSCredentials> get_credentials() = 0;
    virtual bool is_credential_valid() const = 0;
    virtual VoidResult force_refresh() = 0;

    // Certificate expiry check
    virtual std::optional<std::chrono::system_clock::time_point>
        certificate_expiry() const = 0;
};

// ============================================================
// IoTAuthenticator — concrete implementation
// ============================================================

class IoTAuthenticator : public IIoTAuthenticator {
public:
    IoTAuthenticator() = default;
    ~IoTAuthenticator() override;

    VoidResult initialize(const IoTCertConfig& config) override;
    Result<AWSCredentials> get_credentials() override;
    bool is_credential_valid() const override;
    VoidResult force_refresh() override;

    std::optional<std::chrono::system_clock::time_point>
        certificate_expiry() const override;

    // Exposed for testing
    static bool is_pem_format(const std::string& file_path);
    static bool check_key_permissions(const std::string& file_path);

    // Constants
    static constexpr int kMaxRetries = 10;
    static constexpr int kInitialRetryDelaySec = 1;
    static constexpr int kMaxRetryDelaySec = 60;
    static constexpr int kCredentialRefreshBeforeExpirySec = 300;  // 5 minutes
    static constexpr int kCertExpiryWarningDays = 30;

private:
    VoidResult validate_certificate_files() const;
    VoidResult validate_pem_and_chain() const;
    Result<AWSCredentials> fetch_credentials_from_iot();
    Result<AWSCredentials> fetch_with_retry();
    void clear_private_key_memory();
    Result<std::string> load_private_key() const;

    // Parse JSON credential response from IoT Credential Provider
    static Result<AWSCredentials> parse_credential_response(const std::string& json);

    IoTCertConfig config_;
    bool initialized_ = false;

    // Thread-safe credential storage
    mutable std::shared_mutex credential_mutex_;
    AWSCredentials cached_credentials_;
    bool has_cached_credentials_ = false;

    // Retry state
    int consecutive_failures_ = 0;

    // Private key memory security
    std::vector<char> private_key_buffer_;
};

}  // namespace sc
