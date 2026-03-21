#include "core/stream_mode_fsm.h"

#include <iostream>
#include <sstream>

namespace sc {

StreamModeFSM::StreamModeFSM()
    : mode_(StreamMode::DEGRADED) {}

StreamModeFSM::StreamModeFSM(StreamMode initial_mode)
    : mode_(initial_mode) {}

std::optional<StreamMode> StreamModeFSM::evaluate(bool kvs_available, bool webrtc_available) {
    std::lock_guard<std::mutex> lock(mutex_);

    StreamMode desired = determine_mode(kvs_available, webrtc_available);
    if (desired == mode_) {
        return std::nullopt;  // no transition
    }

    // Build reason string
    std::ostringstream reason;
    reason << "kvs=" << (kvs_available ? "online" : "offline")
           << ", webrtc=" << (webrtc_available ? "online" : "offline");

    StreamMode old_mode = mode_;
    mode_ = desired;

    ModeTransition transition{
        old_mode,
        desired,
        reason.str(),
        std::chrono::steady_clock::now()
    };
    last_transition_ = transition;

    // Log the transition
    std::ostringstream log_msg;
    log_msg << "[StreamModeFSM] Mode transition: "
            << stream_mode_to_string(old_mode) << " -> "
            << stream_mode_to_string(desired)
            << " (reason: " << transition.reason << ")";
    std::cerr << log_msg.str() << std::endl;

    // Notify callback (still under lock — callback should be lightweight)
    if (callback_) {
        callback_(desired);
    }

    return desired;
}

StreamMode StreamModeFSM::current_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_;
}

void StreamModeFSM::on_mode_change(ModeCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(cb);
}

std::optional<ModeTransition> StreamModeFSM::last_transition() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_transition_;
}

StreamMode StreamModeFSM::determine_mode(bool kvs_available, bool webrtc_available) const {
    if (kvs_available && webrtc_available) {
        return StreamMode::FULL;
    } else if (kvs_available && !webrtc_available) {
        return StreamMode::KVS_ONLY;
    } else if (!kvs_available && webrtc_available) {
        return StreamMode::WEBRTC_ONLY;
    } else {
        return StreamMode::DEGRADED;
    }
}

}  // namespace sc
