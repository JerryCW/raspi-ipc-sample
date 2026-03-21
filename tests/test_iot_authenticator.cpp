#include <gtest/gtest.h>
#include "auth/iot_authenticator.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

using namespace sc;

namespace fs = std::filesystem;

// ============================================================
// Helper: temp directory for certificate file tests
// ============================================================

namespace {

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() / ("iot_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    std::string path() const { return path_.string(); }
    std::string file(const std::string& name) const {
        return (path_ / name).string();
    }
private:
    fs::path path_;
};

// Write content to a file with specified permissions
void write_file(const std::string& path, const std::string& content, mode_t perms = 0600) {
    std::ofstream f(path, std::ios::binary);
    f << content;
    f.close();
    chmod(path.c_str(), perms);
}

// Minimal self-signed PEM certificate (generated content for testing)
const char* kTestCertPEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBkTCB+wIUZz2NxQb3cFnFGqMM8JlGGTMHxCEwDQYJKoZIhvcNAQELBQAwFDES\n"
    "MBAGA1UEAwwJdGVzdC1yb290MB4XDTI0MDEwMTAwMDAwMFoXDTM0MDEwMTAwMMDAwMFo\n"
    "MBQXEJAGA1UEAwwJdGVzdC1yb290MFwwDQYJKoZIhvcNAQEBBQADSwAwSAJBAL5a\n"
    "c3+VQ5DAdbMFBYBfEBmi0IYAl+GPmRGPMNBJiCwMBBYBfEBmi0IYAl+GPmRGPMNB\n"
    "JiCwMBBYBfEBmi0IYAl+GPmRGPMNBJiCwMCAwEAATANBgkqhkiG9w0BAQsFAANB\n"
    "AJ5ac3+VQ5DAdbMFBYBfEBmi0IYAl+GPmRGPMNBJiCwMBBYBfEBmi0IYAl+GPmRG\n"
    "PMNBJiCwMBBYBfEBmi0IYAl+GPmRGPMNBJiCw=\n"
    "-----END CERTIFICATE-----\n";

const char* kTestKeyPEM =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC+WnN/lUOQwHWz\n"
    "BQWAXxAZotCGAJfhj5kRjzDQSYgsDAQWAXxAZotCGAJfhj5kRjzDQSYgsDAQWAXx\n"
    "AZotCGAJfhj5kRjzDQSYgsDAQWAXxAZotCGAJfhj5kRjzDQSYgsDAgMBAAECggEB\n"
    "AJ5ac3+VQ5DAdbMFBYBfEBmi0IYAl+GPmRGPMNBJiCwMBBYBfEBmi0IYAl+GPmRG\n"
    "PMNBJiCwMBBYBfEBmi0IYAl+GPmRGPMNBJiCwMBBYBfEBmi0IYAl+GPmRGPMNBJi\n"
    "CwMBBYBfEBmi0IYAl+GPmRGPMNBJiCwMBBYBfEBmi0IYAl+GPmRGPMNBJiCwMBBY\n"
    "-----END PRIVATE KEY-----\n";

const char* kTestCAPEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBkTCB+wIUZz2NxQb3cFnFGqMM8JlGGTMHxCEwDQYJKoZIhvcNAQELBQAwFDES\n"
    "MBAGA1UEAwwJdGVzdC1yb290MB4XDTI0MDEwMTAwMDAwMFoXDTM0MDEwMTAwMDAwMFo\n"
    "MBQXEJAGA1UEAwwJdGVzdC1yb290MFwwDQYJKoZIhvcNAQEBBQADSwAwSAJBAL5a\n"
    "c3+VQ5DAdbMFBYBfEBmi0IYAl+GPmRGPMNBJiCwMBBYBfEBmi0IYAl+GPmRGPMNB\n"
    "JiCwMBBYBfEBmi0IYAl+GPmRGPMNBJiCwMCAwEAATANBgkqhkiG9w0BAQsFAANB\n"
    "AJ5ac3+VQ5DAdbMFBYBfEBmi0IYAl+GPmRGPMNBJiCwMBBYBfEBmi0IYAl+GPmRG\n"
    "PMNBJiCwMBBYBfEBmi0IYAl+GPmRGPMNBJiCw=\n"
    "-----END CERTIFICATE-----\n";

const char* kNotPEMContent = "This is not a PEM file.\nJust plain text.\n";

// Build an IoTCertConfig pointing to files in a temp directory
IoTCertConfig make_config(const TempDir& tmp,
                          const std::string& cert = "cert.pem",
                          const std::string& key = "key.pem",
                          const std::string& ca = "ca.pem") {
    IoTCertConfig cfg;
    cfg.cert_path = tmp.file(cert);
    cfg.key_path = tmp.file(key);
    cfg.root_ca_path = tmp.file(ca);
    cfg.credential_endpoint = "test.credentials.iot.us-east-1.amazonaws.com";
    cfg.role_alias = "TestRole";
    cfg.thing_name = "test-thing";
    return cfg;
}

}  // namespace

// ============================================================
// 1. Certificate file validation
// Validates: Requirements 2.1, 2.8
// ============================================================

TEST(IoTAuthenticatorInit, FailsWhenCertFileNotExist) {
    TempDir tmp;
    // Create key and CA but NOT cert
    write_file(tmp.file("key.pem"), kTestKeyPEM, 0400);
    write_file(tmp.file("ca.pem"), kTestCAPEM);

    IoTAuthenticator auth;
    auto result = auth.initialize(make_config(tmp));
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::CertificateNotFound);
    EXPECT_NE(result.error().message.find("cert"), std::string::npos);
}

TEST(IoTAuthenticatorInit, FailsWhenKeyFileNotExist) {
    TempDir tmp;
    // Create cert and CA but NOT key
    write_file(tmp.file("cert.pem"), kTestCertPEM);
    write_file(tmp.file("ca.pem"), kTestCAPEM);

    IoTAuthenticator auth;
    auto result = auth.initialize(make_config(tmp));
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::CertificateNotFound);
    EXPECT_NE(result.error().message.find("key"), std::string::npos);
}

TEST(IoTAuthenticatorInit, FailsWhenRootCANotExist) {
    TempDir tmp;
    // Create cert and key but NOT CA
    write_file(tmp.file("cert.pem"), kTestCertPEM);
    write_file(tmp.file("key.pem"), kTestKeyPEM, 0400);

    IoTAuthenticator auth;
    auto result = auth.initialize(make_config(tmp));
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::CertificateNotFound);
    EXPECT_NE(result.error().message.find("CA"), std::string::npos);
}

TEST(IoTAuthenticatorInit, FailsWhenCertNotPEMFormat) {
    TempDir tmp;
    write_file(tmp.file("cert.pem"), kNotPEMContent);  // Not PEM
    write_file(tmp.file("key.pem"), kTestKeyPEM, 0400);
    write_file(tmp.file("ca.pem"), kTestCAPEM);

    IoTAuthenticator auth;
    auto result = auth.initialize(make_config(tmp));
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::CertificateInvalid);
}

TEST(IoTAuthenticatorInit, FailsWhenKeyHasInsecurePermissions) {
    TempDir tmp;
    write_file(tmp.file("cert.pem"), kTestCertPEM);
    write_file(tmp.file("key.pem"), kTestKeyPEM, 0644);  // group/other readable
    write_file(tmp.file("ca.pem"), kTestCAPEM);

    IoTAuthenticator auth;
    auto result = auth.initialize(make_config(tmp));
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::CertificateInvalid);
    EXPECT_NE(result.error().message.find("permission"), std::string::npos);
}

// ============================================================
// 2. PEM format detection (static method)
// Validates: Requirements 2.8
// ============================================================

TEST(IoTAuthenticatorPEM, ReturnsTrueForValidPEM) {
    TempDir tmp;
    write_file(tmp.file("valid.pem"), kTestCertPEM);
    EXPECT_TRUE(IoTAuthenticator::is_pem_format(tmp.file("valid.pem")));
}

TEST(IoTAuthenticatorPEM, ReturnsTrueForKeyPEM) {
    TempDir tmp;
    write_file(tmp.file("key.pem"), kTestKeyPEM);
    EXPECT_TRUE(IoTAuthenticator::is_pem_format(tmp.file("key.pem")));
}

TEST(IoTAuthenticatorPEM, ReturnsFalseForNonPEM) {
    TempDir tmp;
    write_file(tmp.file("not.pem"), kNotPEMContent);
    EXPECT_FALSE(IoTAuthenticator::is_pem_format(tmp.file("not.pem")));
}

TEST(IoTAuthenticatorPEM, ReturnsFalseForNonexistentFile) {
    EXPECT_FALSE(IoTAuthenticator::is_pem_format("/nonexistent/path/file.pem"));
}

TEST(IoTAuthenticatorPEM, ReturnsFalseForEmptyFile) {
    TempDir tmp;
    write_file(tmp.file("empty.pem"), "");
    EXPECT_FALSE(IoTAuthenticator::is_pem_format(tmp.file("empty.pem")));
}

// ============================================================
// 3. Key permissions (static method)
// Validates: Requirements 2.1, 15.4
// ============================================================

TEST(IoTAuthenticatorPerms, Accepts0400) {
    TempDir tmp;
    write_file(tmp.file("key.pem"), kTestKeyPEM, 0400);
    EXPECT_TRUE(IoTAuthenticator::check_key_permissions(tmp.file("key.pem")));
}

TEST(IoTAuthenticatorPerms, Accepts0600) {
    TempDir tmp;
    write_file(tmp.file("key.pem"), kTestKeyPEM, 0600);
    EXPECT_TRUE(IoTAuthenticator::check_key_permissions(tmp.file("key.pem")));
}

TEST(IoTAuthenticatorPerms, Rejects0644) {
    TempDir tmp;
    write_file(tmp.file("key.pem"), kTestKeyPEM, 0644);
    EXPECT_FALSE(IoTAuthenticator::check_key_permissions(tmp.file("key.pem")));
}

TEST(IoTAuthenticatorPerms, Rejects0777) {
    TempDir tmp;
    write_file(tmp.file("key.pem"), kTestKeyPEM, 0777);
    EXPECT_FALSE(IoTAuthenticator::check_key_permissions(tmp.file("key.pem")));
}

TEST(IoTAuthenticatorPerms, ReturnsFalseForNonexistent) {
    EXPECT_FALSE(IoTAuthenticator::check_key_permissions("/nonexistent/key.pem"));
}

// ============================================================
// 4. Credential validity — not initialized
// Validates: Requirements 2.2, 2.3
// ============================================================

TEST(IoTAuthenticatorCredential, IsCredentialValidReturnsFalseWhenNotInitialized) {
    IoTAuthenticator auth;
    EXPECT_FALSE(auth.is_credential_valid());
}

TEST(IoTAuthenticatorCredential, GetCredentialsFailsWhenNotInitialized) {
    IoTAuthenticator auth;
    auto result = auth.get_credentials();
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::CredentialRefreshFailed);
    EXPECT_NE(result.error().message.find("not initialized"), std::string::npos);
}

TEST(IoTAuthenticatorCredential, ForceRefreshFailsWhenNotInitialized) {
    IoTAuthenticator auth;
    auto result = auth.force_refresh();
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::CredentialRefreshFailed);
}

// ============================================================
// 5. Constants verification
// Validates: Requirements 2.2, 2.4
// ============================================================

TEST(IoTAuthenticatorConstants, MaxRetries) {
    EXPECT_EQ(IoTAuthenticator::kMaxRetries, 10);
}

TEST(IoTAuthenticatorConstants, CredentialRefreshBeforeExpiry) {
    EXPECT_EQ(IoTAuthenticator::kCredentialRefreshBeforeExpirySec, 300);
}

TEST(IoTAuthenticatorConstants, InitialRetryDelay) {
    EXPECT_EQ(IoTAuthenticator::kInitialRetryDelaySec, 1);
}

TEST(IoTAuthenticatorConstants, MaxRetryDelay) {
    EXPECT_EQ(IoTAuthenticator::kMaxRetryDelaySec, 60);
}

TEST(IoTAuthenticatorConstants, CertExpiryWarningDays) {
    EXPECT_EQ(IoTAuthenticator::kCertExpiryWarningDays, 30);
}
