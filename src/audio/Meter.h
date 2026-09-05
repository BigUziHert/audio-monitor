#pragma once
//
// Peak metering hand-off, audio thread -> UI thread.
//
// The audio thread only ever folds a new maximum into an atomic. It does no
// decay, no dB conversion and no timing: all of that is display policy and
// belongs on the UI timer, which polls these atomics at frame rate. Nothing
// here blocks or allocates.
//
#include <atomic>
#include <cmath>

namespace audiomon {

// Max-since-last-read. The audio thread folds in block peaks; the UI thread
// takes the value and resets it, so no peak between UI polls is ever missed.
class AtomicPeak {
public:
    void publish(float v) noexcept {
        float cur = value_.load(std::memory_order_relaxed);
        while (v > cur &&
               !value_.compare_exchange_weak(cur, v, std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {
            // cur is refreshed by compare_exchange_weak on failure
        }
    }

    // UI side: read and reset.
    float take() noexcept { return value_.exchange(0.0f, std::memory_order_relaxed); }

private:
    std::atomic<float> value_{0.0f};
};

struct StereoPeak {
    AtomicPeak l, r;
};

// ------------------------------------------------------------ UI helpers ---

inline constexpr float kMeterFloorDb = -60.0f;

inline float linearToDb(float v) noexcept {
    return v > 1.0e-6f ? 20.0f * std::log10(v) : kMeterFloorDb - 1.0f;
}

inline float dbToLinear(float db) noexcept {
    return std::pow(10.0f, db * 0.05f);
}

// Maps the meter's -60..0 dB display range linearly onto 0..1.
inline float dbToNorm(float db) noexcept {
    if (db <= kMeterFloorDb) return 0.0f;
    if (db >= 0.0f)          return 1.0f;
    return 1.0f - (db / kMeterFloorDb);
}

// Display-side ballistics: instant attack, slow release, with a separate hold.
// Lives on the UI thread so the audio thread never needs a clock.
class MeterBallistics {
public:
    // dtSeconds is real elapsed UI time, so behaviour is frame-rate independent.
    void update(float linearPeak, float dtSeconds) noexcept {
        const float db = linearToDb(linearPeak);

        if (db > level_) level_ = db;                                   // instant attack
        else             level_ -= kReleaseDbPerSec * dtSeconds;        // smooth release
        if (level_ < kMeterFloorDb) level_ = kMeterFloorDb;

        if (db >= hold_) {
            hold_     = db;
            holdTimer_ = kHoldSeconds;
        } else {
            holdTimer_ -= dtSeconds;
            if (holdTimer_ <= 0.0f) {
                hold_ -= kHoldFallDbPerSec * dtSeconds;
                if (hold_ < level_) hold_ = level_;
            }
        }
        if (hold_ < kMeterFloorDb) hold_ = kMeterFloorDb;

        if (linearPeak >= 1.0f) clipTimer_ = kClipSeconds;
        else if (clipTimer_ > 0.0f) clipTimer_ -= dtSeconds;
    }

    float levelDb() const noexcept { return level_; }
    float holdDb()  const noexcept { return hold_; }
    bool  clipped() const noexcept { return clipTimer_ > 0.0f; }
    void  clearClip() noexcept     { clipTimer_ = 0.0f; }

private:
    static constexpr float kReleaseDbPerSec  = 60.0f;   // roughly OBS's fall rate
    static constexpr float kHoldSeconds      = 1.0f;
    static constexpr float kHoldFallDbPerSec = 20.0f;
    static constexpr float kClipSeconds      = 1.5f;

    float level_     = kMeterFloorDb;
    float hold_      = kMeterFloorDb;
    float holdTimer_ = 0.0f;
    float clipTimer_ = 0.0f;
};

} // namespace audiomon
