#include "ai/frame_exporter.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

// POSIX IPC headers
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// MSG_NOSIGNAL is Linux-only; macOS uses SO_NOSIGPIPE instead
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace sc {

// ============================================================
// Construction / Destruction
// ============================================================

FrameExporter::FrameExporter(const FrameExporterConfig& config)
    : config_(config),
      last_sample_time_(std::chrono::steady_clock::now()) {
    auto shm_res = init_shared_memory();
    if (shm_res.is_err()) {
        std::cerr << "[FrameExporter] shared memory init failed: "
                  << shm_res.error().message << std::endl;
    }

    auto sock_res = init_socket();
    if (sock_res.is_err()) {
        std::cerr << "[FrameExporter] socket init failed: "
                  << sock_res.error().message << std::endl;
    }
}

FrameExporter::~FrameExporter() {
    cleanup_shared_memory();
    cleanup_socket();
}

// ============================================================
// IAIModel interface
// ============================================================

std::string FrameExporter::name() const {
    return "frame_exporter";
}

Result<std::vector<AnalysisResult>> FrameExporter::analyze(const FrameBuffer& frame) {
    // Empty result — FrameExporter doesn't produce detections itself
    std::vector<AnalysisResult> empty;

    // 1. Frame rate sampling
    if (!should_sample_frame()) {
        return Result<std::vector<AnalysisResult>>::Ok(std::move(empty));
    }

    // Update last sample time
    last_sample_time_ = std::chrono::steady_clock::now();

    // 2. Check shared memory is valid
    if (shm_ptr_ == nullptr || shm_ptr_ == MAP_FAILED) {
        ++frames_skipped_;
        return Result<std::vector<AnalysisResult>>::Ok(std::move(empty));
    }

    // 3. Try to accept a client if none connected
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (client_fd_ == -1 && server_fd_ != -1) {
            // Non-blocking accept
            int fd = ::accept(server_fd_, nullptr, nullptr);
            if (fd >= 0) {
                client_fd_ = fd;
                std::cerr << "[FrameExporter] Python client connected (fd="
                          << fd << ")" << std::endl;
            }
            // If accept returns -1 with EAGAIN/EWOULDBLOCK, no client yet — that's fine
        }

        // 4. No client connected — skip frame
        if (client_fd_ == -1) {
            ++frames_skipped_;
            return Result<std::vector<AnalysisResult>>::Ok(std::move(empty));
        }
    }

    // 5. Calculate buffer layout
    auto* header = static_cast<SharedMemoryHeader*>(shm_ptr_);
    uint32_t buffer_size = header->buffer_size;
    if (buffer_size == 0) {
        ++frames_skipped_;
        return Result<std::vector<AnalysisResult>>::Ok(std::move(empty));
    }

    size_t frame_data_size = frame.size();
    if (frame_data_size == 0 || frame_data_size > buffer_size) {
        ++frames_skipped_;
        return Result<std::vector<AnalysisResult>>::Ok(std::move(empty));
    }

    // 6. Copy frame data to current ping-pong buffer
    size_t offset = sizeof(SharedMemoryHeader) + static_cast<size_t>(current_buffer_) * buffer_size;
    auto* dest = static_cast<uint8_t*>(shm_ptr_) + offset;
    std::memcpy(dest, frame.data(), frame_data_size);

    // 7. Update header active_buffer
    header->active_buffer = current_buffer_;

    // 8. Build and send frame notification
    FrameNotification notification;
    notification.shm_name = config_.shm_name;
    notification.buffer_index = current_buffer_;
    notification.offset = offset;
    notification.size = frame_data_size;
    notification.width = frame.info().width;
    notification.height = frame.info().height;
    notification.pixel_format = "BGR";  // AI appsink outputs BGR via videoconvert

    // Convert steady_clock timestamp to Unix milliseconds
    auto now_system = std::chrono::system_clock::now();
    notification.timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now_system.time_since_epoch())
            .count());
    notification.sequence = frame.info().sequence_number;

    auto send_res = send_frame_notification(notification);
    if (send_res.is_err()) {
        // Client disconnected — will retry accept on next call
        ++frames_skipped_;
        return Result<std::vector<AnalysisResult>>::Ok(std::move(empty));
    }

    // 9. Toggle ping-pong buffer
    current_buffer_ = (current_buffer_ == 0) ? 1 : 0;

    // 10. Increment exported counter
    ++frames_exported_;

    return Result<std::vector<AnalysisResult>>::Ok(std::move(empty));
}

// ============================================================
// Stats accessors
// ============================================================

uint64_t FrameExporter::frames_skipped() const {
    return frames_skipped_;
}

uint64_t FrameExporter::frames_exported() const {
    return frames_exported_;
}

// ============================================================
// Shared memory management
// ============================================================

VoidResult FrameExporter::init_shared_memory() {
    // Remove any stale shm segment
    ::shm_unlink(config_.shm_name.c_str());

    shm_fd_ = ::shm_open(config_.shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd_ == -1) {
        return ErrVoid(ErrorCode::AIModelLoadFailed,
                       "shm_open failed: " + std::string(std::strerror(errno)),
                       "FrameExporter");
    }

    // Set total size: header + 2 buffers
    size_t total_size = config_.shm_size;
    if (::ftruncate(shm_fd_, static_cast<off_t>(total_size)) == -1) {
        ::close(shm_fd_);
        ::shm_unlink(config_.shm_name.c_str());
        shm_fd_ = -1;
        return ErrVoid(ErrorCode::AIModelLoadFailed,
                       "ftruncate failed: " + std::string(std::strerror(errno)),
                       "FrameExporter");
    }

    shm_ptr_ = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (shm_ptr_ == MAP_FAILED) {
        ::close(shm_fd_);
        ::shm_unlink(config_.shm_name.c_str());
        shm_fd_ = -1;
        shm_ptr_ = nullptr;
        return ErrVoid(ErrorCode::AIModelLoadFailed,
                       "mmap failed: " + std::string(std::strerror(errno)),
                       "FrameExporter");
    }

    // Initialize header
    auto* header = static_cast<SharedMemoryHeader*>(shm_ptr_);
    header->magic = 0x46524D45;  // "FRME"
    header->version = 1;
    // Each buffer gets half of the remaining space after the header
    uint32_t buffer_size = static_cast<uint32_t>((total_size - sizeof(SharedMemoryHeader)) / 2);
    header->buffer_size = buffer_size;
    header->active_buffer = 0;
    std::memset(header->padding, 0, sizeof(header->padding));

    std::cerr << "[FrameExporter] shared memory created: " << config_.shm_name
              << " (" << total_size << " bytes, buffer_size=" << buffer_size << ")"
              << std::endl;

    return OkVoid();
}

void FrameExporter::cleanup_shared_memory() {
    if (shm_ptr_ != nullptr && shm_ptr_ != MAP_FAILED) {
        ::munmap(shm_ptr_, config_.shm_size);
        shm_ptr_ = nullptr;
    }
    if (shm_fd_ != -1) {
        ::close(shm_fd_);
        shm_fd_ = -1;
    }
    ::shm_unlink(config_.shm_name.c_str());
}

// ============================================================
// Unix Socket management
// ============================================================

VoidResult FrameExporter::init_socket() {
    // Remove any stale socket file
    ::unlink(config_.socket_path.c_str());

    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ == -1) {
        return ErrVoid(ErrorCode::AIModelLoadFailed,
                       "socket() failed: " + std::string(std::strerror(errno)),
                       "FrameExporter");
    }

    // Set non-blocking mode on server socket
    int flags = ::fcntl(server_fd_, F_GETFL, 0);
    if (flags == -1 || ::fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
        ::close(server_fd_);
        server_fd_ = -1;
        return ErrVoid(ErrorCode::AIModelLoadFailed,
                       "fcntl(O_NONBLOCK) failed: " + std::string(std::strerror(errno)),
                       "FrameExporter");
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, config_.socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
        ::close(server_fd_);
        server_fd_ = -1;
        return ErrVoid(ErrorCode::AIModelLoadFailed,
                       "bind() failed: " + std::string(std::strerror(errno)),
                       "FrameExporter");
    }

    if (::listen(server_fd_, 1) == -1) {
        ::close(server_fd_);
        ::unlink(config_.socket_path.c_str());
        server_fd_ = -1;
        return ErrVoid(ErrorCode::AIModelLoadFailed,
                       "listen() failed: " + std::string(std::strerror(errno)),
                       "FrameExporter");
    }

    std::cerr << "[FrameExporter] Unix socket listening: " << config_.socket_path << std::endl;

    return OkVoid();
}

void FrameExporter::cleanup_socket() {
    if (client_fd_ != -1) {
        ::close(client_fd_);
        client_fd_ = -1;
    }
    if (server_fd_ != -1) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
    ::unlink(config_.socket_path.c_str());
}

VoidResult FrameExporter::send_frame_notification(const FrameNotification& msg) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    if (client_fd_ == -1) {
        return ErrVoid(ErrorCode::EndpointUnreachable,
                       "No client connected", "FrameExporter");
    }

    // Build JSON manually (no external JSON library dependency)
    std::ostringstream json;
    json << "{"
         << "\"shm_name\":\"" << msg.shm_name << "\","
         << "\"buffer_index\":" << static_cast<int>(msg.buffer_index) << ","
         << "\"offset\":" << msg.offset << ","
         << "\"size\":" << msg.size << ","
         << "\"width\":" << msg.width << ","
         << "\"height\":" << msg.height << ","
         << "\"pixel_format\":\"" << msg.pixel_format << "\","
         << "\"timestamp_ms\":" << msg.timestamp_ms << ","
         << "\"sequence\":" << msg.sequence
         << "}\n";

    std::string data = json.str();
    ssize_t sent = ::send(client_fd_, data.c_str(), data.size(), MSG_NOSIGNAL);

    if (sent == -1) {
        // Client disconnected (EPIPE, ECONNRESET, etc.)
        std::cerr << "[FrameExporter] Python client disconnected: "
                  << std::strerror(errno) << std::endl;
        ::close(client_fd_);
        client_fd_ = -1;
        return ErrVoid(ErrorCode::EndpointUnreachable,
                       "send() failed: " + std::string(std::strerror(errno)),
                       "FrameExporter");
    }

    return OkVoid();
}

// ============================================================
// Frame rate control
// ============================================================

bool FrameExporter::should_sample_frame() const {
    if (config_.target_fps <= 0.0) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_sample_time_).count();
    double interval = 1.0 / config_.target_fps;

    return elapsed >= interval;
}

}  // namespace sc
