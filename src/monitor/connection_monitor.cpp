#include "monitor/connection_monitor.h"

#include <chrono>
#include <iostream>
#include <sstream>

#ifdef CURL_FOUND
#include <curl/curl.h>
#endif

namespace sc {

// ============================================================
// Construction / Destruction
// ============================================================

ConnectionMonitor::ConnectionMonitor(
    uint32_t failure_threshold,
    uint32_t success_threshold,
    std::chrono::seconds check_interval,
    std::chrono::seconds request_timeout)
    : failure_threshold_(failure_threshold)
    , success_threshold_(success_threshold)
    , check_interval_(check_interval)
    , request_timeout_(request_timeout)
    , fsm_(StreamMode::DEGRADED) {}

ConnectionMonitor::~ConnectionMonitor() {
    if (running_.load()) {
        stop();
    }
}

// ============================================================
// start / stop
// ============================================================

VoidResult ConnectionMonitor::start(const std::string& kvs_endpoint,
                                    const std::string& webrtc_endpoint) {
    if (running_.load()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "ConnectionMonitor already running",
                       "ConnectionMonitor::start");
    }

    {
        std::unique_lock<std::shared_mutex> lock(status_mutex_);
        kvs_status_ = EndpointStatus{};
        kvs_status_.url = kvs_endpoint;
        webrtc_status_ = EndpointStatus{};
        webrtc_status_.url = webrtc_endpoint;
    }

    // Wire FSM callback to forward mode changes
    fsm_.on_mode_change([this](StreamMode mode) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (mode_callback_) {
            mode_callback_(mode);
        }
    });

    running_.store(true);
    monitor_thread_ = std::thread(&ConnectionMonitor::monitor_loop, this);

    return OkVoid();
}

VoidResult ConnectionMonitor::stop() {
    if (!running_.load()) {
        return OkVoid();
    }
    running_.store(false);
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    return OkVoid();
}

// ============================================================
// Status accessors
// ============================================================

EndpointStatus ConnectionMonitor::kvs_status() const {
    std::shared_lock<std::shared_mutex> lock(status_mutex_);
    return kvs_status_;
}

EndpointStatus ConnectionMonitor::webrtc_status() const {
    std::shared_lock<std::shared_mutex> lock(status_mutex_);
    return webrtc_status_;
}

void ConnectionMonitor::on_mode_change(ModeCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    mode_callback_ = std::move(cb);
}

// ============================================================
// Monitor loop
// ============================================================

void ConnectionMonitor::monitor_loop() {
    while (running_.load()) {
        check_once();

        // Sleep in small increments so we can exit promptly on stop()
        auto deadline = std::chrono::steady_clock::now() + check_interval_;
        while (running_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

void ConnectionMonitor::check_once() {
    double kvs_time = 0.0;
    double webrtc_time = 0.0;

    std::string kvs_url;
    std::string webrtc_url;
    {
        std::shared_lock<std::shared_mutex> lock(status_mutex_);
        kvs_url = kvs_status_.url;
        webrtc_url = webrtc_status_.url;
    }

    bool kvs_ok = check_endpoint(kvs_url, kvs_time);
    bool webrtc_ok = check_endpoint(webrtc_url, webrtc_time);

    {
        std::unique_lock<std::shared_mutex> lock(status_mutex_);
        update_status(kvs_status_, kvs_ok, kvs_time);
        update_status(webrtc_status_, webrtc_ok, webrtc_time);
    }

    // Log each check result
    auto log_check = [](const std::string& label, const std::string& url,
                        bool ok, double time_ms) {
        std::ostringstream msg;
        msg << "[ConnectionMonitor] " << label
            << " endpoint=" << url
            << " reachable=" << (ok ? "true" : "false")
            << " response_time_ms=" << time_ms;
        std::cerr << msg.str() << std::endl;
    };
    log_check("KVS", kvs_url, kvs_ok, kvs_time);
    log_check("WebRTC", webrtc_url, webrtc_ok, webrtc_time);

    evaluate_and_notify();
}

// ============================================================
// Endpoint health check (CURL or stub)
// ============================================================

bool ConnectionMonitor::check_endpoint(const std::string& url,
                                       double& response_time_ms) {
    if (url.empty()) {
        response_time_ms = 0.0;
        return false;
    }

#ifdef CURL_FOUND
    CURL* curl = curl_easy_init();
    if (!curl) {
        response_time_ms = 0.0;
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);           // HEAD request
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(request_timeout_.count()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(request_timeout_.count()));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // Suppress response body (there shouldn't be one for HEAD, but be safe)
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     +[](char*, size_t size, size_t nmemb, void*) -> size_t {
                         return size * nmemb;
                     });

    auto start = std::chrono::steady_clock::now();
    CURLcode res = curl_easy_perform(curl);
    auto end = std::chrono::steady_clock::now();

    response_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    bool reachable = false;
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        // Any 2xx or 3xx or even 403 (endpoint exists but auth required) counts as reachable
        reachable = (http_code > 0 && http_code < 500);
    }

    curl_easy_cleanup(curl);
    return reachable;
#else
    // Stub: always report endpoint as reachable when CURL is not available
    response_time_ms = 1.0;
    return true;
#endif
}

// ============================================================
// Status update with consecutive counter logic
// ============================================================

void ConnectionMonitor::update_status(EndpointStatus& status,
                                      bool reachable,
                                      double response_time_ms) {
    status.reachable = reachable;
    status.response_time_ms = response_time_ms;
    status.check_time = std::chrono::steady_clock::now();

    if (reachable) {
        status.consecutive_failures = 0;
        status.consecutive_successes++;
    } else {
        status.consecutive_successes = 0;
        status.consecutive_failures++;
    }
}

// ============================================================
// Evaluate endpoint states and drive FSM
// ============================================================

void ConnectionMonitor::evaluate_and_notify() {
    bool kvs_available = false;
    bool webrtc_available = false;

    {
        std::shared_lock<std::shared_mutex> lock(status_mutex_);

        // Endpoint is considered "available" if it's currently reachable
        // AND has not hit the failure threshold, OR has met the recovery
        // threshold after being offline.
        //
        // Simplified logic:
        //   - offline when consecutive_failures >= failure_threshold_
        //   - online when consecutive_successes >= success_threshold_
        //     OR was never marked offline (consecutive_failures < failure_threshold_)

        kvs_available = (kvs_status_.consecutive_failures < failure_threshold_)
                        || (kvs_status_.consecutive_successes >= success_threshold_);

        webrtc_available = (webrtc_status_.consecutive_failures < failure_threshold_)
                           || (webrtc_status_.consecutive_successes >= success_threshold_);
    }

    // FSM evaluate — returns new mode only on transition
    fsm_.evaluate(kvs_available, webrtc_available);
}

}  // namespace sc
