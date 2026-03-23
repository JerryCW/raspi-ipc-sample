// ============================================================
// test_frame_exporter.cpp — Unit tests (Task 2.5) and
// Property-based tests (Tasks 2.6, 2.7) for FrameExporter
// ============================================================

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// POSIX IPC
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ai/frame_exporter.h"
#include "buffer/frame_buffer_pool.h"

using namespace sc;

// ============================================================
// Helpers
// ============================================================

static FrameInfo make_info(uint64_t seq, uint32_t w = 640, uint32_t h = 480) {
    FrameInfo fi{};
    fi.width = w;
    fi.height = h;
    fi.stride = w * 3;
    fi.timestamp = std::chrono::steady_clock::now();
    fi.sequence_number = seq;
    return fi;
}

static std::vector<uint8_t> make_data(size_t size = 128) {
    return std::vector<uint8_t>(size, 0xAB);
}

/// Generate a unique shm name for test isolation
static std::string unique_shm_name() {
    static int counter = 0;
    return "/test_fe_shm_" + std::to_string(::getpid()) + "_" + std::to_string(counter++);
}

/// Generate a unique socket path for test isolation
static std::string unique_socket_path() {
    static int counter = 0;
    return "/tmp/test_fe_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".sock";
}

/// Build a FrameExporterConfig with unique names and very high fps
/// so should_sample_frame() always passes in unit tests.
static FrameExporterConfig make_test_config(double fps = 100000.0) {
    FrameExporterConfig cfg;
    cfg.shm_name = unique_shm_name();
    cfg.shm_size = 1 * 1024 * 1024;  // 1MB for tests
    cfg.socket_path = unique_socket_path();
    cfg.target_fps = fps;
    return cfg;
}

/// Connect a Unix socket client to the given path. Returns fd or -1.
static int connect_client(const std::string& socket_path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Read a newline-delimited JSON message from a socket fd.
static std::string read_json_message(int fd, int timeout_ms = 2000) {
    std::string buf;
    buf.reserve(4096);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        char c;
        ssize_t n = ::recv(fd, &c, 1, MSG_DONTWAIT);
        if (n == 1) {
            if (c == '\n') return buf;
            buf += c;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return buf;
}

/// Call analyze() in a loop until at least one frame is exported,
/// to handle the should_sample_frame() timing gate.
/// Returns the number of analyze() calls made.
static int analyze_until_exported(FrameExporter& exporter, size_t data_size = 256,
                                  uint64_t seq = 1, int max_attempts = 50) {
    uint64_t before = exporter.frames_exported();
    for (int i = 0; i < max_attempts; ++i) {
        FrameBuffer fb(make_data(data_size), make_info(seq));
        exporter.analyze(fb);
        if (exporter.frames_exported() > before) return i + 1;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return max_attempts;
}

// ============================================================
// Task 2.5: Unit Tests
// ============================================================

namespace {

// --- 2.5.1: Shared memory creation and RAII cleanup ---
// **Validates: Requirements 1.2**

TEST(FrameExporterUnit, SharedMemoryCreatedOnConstruction) {
    auto cfg = make_test_config();

    {
        FrameExporter exporter(cfg);

        // Verify shm segment exists by opening it read-only
        int fd = ::shm_open(cfg.shm_name.c_str(), O_RDONLY, 0);
        ASSERT_NE(fd, -1) << "Shared memory segment should exist after construction";

        // Verify size via fstat
        struct stat st{};
        ASSERT_EQ(::fstat(fd, &st), 0);
        EXPECT_EQ(static_cast<size_t>(st.st_size), cfg.shm_size);

        // Verify header magic
        void* ptr = ::mmap(nullptr, sizeof(SharedMemoryHeader), PROT_READ, MAP_SHARED, fd, 0);
        ASSERT_NE(ptr, MAP_FAILED);
        auto* header = static_cast<SharedMemoryHeader*>(ptr);
        EXPECT_EQ(header->magic, 0x46524D45u);
        EXPECT_EQ(header->version, 1u);
        EXPECT_EQ(header->active_buffer, 0u);

        ::munmap(ptr, sizeof(SharedMemoryHeader));
        ::close(fd);
    }

    // After destructor, shm should be unlinked
    int fd = ::shm_open(cfg.shm_name.c_str(), O_RDONLY, 0);
    if (fd != -1) {
        ::close(fd);
        ::shm_unlink(cfg.shm_name.c_str());
    }
    EXPECT_EQ(fd, -1) << "Shared memory should be unlinked after destructor (RAII)";
}

// --- 2.5.2: Unix Socket binding and accepting connections ---
// **Validates: Requirements 1.2**

TEST(FrameExporterUnit, SocketCreatedAndAcceptsConnection) {
    auto cfg = make_test_config();
    FrameExporter exporter(cfg);

    // Verify socket file exists
    struct stat st{};
    ASSERT_EQ(::stat(cfg.socket_path.c_str(), &st), 0)
        << "Socket file should exist after construction";
    EXPECT_TRUE(S_ISSOCK(st.st_mode));

    // Connect a client
    int client_fd = connect_client(cfg.socket_path);
    ASSERT_NE(client_fd, -1) << "Client should be able to connect";

    // Call analyze until a frame is exported (handles sampling gate)
    analyze_until_exported(exporter);

    // We should receive a JSON notification
    std::string msg = read_json_message(client_fd);
    EXPECT_FALSE(msg.empty()) << "Should receive frame notification after analyze()";
    EXPECT_NE(msg.find("\"buffer_index\""), std::string::npos);
    EXPECT_NE(msg.find("\"shm_name\""), std::string::npos);

    ::close(client_fd);
}

// --- 2.5.3: No client → analyze() does not block ---
// **Validates: Requirements 1.5**

TEST(FrameExporterUnit, AnalyzeWithoutClientDoesNotBlock) {
    auto cfg = make_test_config();
    FrameExporter exporter(cfg);

    // Small sleep to ensure should_sample_frame() passes
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    // No client connected — analyze should return quickly
    FrameBuffer fb(make_data(256), make_info(1));

    auto start = std::chrono::steady_clock::now();
    auto result = exporter.analyze(fb);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(result.is_ok());
    // Should complete in well under 100ms (non-blocking)
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100);
    // When no client is connected and sampling passes, frames_skipped should increment
    EXPECT_GE(exporter.frames_skipped(), 1u);
}

TEST(FrameExporterUnit, MultipleAnalyzeWithoutClientAllNonBlocking) {
    auto cfg = make_test_config();
    FrameExporter exporter(cfg);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        // Small sleep to pass the sampling gate each time
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        FrameBuffer fb(make_data(128), make_info(static_cast<uint64_t>(i)));
        auto result = exporter.analyze(fb);
        EXPECT_TRUE(result.is_ok());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    // 20 calls should complete in well under 500ms total
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
    EXPECT_GE(exporter.frames_skipped(), 1u);
}

// --- 2.5.4: Python disconnect → auto wait for reconnect ---
// **Validates: Requirements 1.6**

TEST(FrameExporterUnit, ClientDisconnectAndReconnect) {
    auto cfg = make_test_config();
    FrameExporter exporter(cfg);

    // First client connects
    int client1 = connect_client(cfg.socket_path);
    ASSERT_NE(client1, -1);

    // Trigger accept + send
    analyze_until_exported(exporter, 256, 1);
    std::string msg1 = read_json_message(client1);
    EXPECT_FALSE(msg1.empty());

    // Disconnect first client
    ::close(client1);

    // Send a frame — this should detect the broken pipe and reset client_fd
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        FrameBuffer fb(make_data(256), make_info(2));
        exporter.analyze(fb);
    }

    // Now a second client connects
    int client2 = connect_client(cfg.socket_path);
    ASSERT_NE(client2, -1) << "Should be able to reconnect after first client disconnects";

    // Trigger accept of new client + send
    uint64_t before = exporter.frames_exported();
    for (int i = 0; i < 50; ++i) {
        FrameBuffer fb(make_data(256), make_info(3));
        exporter.analyze(fb);
        if (exporter.frames_exported() > before) break;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    std::string msg2 = read_json_message(client2);
    EXPECT_FALSE(msg2.empty()) << "Second client should receive notifications after reconnect";

    ::close(client2);
}

// --- 2.5.5: Socket cleaned up after destruction ---

TEST(FrameExporterUnit, SocketCleanedUpAfterDestruction) {
    auto cfg = make_test_config();

    {
        FrameExporter exporter(cfg);
        struct stat st{};
        ASSERT_EQ(::stat(cfg.socket_path.c_str(), &st), 0);
    }

    // After destructor, socket file should be removed
    struct stat st{};
    EXPECT_NE(::stat(cfg.socket_path.c_str(), &st), 0)
        << "Socket file should be removed after destructor";
}

// --- 2.5.6: ConfigDefaults ---

TEST(FrameExporterUnit, ConfigDefaults) {
    FrameExporterConfig config;
    EXPECT_EQ(config.shm_name, "/smart_camera_frames");
    EXPECT_EQ(config.shm_size, 20u * 1024 * 1024);
    EXPECT_EQ(config.socket_path, "/run/smart-camera/ai.sock");
    EXPECT_DOUBLE_EQ(config.target_fps, 2.0);
}

// --- 2.5.7: name() returns "frame_exporter" ---

TEST(FrameExporterUnit, NameReturnsFrameExporter) {
    auto cfg = make_test_config();
    FrameExporter exporter(cfg);
    EXPECT_EQ(exporter.name(), "frame_exporter");
}

// --- 2.5.8: frames_exported increments with client ---

TEST(FrameExporterUnit, FramesExportedIncrements) {
    auto cfg = make_test_config();
    FrameExporter exporter(cfg);

    int client_fd = connect_client(cfg.socket_path);
    ASSERT_NE(client_fd, -1);

    EXPECT_EQ(exporter.frames_exported(), 0u);

    // Send 3 frames, waiting for each to be exported
    for (uint64_t i = 1; i <= 3; ++i) {
        analyze_until_exported(exporter, 256, i);
        // Drain the notification so the socket buffer doesn't fill
        read_json_message(client_fd, 200);
    }

    EXPECT_EQ(exporter.frames_exported(), 3u);

    ::close(client_fd);
}

}  // namespace


// ============================================================
// Task 2.6: Property 2 — 帧率采样限制
// Feature: ai-video-summary, Property 2: 帧率采样限制
// **Validates: Requirements 1.4**
//
// *For any* configured target frame rate F and any frame sequence
// (with monotonically increasing timestamps), the number of frames
// passed by should_sample_frame() should not exceed
// `duration_seconds * F + 1` (allowing 1 frame tolerance).
// ============================================================

TEST(FrameExporterProperty, FrameRateSamplingLimit) {
    rc::check(
        "Feature: ai-video-summary, Property 2: 帧率采样限制 — "
        "For any target fps F and frame sequence, frames exported "
        "<= duration_seconds * F + 1",
        []() {
            // Generate a target fps in [1, 30]
            const auto fps = *rc::gen::inRange<int>(1, 31);
            // Generate number of frames to send in [5, 60]
            const auto num_frames = *rc::gen::inRange<int>(5, 61);
            // Generate inter-frame interval in microseconds [500, 20000] (0.5ms to 20ms)
            const auto interval_us = *rc::gen::inRange<int>(500, 20001);

            FrameExporterConfig cfg;
            cfg.shm_name = unique_shm_name();
            cfg.shm_size = 1 * 1024 * 1024;
            cfg.socket_path = unique_socket_path();
            cfg.target_fps = static_cast<double>(fps);

            FrameExporter exporter(cfg);

            // Connect a client so frames are exported (not skipped due to no client)
            int client_fd = connect_client(cfg.socket_path);
            RC_ASSERT(client_fd != -1);

            // Small sleep to let the sampling gate open for the first frame
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

            auto start_time = std::chrono::steady_clock::now();

            for (int i = 0; i < num_frames; ++i) {
                FrameBuffer fb(make_data(256), make_info(static_cast<uint64_t>(i)));
                exporter.analyze(fb);
                // Drain any notification to prevent socket buffer from filling
                read_json_message(client_fd, 1);
                std::this_thread::sleep_for(std::chrono::microseconds(interval_us));
            }

            auto end_time = std::chrono::steady_clock::now();
            double duration_seconds = std::chrono::duration<double>(end_time - start_time).count();

            uint64_t frames_passed = exporter.frames_exported();
            uint64_t max_allowed = static_cast<uint64_t>(duration_seconds * fps + 1);

            RC_ASSERT(frames_passed <= max_allowed);

            ::close(client_fd);
        });
}

// ============================================================
// Task 2.7: Property 4 — Ping-pong 缓冲区交替
// Feature: ai-video-summary, Property 4: Ping-pong 缓冲区交替
// **Validates: Requirements 1.7**
//
// *For any* sequence of N consecutive frames written to shared
// memory (N >= 2), the buffer_index used for frame i should equal
// `i % 2`, i.e., consecutive frames always alternate between
// buffer 0 and 1.
// ============================================================

TEST(FrameExporterProperty, PingPongBufferAlternation) {
    rc::check(
        "Feature: ai-video-summary, Property 4: Ping-pong 缓冲区交替 — "
        "For any N consecutive frames (N >= 2), buffer_index for frame i == i % 2",
        []() {
            // Generate number of frames in [2, 30]
            const auto num_frames = *rc::gen::inRange<int>(2, 31);

            FrameExporterConfig cfg;
            cfg.shm_name = unique_shm_name();
            cfg.shm_size = 1 * 1024 * 1024;
            cfg.socket_path = unique_socket_path();
            cfg.target_fps = 100000.0;  // Very high fps so no frames are skipped

            FrameExporter exporter(cfg);

            // Connect a client
            int client_fd = connect_client(cfg.socket_path);
            RC_ASSERT(client_fd != -1);

            // Collect buffer_index values from all exported frames
            std::vector<int> buffer_indices;

            for (int i = 0; i < num_frames; ++i) {
                // Small sleep to ensure sampling gate passes
                std::this_thread::sleep_for(std::chrono::microseconds(50));

                uint64_t before = exporter.frames_exported();
                FrameBuffer fb(make_data(256), make_info(static_cast<uint64_t>(i)));
                exporter.analyze(fb);

                if (exporter.frames_exported() > before) {
                    // Frame was exported — read the notification
                    std::string msg = read_json_message(client_fd, 500);
                    RC_ASSERT(!msg.empty());

                    // Parse buffer_index from JSON
                    auto pos = msg.find("\"buffer_index\":");
                    RC_ASSERT(pos != std::string::npos);
                    int idx = msg[pos + 15] - '0';  // single digit: 0 or 1
                    RC_ASSERT(idx == 0 || idx == 1);
                    buffer_indices.push_back(idx);
                }
            }

            // We need at least 2 exported frames to verify alternation
            RC_PRE(buffer_indices.size() >= 2u);

            // Verify: buffer_index for frame i == i % 2
            for (size_t i = 0; i < buffer_indices.size(); ++i) {
                int expected = static_cast<int>(i % 2);
                RC_ASSERT(buffer_indices[i] == expected);
            }

            ::close(client_fd);
        });
}
