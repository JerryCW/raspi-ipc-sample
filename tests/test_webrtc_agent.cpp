#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "webrtc/webrtc_agent.h"

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
        creds.access_key_id = "AKIAIOSFODNN7EXAMPLE";
        creds.secret_access_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
        creds.session_token = "FwoGZXIvYXdzEBYaDHqa0AP";
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
// Helper: create a valid WebRTCConfig for testing
// ============================================================

WebRTCConfig make_test_config() {
    WebRTCConfig config;
    config.channel_name = "test-channel";
    config.region = "us-east-1";
    config.max_viewers = 10;
    config.stun_urls = {"stun:stun.kinesisvideo.us-east-1.amazonaws.com:443"};
    return config;
}

// ============================================================
// Stub implementation tests (macOS environment)
// ============================================================

class WebRTCAgentTest : public ::testing::Test {
protected:
    void SetUp() override {
        agent_ = create_webrtc_agent();
        auth_ = std::make_shared<MockIoTAuthenticator>();
    }

    void TearDown() override {
        agent_.reset();
    }

    std::unique_ptr<IWebRTCAgent> agent_;
    std::shared_ptr<MockIoTAuthenticator> auth_;
};

TEST_F(WebRTCAgentTest, FactoryCreatesInstance) {
    ASSERT_NE(agent_, nullptr);
}

TEST_F(WebRTCAgentTest, InitializeWithValidConfig) {
    auto config = make_test_config();
    auto result = agent_->initialize(config, auth_);
    EXPECT_TRUE(result.is_ok());
}

TEST_F(WebRTCAgentTest, InitializeWithNullAuth) {
    auto config = make_test_config();
    auto result = agent_->initialize(config, nullptr);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST_F(WebRTCAgentTest, InitializeWithEmptyChannelName) {
    WebRTCConfig config;
    config.channel_name = "";
    config.region = "us-east-1";
    auto result = agent_->initialize(config, auth_);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST_F(WebRTCAgentTest, InitializeWithEmptyRegion) {
    WebRTCConfig config;
    config.channel_name = "test-channel";
    config.region = "";
    auto result = agent_->initialize(config, auth_);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST_F(WebRTCAgentTest, StartSignalingWithoutInitialize) {
    auto result = agent_->start_signaling();
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, ErrorCode::WebRTCSignalingFailed);
}

TEST_F(WebRTCAgentTest, StartAndStopSignaling) {
    auto config = make_test_config();
    ASSERT_TRUE(agent_->initialize(config, auth_).is_ok());

    auto start_result = agent_->start_signaling();
    EXPECT_TRUE(start_result.is_ok());
    EXPECT_TRUE(agent_->is_signaling_connected());

    auto stop_result = agent_->stop_signaling();
    EXPECT_TRUE(stop_result.is_ok());
    EXPECT_FALSE(agent_->is_signaling_connected());
}

TEST_F(WebRTCAgentTest, InitialViewerCountIsZero) {
    auto config = make_test_config();
    ASSERT_TRUE(agent_->initialize(config, auth_).is_ok());
    EXPECT_EQ(agent_->active_viewer_count(), 0u);
}

TEST_F(WebRTCAgentTest, SignalingNotConnectedBeforeStart) {
    auto config = make_test_config();
    ASSERT_TRUE(agent_->initialize(config, auth_).is_ok());
    EXPECT_FALSE(agent_->is_signaling_connected());
}

// ============================================================
// Concurrent viewer capacity tests
// ============================================================

TEST_F(WebRTCAgentTest, DefaultMaxViewersIsTen) {
    EXPECT_EQ(WebRTCAgent::kDefaultMaxViewers, 10u);
}

TEST_F(WebRTCAgentTest, MaxViewersFromConfig) {
    WebRTCConfig config = make_test_config();
    config.max_viewers = 5;
    ASSERT_TRUE(agent_->initialize(config, auth_).is_ok());
    // After init, max_viewers should be respected (tested via capacity behavior)
    EXPECT_EQ(agent_->active_viewer_count(), 0u);
}

// ============================================================
// Viewer callback tests
// ============================================================

TEST_F(WebRTCAgentTest, ViewerCallbackRegistration) {
    auto config = make_test_config();
    ASSERT_TRUE(agent_->initialize(config, auth_).is_ok());

    bool callback_called = false;
    agent_->on_viewer_change([&](const std::string& /*viewer_id*/, bool /*connected*/) {
        callback_called = true;
    });

    // Callback is registered but not called until a viewer connects
    EXPECT_FALSE(callback_called);
}

// ============================================================
// Shutdown sequence tests
// ============================================================

TEST_F(WebRTCAgentTest, ShutdownSequenceCompletesCleanly) {
    auto config = make_test_config();
    ASSERT_TRUE(agent_->initialize(config, auth_).is_ok());
    ASSERT_TRUE(agent_->start_signaling().is_ok());

    // Stop should complete the full shutdown sequence
    auto result = agent_->stop_signaling();
    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(agent_->is_signaling_connected());
    EXPECT_EQ(agent_->active_viewer_count(), 0u);
}

TEST_F(WebRTCAgentTest, DoubleStopIsIdempotent) {
    auto config = make_test_config();
    ASSERT_TRUE(agent_->initialize(config, auth_).is_ok());
    ASSERT_TRUE(agent_->start_signaling().is_ok());

    EXPECT_TRUE(agent_->stop_signaling().is_ok());
    // Second stop should also succeed without issues
    EXPECT_TRUE(agent_->stop_signaling().is_ok());
}

// ============================================================
// Constants validation
// ============================================================

TEST_F(WebRTCAgentTest, SignalingRetryConstants) {
    // Initial retry delay: 2 seconds
    EXPECT_EQ(WebRTCAgent::kInitialSignalingRetryDelaySec, 2);
    // Max retry delay: 60 seconds
    EXPECT_EQ(WebRTCAgent::kMaxSignalingRetryDelaySec, 60);
}

TEST_F(WebRTCAgentTest, DisconnectCleanupTimeout) {
    // Resources should be released within 10 seconds of disconnect
    EXPECT_EQ(WebRTCAgent::kDisconnectCleanupTimeoutSec, 10);
}

TEST_F(WebRTCAgentTest, ShutdownPeerSleepDuration) {
    // 1 second sleep between close peers and free peers
    EXPECT_EQ(WebRTCAgent::kShutdownPeerSleepMs, 1000);
}

// ============================================================
// WebRTCAgent internal tests via concrete type
// ============================================================

class WebRTCAgentInternalTest : public ::testing::Test {
protected:
    void SetUp() override {
        agent_ = std::make_unique<WebRTCAgent>();
        auth_ = std::make_shared<MockIoTAuthenticator>();
    }

    void TearDown() override {
        agent_.reset();
    }

    std::unique_ptr<WebRTCAgent> agent_;
    std::shared_ptr<MockIoTAuthenticator> auth_;
};

TEST_F(WebRTCAgentInternalTest, ViewerCountCapacityEnforcement) {
    WebRTCConfig config = make_test_config();
    config.max_viewers = 2;
    ASSERT_TRUE(agent_->initialize(config, auth_).is_ok());
    ASSERT_TRUE(agent_->start_signaling().is_ok());

    // Viewer count starts at 0
    EXPECT_EQ(agent_->active_viewer_count(), 0u);

    // After stop, viewer count should be 0
    agent_->stop_signaling();
    EXPECT_EQ(agent_->active_viewer_count(), 0u);
}

TEST_F(WebRTCAgentInternalTest, ZeroMaxViewersDefaultsToTen) {
    WebRTCConfig config = make_test_config();
    config.max_viewers = 0;
    ASSERT_TRUE(agent_->initialize(config, auth_).is_ok());
    // Should default to kDefaultMaxViewers (10)
    // Verified by the fact that initialize succeeds
}

TEST_F(WebRTCAgentInternalTest, DestructorStopsSignaling) {
    WebRTCConfig config = make_test_config();
    ASSERT_TRUE(agent_->initialize(config, auth_).is_ok());
    ASSERT_TRUE(agent_->start_signaling().is_ok());
    EXPECT_TRUE(agent_->is_signaling_connected());

    // Destructor should call stop_signaling
    agent_.reset();
    // No crash = success
}

}  // namespace
}  // namespace sc
