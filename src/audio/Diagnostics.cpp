#include "audio/Diagnostics.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <windows.h>

#ifndef AUDIOMON_VERSION
#define AUDIOMON_VERSION "unknown"
#endif

namespace audiomon {

void DiagnosticHistory::beginSession(uint64_t id, std::string topology) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.size() == kMaxSessions) sessions_.pop_front();
    sessions_.push_back({id, std::move(topology), {}, 0});
}

void DiagnosticHistory::record(const DiagnosticSample& sample, std::string devices) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!devices.empty()) {
        for (auto& session : sessions_) {
            if (session.id != sample.session) continue;
            session.latestDevices = std::move(devices);
            session.latestDevicesElapsedMillis = sample.elapsedMillis;
            break;
        }
    }
    if (samples_.size() == kMaxSamples) {
        samples_.pop_front();
        ++discardedSamples_;
    }
    samples_.push_back(sample);
}

DiagnosticHistoryCopy DiagnosticHistory::copy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {{samples_.begin(), samples_.end()}, {sessions_.begin(), sessions_.end()},
            discardedSamples_};
}

QueueTrend diagnosticQueueTrend(const std::vector<DiagnosticSample>& samples,
                               bool output, size_t index) {
    QueueTrend trend;
    if (samples.empty()) return trend;
    auto newest = samples.rbegin();
    for (; newest != samples.rend(); ++newest) {
        if (newest->session != samples.back().session ||
            index >= (output ? newest->outputCount : newest->sourceCount)) return trend;
        const auto& candidate = output ? newest->outputs[index] : newest->sources[index];
        if (candidate.valid && !candidate.priming && candidate.flowing && candidate.queueRate) break;
    }
    if (newest == samples.rend()) return trend;
    const auto& last = *newest;
    const auto& anchor = output ? last.outputs[index] : last.sources[index];
    trend.lastElapsedMillis = last.elapsedMillis;
    double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0, sumPpm = 0;
    std::array<std::vector<double>, 5> minutes;
    trend.minimumMillis = 1e30;
    for (auto it = newest; it != samples.rend(); ++it) {
        if (it->session != last.session ||
            index >= (output ? it->outputCount : it->sourceCount)) break;
        const auto& s = output ? it->outputs[index] : it->sources[index];
        if (!s.valid || s.priming || !s.flowing || s.epoch != anchor.epoch ||
            s.queueRate != anchor.queueRate || s.nativeRate != anchor.nativeRate ||
            s.targetFrames != anchor.targetFrames ||
            s.starvationEvents != anchor.starvationEvents ||
            s.trimmedFrames != anchor.trimmedFrames || s.overflowFrames != anchor.overflowFrames ||
            (output && it->monitoringGeneration != last.monitoringGeneration)) break;
        const uint64_t ageMillis = last.elapsedMillis - it->elapsedMillis;
        if (ageMillis > 300000) break; // old healthy hours must not dilute a recent leak
        const double x = -double(ageMillis) / 60000.0;
        const double y = double(s.queueFrames) * 1000.0 / s.queueRate;
        if (!trend.samples) trend.lastMillis = y;
        trend.firstMillis = y;
        trend.durationSeconds = -x * 60.0;
        trend.minimumMillis = std::min(trend.minimumMillis, y);
        trend.maximumMillis = std::max(trend.maximumMillis, y);
        sumX += x; sumY += y; sumXX += x*x; sumXY += x*y;
        sumPpm += s.correctionPpm;
        minutes[std::min<size_t>(4, static_cast<size_t>(ageMillis / 60000))].push_back(y);
        ++trend.samples;
    }
    if (trend.samples) trend.meanCorrectionPpm = sumPpm / double(trend.samples);
    const double denominator = double(trend.samples) * sumXX - sumX * sumX;
    if (denominator > 1e-12)
        trend.slopeMillisPerMinute =
            (double(trend.samples) * sumXY - sumX * sumY) / denominator;
    // Minute medians reject packet-phase oscillation better than comparing two
    // samples. Require sustained increases over every sufficiently populated
    // minute in this recent segment, not a single partial phase ramp. A very
    // slow bounded phase drift can still mimic a leak; this is a qualified hint.
    std::vector<double> medians;
    for (auto it = minutes.rbegin(); it != minutes.rend(); ++it) {
        if (it->size() < 8) continue;
        std::sort(it->begin(), it->end());
        const size_t mid = it->size() / 2;
        medians.push_back(it->size() % 2 ? (*it)[mid] : ((*it)[mid-1] + (*it)[mid])*0.5);
    }
    bool persistent = medians.size() >= 3;
    for (size_t i = 1; i < medians.size(); ++i)
        persistent = persistent && medians[i] - medians[i-1] >= 0.5;
    trend.growing = persistent && trend.durationSeconds >= 180 &&
        medians.back() - medians.front() >= 5 && trend.slopeMillisPerMinute >= 1;
    return trend;
}

std::string formatDiagnosticReport(const DiagnosticSample& current,
                                  const DiagnosticHistoryCopy& history,
                                  const std::string& currentDevices,
                                  const std::string& recentLog) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(3);
    out << "Audio Monitor audio diagnostics v1\n"
        << "Application build: " << AUDIOMON_VERSION << "; process architecture: "
        << (sizeof(void*) == 8 ? "64-bit" : "32-bit") << "\n"
        << "Export UTC Unix milliseconds: " << current.utcMillis << "\n"
        << "Session: " << current.session << "; engine " << (current.running ? "running" : "stopped")
        << "; forwarding " << (current.monitoring ? "on" : "off") << "\n"
        << "Configured sources: " << current.sourceCount << "; outputs: " << current.outputCount << "\n"
        << "Engine-object uptime: " << current.elapsedMillis << " ms\n";
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    out << "Logical processors (current processor group): " << system.dwNumberOfProcessors << "\n";
    using VersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto getVersion = reinterpret_cast<VersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (getVersion && getVersion(&version) == 0)
        out << "Windows: " << version.dwMajorVersion << '.' << version.dwMinorVersion
            << " build " << version.dwBuildNumber << "\n";
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        GetSystemTimeAsFileTime(&now);
        const uint64_t nowTicks = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        const uint64_t createdTicks = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        out << "Process uptime (wall clock): " << (nowTicks >= createdTicks ? (nowTicks-createdTicks)/10000 : 0)
            << " ms\n";
    }
    out
        << "Canonical mix: 48000 Hz, stereo float, 480 frames / 10 ms pump\n"
        << "Source buffer preference: " << current.bufferMillis << " ms\n"
        << "Global missed pump periods (10 ms each, counted once): " << current.pumpMissedPeriods << "\n\n"
        << "INTERPRETATION\n"
        << "Queue milliseconds are inferred software queue timing, NOT measured end-to-end latency or A/V sync.\n"
        << "No audio/video timestamps are compared; driver/hardware, network and recording/video delays are excluded.\n"
        << "Queue depth is a consumer-published pre-consume observation, not a read of two live ring cursors.\n"
        << "Scalar fields are best-effort observations, not a sample-accurate transaction. Invalid/stopped rows\n"
        << "must not be interpreted as current delay; their counters/format can describe the last active stream.\n"
        << "Positive correction ppm consumes the input queue faster; negative consumes it slower.\n"
        << "Source overflow/trim units: source-native frames; source shortfall: canonical 48 kHz frames.\n"
        << "Source shortfalls are counted only after priming while capture reports flowing; an endpoint\n"
        << "falling silent can still report flowing briefly, so these are not necessarily audible glitches.\n"
        << "Shortfall counters exclude timeline-recovery fades, discarded queues, and priming silence.\n"
        << "Zero output underruns do not rule out source gaps: the mixer can deliver silence on time.\n"
        << "Output overflow/trim: canonical 48 kHz frames; output shortfall: output-native frames.\n"
        << "Underruns and starvation counts are events, not frames. Pump misses are global, not per output.\n"
        << "A capture epoch is a timeline break, not a device restart count.\n"
        << "Capture epochs advance on start requests, WASAPI DATA_DISCONTINUITY packets, and ring overflow drops.\n"
        << "Capture start requests include the initial start and failed opens; they are not successful reopen counts.\n"
        << "Initial discontinuities flag the first nonempty packet after each start; later flagged packets are counted separately.\n"
        << "Capture overflow frame counters reset on capture start, not on packet discontinuities; other counters reset at engine start.\n"
        << "Capture timeline event logs are coalesced by the supervisor; their timestamps are observation times.\n"
        << "History survives engine stop/restart in this process only; no audio samples are recorded.\n"
        << "Retained history: " << history.samples.size() << " / " << DiagnosticHistory::kMaxSamples
        << " samples (nominal 5 s cadence, about 2 h; lifecycle boundaries also sampled); discarded "
        << history.discardedSamples << ". Topology descriptions: last 32 sessions.\n\n"
        << "CURRENT DEVICE DETAILS (may include device IDs and application paths)\n" << currentDevices << "\n"
        << "SESSION TOPOLOGIES\n";
    for (const auto& session : history.sessions) {
        out << "Session " << session.id << " (initial configured selections):\n" << session.topology << "\n";
        if (!session.latestDevices.empty())
            out << "Session " << session.id << " last sampled runtime details at engine uptime "
                << session.latestDevicesElapsedMillis << " ms (retained before stop cleared stream identity):\n"
                << session.latestDevices << "\n";
    }
    out << "\nCAPTURE TIMELINE CAUSES (counts since engine start; retained after stop)\n";
    for (size_t i = 0; i < current.sourceCount; ++i) {
        const auto& s = current.sources[i];
        out << "Source " << i << ": start requests=" << s.captureStartRequests
            << "; initial packet discontinuities=" << s.captureInitialDiscontinuities
            << "; later packet discontinuities=" << s.captureDiscontinuities
            << "; ring overflow breaks=" << s.captureOverflowEvents << "\n";
    }
    out << "\nQUEUE TRENDS (last active segment, most recent 5 min maximum; retained after pause/stop)\n"
        << "Possible growth requires >=3 min and persistent increases in minute medians. Packet phase\n"
        << "or scheduling jitter can still mimic a leak: this is NOT confirmed clock drift or A/V desync.\n";
    for (bool output : {false, true}) {
        const size_t count = output ? current.outputCount : current.sourceCount;
        for (size_t i = 0; i < count; ++i) {
            const auto trend = diagnosticQueueTrend(history.samples, output, i);
            out << (output ? "Output " : "Source ") << i << ": ";
            if (history.samples.empty() || history.samples.back().session != current.session ||
                trend.samples < 2) {
                out << "insufficient continuous active history\n";
                continue;
            }
            out << "last active observation at engine uptime " << trend.lastElapsedMillis << " ms; "
                << trend.samples << " samples / " << trend.durationSeconds << " s; queue "
                << trend.firstMillis << " -> " << trend.lastMillis << " ms, range "
                << trend.minimumMillis << ".." << trend.maximumMillis << " ms; slope "
                << trend.slopeMillisPerMinute << " ms/min; mean correction "
                << trend.meanCorrectionPpm << " ppm; "
                << (trend.growing ? "POSSIBLE recent software queue growth" : "no sustained growth flag") << "\n";
        }
    }
    const auto rows = [&](const DiagnosticSample& sample, const char* kind) {
        for (bool output : {false, true}) {
            const size_t count = output ? sample.outputCount : sample.sourceCount;
            for (size_t i = 0; i < count; ++i) {
                const auto& s = output ? sample.outputs[i] : sample.sources[i];
                out << kind << ',' << sample.session << ',' << sample.elapsedMillis << ',' << sample.utcMillis
                    << ',' << sample.monitoringGeneration << ',' << sample.running << ',' << sample.monitoring
                    << ',' << sample.pumpMissedPeriods << ',' << (output ? "output" : "source") << ',' << i
                    << ',' << streamStateName(s.state) << ',' << s.valid << ',' << s.flowing << ',' << s.priming
                    << ',' << s.muted << ',' << s.exclusive << ',' << s.epoch << ',' << s.nativeRate << ',' << s.queueRate
                    << ',' << s.queueFrames << ',' << s.targetFrames << ',' << s.capacityFrames << ',' << s.blockFrames
                    << ',' << (s.queueRate ? double(s.queueFrames)*1000.0/s.queueRate : 0.0)
                    << ',' << s.correctionPpm << ',' << s.overflowFrames << ',' << s.trimmedFrames
                    << ',' << s.starvedFrames << ',' << s.starvationEvents << ',' << s.underrunEvents
                    << ',' << s.latencyCorrections << ',' << s.captureStartRequests
                    << ',' << s.captureInitialDiscontinuities << ',' << s.captureDiscontinuities
                    << ',' << s.captureOverflowEvents << '\n';
            }
        }
    };
    out << "\nNUMERIC HISTORY CSV\n"
        << "kind,session,elapsed_ms,utc_unix_ms,forwarding_generation,engine_running,forwarding,pump_misses,type,index,state,valid,flowing,priming,muted,exclusive,epoch,native_hz,queue_hz,queue_frames,target_frames,capacity_frames,max_callback_frames,queue_ms,correction_ppm,overflow_frames,trimmed_frames,shortfall_frames,shortfall_events,render_underrun_events,latency_correction_events,capture_start_requests,capture_initial_discontinuities,capture_discontinuities,capture_overflow_events\n";
    for (const auto& sample : history.samples) rows(sample, "history");
    rows(current, "current");
    out << "\nRECENT RUNTIME LOG (bounded in-memory tail; local wall-clock timestamps)\n" << recentLog;
    return out.str();
}

} // namespace audiomon
