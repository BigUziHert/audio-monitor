#pragma once

// Numeric history is sampled by the supervisor, never by an audio callback.
// At most 1440 samples and 32 topology descriptions are retained in memory.
#include "audio/StreamTypes.h"
#include "config/Config.h"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace audiomon {

struct DiagnosticStream {
    StreamState state = StreamState::Stopped;
    bool valid = false; // queue belongs to the active, acknowledged timeline
    bool flowing = false;
    bool priming = true;
    bool muted = false;
    bool exclusive = false;
    uint32_t epoch = 0;
    uint32_t nativeRate = 0;
    uint32_t queueRate = 0;
    uint32_t queueFrames = 0;
    uint32_t targetFrames = 0;
    uint32_t capacityFrames = 0;
    uint32_t blockFrames = 0;
    double correctionPpm = 0;
    uint64_t overflowFrames = 0;
    uint64_t trimmedFrames = 0;
    uint64_t starvedFrames = 0;
    uint64_t starvationEvents = 0;
    uint64_t underrunEvents = 0;
    uint64_t latencyCorrections = 0;
};

struct DiagnosticSample {
    uint64_t session = 0;
    uint64_t elapsedMillis = 0; // monotonic since engine construction
    uint64_t utcMillis = 0;     // Unix time, UTC
    uint64_t monitoringGeneration = 0;
    uint64_t pumpMissedPeriods = 0;
    bool running = false;
    bool monitoring = false;
    uint32_t bufferMillis = 0;
    size_t sourceCount = 0;
    size_t outputCount = 0;
    std::array<DiagnosticStream, kMaxSources> sources{};
    std::array<DiagnosticStream, kMaxOutputs> outputs{};
};

struct DiagnosticSession {
    uint64_t id = 0;
    std::string topology;
    std::string latestDevices;
    uint64_t latestDevicesElapsedMillis = 0;
};

struct DiagnosticHistoryCopy {
    std::vector<DiagnosticSample> samples;
    std::vector<DiagnosticSession> sessions;
    uint64_t discardedSamples = 0;
};

class DiagnosticHistory {
public:
    static constexpr uint64_t kSampleMillis = 5000;
    static constexpr size_t kMaxSamples = 1440;
    static constexpr size_t kMaxSessions = 32;
    void beginSession(uint64_t id, std::string topology);
    void record(const DiagnosticSample& sample, std::string devices = {});
    DiagnosticHistoryCopy copy() const;
private:
    mutable std::mutex mutex_;
    std::deque<DiagnosticSample> samples_;
    std::deque<DiagnosticSession> sessions_;
    uint64_t discardedSamples_ = 0;
};

struct QueueTrend {
    size_t samples = 0;
    double durationSeconds = 0;
    double firstMillis = 0;
    double lastMillis = 0;
    double minimumMillis = 0;
    double maximumMillis = 0;
    double slopeMillisPerMinute = 0;
    double meanCorrectionPpm = 0;
    bool growing = false;
    uint64_t lastElapsedMillis = 0;
};

// Only the newest uninterrupted active segment of the newest session is
// compared (retained even after pause/stop): a changed epoch,
// forwarding generation, target, format, stop, idle or prime breaks the trend.
QueueTrend diagnosticQueueTrend(const std::vector<DiagnosticSample>& samples,
                               bool output, size_t index);
std::string formatDiagnosticReport(const DiagnosticSample& current,
                                  const DiagnosticHistoryCopy& history,
                                  const std::string& currentDevices,
                                  const std::string& recentLog);

} // namespace audiomon
