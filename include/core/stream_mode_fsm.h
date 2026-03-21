#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "core/stream_mode.h"

namespace sc {

// ============================================================
// StreamModeFSM — state machine for stream mode transitions
// based on endpoint availability (KVS + WebRTC).
//
// States: FULL, KVS_ONLY, WEBRTC_ONLY, DEGRADED
// Transitions driven by evaluate(kvs_available, webrtc_available).
// ============================================================

struct ModeTransition {
    StreamMode old_mode;
    StreamMode new_mode;
    std::string reason;
    std::chrono::steady_clock::time_point timestamp;
};

using ModeCallback = std::function<void(StreamMode)>;

class StreamModeFSM {
public:
    StreamModeFSM();
    explicit StreamModeFSM(StreamMode initial_mode);

    /// Evaluate current endpoint availability and return a new mode
    /// if a transition should occur, or std::nullopt if no change.
    std::optional<StreamMode> evaluate(bool kvs_available, bool webrtc_available);

    /// Current mode (thread-safe read).
    StreamMode current_mode() const;

    /// Register a callback invoked on every mode transition.
    void on_mode_change(ModeCallback cb);

    /// Last transition info (empty if no transition has occurred).
    std::optional<ModeTransition> last_transition() const;

private:
    StreamMode determine_mode(bool kvs_available, bool webrtc_available) const;
    void notify(StreamMode new_mode);

    mutable std::mutex mutex_;
    StreamMode mode_;
    ModeCallback callback_;
    std::optional<ModeTransition> last_transition_;
};

}  // namespace sc
