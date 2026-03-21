#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "stream/stream_uploader.h"

namespace sc {
namespace {

// ============================================================
// Mock IoT Authenticator for testing
// ============================================================

class MockIoTAuthenticator : public IIoTAuthenticator {
public:
    VoidResult initialize(const IoTCertConfig& /*config*/) override {
        return OkVoid();
    }

    Result<AWSCredentials> get_credentials() override {
        AWSCredentials creds;
        creds.access_key_id = "MOCK_ACCESS_KEY";
        creds.secret_access_key = "MOCK_SECRET_KEY";
        creds.session_token = "MOCK_SESSION_TOKEN";
        creds.expiration = std::chrono::system_clock::now() + std::chrono::hours(1);
        return Result<AWSCredentials>::Ok(std::move(creds));
    }

    bool is_credential_valid() const override { return true; }

    VoidResult force_refresh() override { return OkVoid(); }

    std::optional<std::chrono::system_clock::time_point>
    certificate_expiry() const override {
        return std::chrono::system_clock::now() + std::chrono::hours(24 * 365);
    }
};

// ============================================================
// Helper: create default KVSStreamConfig
// ============================================================

KVSStreamConfig make_default_config() {
    KVSStreamConfig config;
    config.stream_name = "test-stream";
    config.region = "ap-northeast-1";
    config.retention_hours = 168;
    config.auto_create_stream = true;
    return config;
}

// ============================================================
// Tests: iot-certificate string format
// ============================================================

TEST(StreamUploaderTest, BuildIotCertificateString_CorrectFormat) {
    IoTCertConfig iot;
    iot.thing_name = "my-camera";
    iot.credential_endpoint = "xxxx.credentials.iot.ap-northeast-1.amazonaws.com";
    iot.cert_path = "/etc/certs/device.cert.pem";
    iot.key_path = "/etc/certs/device.private.key";
    iot.root_ca_path = "/etc/certs/AmazonRootCA1.pem";
    iot.role_alias = "SmartCameraRole";

    auto result = StreamUploader::build_iot_certificate_string(iot);

    // Verify comma-separated key=value format
    EXPECT_NE(result.find("iot-thing-name=my-camera"), std::string::npos);
    EXPECT_NE(result.find("endpoint=xxxx.credentials.iot.ap-northeast-1.amazonaws.com"),
              std::string::npos);
    EXPECT_NE(result.find("cert-path=/etc/certs/device.cert.pem"), std::string::npos);
    EXPECT_NE(result.find("key-path=/etc/certs/device.private.key"), std::string::npos);
    EXPECT_NE(result.find("ca-path=/etc/certs/AmazonRootCA1.pem"), std::string::npos);
    EXPECT_NE(result.find("role-aliases=SmartCameraRole"), std::string::npos);

    // Verify fields are comma-separated
    // Count commas — should be 5 (6 fields, 5 separators)
    auto comma_count = std::count(result.begin(), result.end(), ',');
    EXPECT_EQ(comma_count, 5);
}

TEST(StreamUploaderTest, BuildIotCertificateString_AllFieldsPresent) {
    IoTCertConfig iot;
    iot.thing_name = "thing";
    iot.credential_endpoint = "endpoint";
    iot.cert_path = "cert";
    iot.key_path = "key";
    iot.root_ca_path = "ca";
    iot.role_alias = "role";

    auto result = StreamUploader::build_iot_certificate_string(iot);

    // Should start with iot-thing-name
    EXPECT_EQ(result.substr(0, 15), "iot-thing-name=");

    // Should not contain spaces (kvssink expects no spaces in the string)
    // Only if the input values have no spaces
    EXPECT_EQ(result.find(' '), std::string::npos);
}

// ============================================================
// Tests: initialization
// ============================================================

TEST(StreamUploaderTest, Initialize_Success) {
    auto uploader = create_stream_uploader();
    auto auth = std::make_shared<MockIoTAuthenticator>();
    auto config = make_default_config();

    auto result = uploader->initialize(config, auth);
    EXPECT_TRUE(result.is_ok());
}

TEST(StreamUploaderTest, Initialize_NullAuth) {
    auto uploader = create_stream_uploader();
    auto config = make_default_config();

    auto result = uploader->initialize(config, nullptr);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(StreamUploaderTest, Initialize_EmptyStreamName) {
    auto uploader = create_stream_uploader();
    auto auth = std::make_shared<MockIoTAuthenticator>();
    KVSStreamConfig config;
    config.stream_name = "";
    config.region = "us-east-1";

    auto result = uploader->initialize(config, auth);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(StreamUploaderTest, Initialize_EmptyRegion) {
    auto uploader = create_stream_uploader();
    auto auth = std::make_shared<MockIoTAuthenticator>();
    KVSStreamConfig config;
    config.stream_name = "test-stream";
    config.region = "";

    auto result = uploader->initialize(config, auth);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(StreamUploaderTest, Initialize_DefaultRetention) {
    auto uploader = create_stream_uploader();
    auto auth = std::make_shared<MockIoTAuthenticator>();
    KVSStreamConfig config;
    config.stream_name = "test-stream";
    config.region = "us-east-1";
    config.retention_hours = 0;  // Should default to 168 (7 days)
    config.auto_create_stream = true;

    auto result = uploader->initialize(config, auth);
    EXPECT_TRUE(result.is_ok());
}

// ============================================================
// Tests: start / stop lifecycle
// ============================================================

TEST(StreamUploaderTest, Start_WithoutInitialize_Fails) {
    auto uploader = create_stream_uploader();

    auto result = uploader->start();
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::KVSConnectionFailed);
}

TEST(StreamUploaderTest, StartStop_Lifecycle) {
    auto uploader = create_stream_uploader();
    auto auth = std::make_shared<MockIoTAuthenticator>();
    auto config = make_default_config();

    ASSERT_TRUE(uploader->initialize(config, auth).is_ok());

    auto start_result = uploader->start();
    EXPECT_TRUE(start_result.is_ok());
    EXPECT_TRUE(uploader->is_connected());

    auto stop_result = uploader->stop();
    EXPECT_TRUE(stop_result.is_ok());
    EXPECT_FALSE(uploader->is_connected());
}

// ============================================================
// Tests: connection state and bytes uploaded
// ============================================================

TEST(StreamUploaderTest, IsConnected_InitiallyFalse) {
    auto uploader = create_stream_uploader();
    EXPECT_FALSE(uploader->is_connected());
}

TEST(StreamUploaderTest, BytesUploaded_InitiallyZero) {
    auto uploader = create_stream_uploader();
    EXPECT_EQ(uploader->bytes_uploaded(), 0u);
}

// ============================================================
// Tests: flush buffer
// ============================================================

TEST(StreamUploaderTest, FlushBuffer_Success) {
    auto uploader = create_stream_uploader();
    auto auth = std::make_shared<MockIoTAuthenticator>();
    auto config = make_default_config();

    ASSERT_TRUE(uploader->initialize(config, auth).is_ok());
    ASSERT_TRUE(uploader->start().is_ok());

    auto result = uploader->flush_buffer();
    EXPECT_TRUE(result.is_ok());
}

// ============================================================
// Tests: storage-size unit verification (MB, NOT * 1024)
// ============================================================

TEST(StreamUploaderTest, StorageSizeConstant_IsMB) {
    // Verify the storage-size constant is in MB (not bytes, not KB)
    // kvssink storage-size property is already in MB — do NOT multiply by 1024
    EXPECT_EQ(StreamUploader::kDefaultStorageSizeMB, 128u);

    // Ensure it's a reasonable MB value (not accidentally multiplied)
    EXPECT_LT(StreamUploader::kDefaultStorageSizeMB, 1024u);
    EXPECT_GT(StreamUploader::kDefaultStorageSizeMB, 0u);
}

// ============================================================
// Tests: reconnection exponential backoff
// ============================================================

TEST(StreamUploaderTest, ReconnectConstants) {
    // Initial delay: 1 second
    EXPECT_EQ(StreamUploader::kInitialReconnectDelaySec, 1);
    // Max delay: 30 seconds
    EXPECT_EQ(StreamUploader::kMaxReconnectDelaySec, 30);
}

TEST(StreamUploaderTest, ExponentialBackoff_Sequence) {
    // Verify the exponential backoff sequence: 1, 2, 4, 8, 16, 30, 30, ...
    int delay = StreamUploader::kInitialReconnectDelaySec;
    EXPECT_EQ(delay, 1);

    delay = std::min(delay * 2, StreamUploader::kMaxReconnectDelaySec);
    EXPECT_EQ(delay, 2);

    delay = std::min(delay * 2, StreamUploader::kMaxReconnectDelaySec);
    EXPECT_EQ(delay, 4);

    delay = std::min(delay * 2, StreamUploader::kMaxReconnectDelaySec);
    EXPECT_EQ(delay, 8);

    delay = std::min(delay * 2, StreamUploader::kMaxReconnectDelaySec);
    EXPECT_EQ(delay, 16);

    // Next would be 32, but capped at 30
    delay = std::min(delay * 2, StreamUploader::kMaxReconnectDelaySec);
    EXPECT_EQ(delay, 30);

    // Should stay at 30
    delay = std::min(delay * 2, StreamUploader::kMaxReconnectDelaySec);
    EXPECT_EQ(delay, 30);
}

// ============================================================
// Tests: default retention hours
// ============================================================

TEST(StreamUploaderTest, DefaultRetentionHours_SevenDays) {
    EXPECT_EQ(StreamUploader::kDefaultRetentionHours, 168u);  // 7 * 24 = 168
}

// ============================================================
// Tests: factory function
// ============================================================

TEST(StreamUploaderTest, Factory_CreatesInstance) {
    auto uploader = create_stream_uploader();
    EXPECT_NE(uploader, nullptr);
}

}  // namespace
}  // namespace sc
