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

// Generate a self-signed CA cert + key via openssl CLI.
// Returns true on success.
bool generate_ca(const std::string& cert_path,
                 const std::string& key_path,
                 const std::string& cn = "TestCA") {
    std::string cmd = "openssl req -x509 -newkey rsa:2048"
                      " -keyout " + key_path +
                      " -out " + cert_path +
                      " -days 3650 -nodes"
                      " -subj /CN=" + cn +
                      " 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

// Generate a device cert signed by a given CA.
// Returns true on success.
bool generate_signed_cert(const std::string& cert_path,
                          const std::string& key_path,
                          const std::string& ca_cert_path,
                          const std::string& ca_key_path,
                          const std::string& cn = "TestDevice") {
    std::string csr_path = key_path + ".csr";
    std::string cmd1 = "openssl req -newkey rsa:2048"
                       " -keyout " + key_path +
                       " -out " + csr_path +
                       " -nodes -subj /CN=" + cn +
                       " 2>/dev/null";
    if (std::system(cmd1.c_str()) != 0) return false;

    std::string cmd2 = "openssl x509 -req"
                       " -in " + csr_path +
                       " -CA " + ca_cert_path +
                       " -CAkey " + ca_key_path +
                       " -CAcreateserial"
                       " -out " + cert_path +
                       " -days 3650 2>/dev/null";
    return std::system(cmd2.c_str()) == 0;
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

// ============================================================
// 6. Certificate chain verification — behavioral change
// After the fix, initialize() succeeds even when the device
// cert was signed by a different CA than the provided root CA.
// Full chain verification happens server-side during mTLS.
// Validates: Key Gotcha "AWS IoT cert validation: use
// X509_V_FLAG_PARTIAL_CHAIN; full chain verification
// happens server-side during mTLS."
// ============================================================

TEST(IoTAuthenticatorChain, ValidChain_InitializeSucceeds) {
    TempDir tmp;

    // Generate CA + device cert signed by that CA
    ASSERT_TRUE(generate_ca(tmp.file("ca.pem"), tmp.file("ca_key.pem"), "TestCA"));
    ASSERT_TRUE(generate_signed_cert(
        tmp.file("cert.pem"), tmp.file("key.pem"),
        tmp.file("ca.pem"), tmp.file("ca_key.pem"), "TestDevice"));
    chmod(tmp.file("key.pem").c_str(), 0400);

    IoTAuthenticator auth;
    auto result = auth.initialize(make_config(tmp));
    EXPECT_TRUE(result.is_ok()) << "Valid chain should succeed: "
        << (result.is_err() ? result.error().message : "");
}

TEST(IoTAuthenticatorChain, MismatchedCA_InitializeSucceeds) {
    TempDir tmp;

    // Generate CA-A and sign device cert with it
    ASSERT_TRUE(generate_ca(tmp.file("ca_a.pem"), tmp.file("ca_a_key.pem"), "CA_A"));
    ASSERT_TRUE(generate_signed_cert(
        tmp.file("cert.pem"), tmp.file("key.pem"),
        tmp.file("ca_a.pem"), tmp.file("ca_a_key.pem"), "Device"));
    chmod(tmp.file("key.pem").c_str(), 0400);

    // Generate unrelated CA-B and use it as root_ca_path
    ASSERT_TRUE(generate_ca(tmp.file("ca.pem"), tmp.file("ca_b_key.pem"), "CA_B"));

    // Device cert signed by CA-A, but root CA is CA-B → chain mismatch
    // After the fix, this should succeed (warning logged, not error)
    IoTAuthenticator auth;
    auto result = auth.initialize(make_config(tmp));
    EXPECT_TRUE(result.is_ok()) << "Mismatched CA should succeed (warning only): "
        << (result.is_err() ? result.error().message : "");
}

TEST(IoTAuthenticatorChain, SelfSignedCert_InitializeSucceeds) {
    TempDir tmp;

    // Generate a self-signed cert (acts as both device cert and CA)
    ASSERT_TRUE(generate_ca(tmp.file("cert.pem"), tmp.file("key.pem"), "SelfSigned"));
    chmod(tmp.file("key.pem").c_str(), 0400);

    // Copy cert to ca.pem (same cert used as root CA)
    {
        std::ifstream src(tmp.file("cert.pem"), std::ios::binary);
        std::ofstream dst(tmp.file("ca.pem"), std::ios::binary);
        dst << src.rdbuf();
    }  // streams closed here

    IoTAuthenticator auth;
    auto result = auth.initialize(make_config(tmp));
    EXPECT_TRUE(result.is_ok()) << "Self-signed cert should succeed: "
        << (result.is_err() ? result.error().message : "");
}
