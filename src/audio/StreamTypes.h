#pragma once
//
// Shared vocabulary for the capture and render streams.
//
#include <cstdint>
#include <string>

namespace audiomon {

// Copied under one info lock. Setup/restart may still be in progress; callers
// must qualify the identity/format with stream state and timeline generation.
struct StreamDiagnosticInfo {
    std::wstring name;
    std::wstring id;
    std::string error;
    std::string format;
};

enum class StreamState : int {
    Stopped = 0,
    Opening,      // worker is resolving/initialising the device
    Running,      // device open and healthy
    Failed,       // device gone or unopenable; the supervisor will retry
};

inline const char* streamStateName(StreamState s) {
    switch (s) {
        case StreamState::Stopped: return "stopped";
        case StreamState::Opening: return "opening";
        case StreamState::Running: return "running";
        case StreamState::Failed:  return "failed";
    }
    return "?";
}

} // namespace audiomon
