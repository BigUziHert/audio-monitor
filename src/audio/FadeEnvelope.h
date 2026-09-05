#pragma once
//
// Per-block presence envelope: the thing that stops a silent gap from
// clicking.
//
// Extracted from the mixer so it can be tested on any platform. The subtlety
// worth isolating is that a fade-OUT has to land exactly where the audio
// stops. The resampler reports it filled `made` of `frames`; if the envelope
// merely aims at zero for the whole block, it is still near 1.0 when the
// samples run out and the cut is as abrupt as no fade at all.
//
#include <algorithm>
#include <cstdint>

namespace audiomon {

class FadeEnvelope {
public:
    // fadeFrames is the length of a full fade, in frames.
    void configure(uint32_t fadeFrames) noexcept {
        fade_ = std::max<uint32_t>(1, fadeFrames);
    }

    void reset(float value = 0.0f) noexcept { value_ = value; }
    float value() const noexcept { return value_; }
    uint32_t fadeFrames() const noexcept { return fade_; }
    uint32_t framesUntilSilent() const noexcept {
        if (value_ <= 0.0f) return 0;
        return std::min(fade_, static_cast<uint32_t>(value_ * float(fade_) + 0.999999f));
    }

    // Call once per block before iterating frames. `made` is how many frames
    // of real audio the block contains; `frames` is the block length. A planned
    // stop uses forceFadeOut when those frames fill the whole render block.
    void beginBlock(uint32_t made, uint32_t frames, bool forceFadeOut = false) noexcept {
        made_    = made;
        forced_  = forceFadeOut && made > 0;
        starved_ = forced_ || (made < frames);
        rampLen_ = starved_ ? std::min(made, fade_) : 0;
        start_   = forced_ ? 0 : (starved_ ? (made - rampLen_) : frames);
        rise_    = 1.0f / float(fade_);
        // A deliberate stop may span several short render periods. Keep the
        // configured slope instead of compressing the whole fade into this
        // block; the mixer will provide more stale audio on the next block.
        fall_    = 1.0f / float(fade_);
        // With fewer than one fade's worth of real samples there is no way to
        // descend from unity without a large per-frame step. Cap the starting
        // level so the remaining samples still taper at the configured rate.
        if (!forced_ && starved_ && made < fade_)
            value_ = std::min(value_, float(made) / float(fade_));
    }

    // Advance one frame and return the amplitude to apply.
    float next(uint32_t f) noexcept {
        float target = 1.0f;
        if (starved_) {
            if (f >= made_)        target = 0.0f;
            else if (forced_)      target = 0.0f;
            else if (f >= start_)  target = float(made_ - f) / float(fade_);
        }
        if (value_ < target) {
            value_ = std::min(target, value_ + rise_);
        } else if (value_ - target <= fall_ + 1.0e-6f) {
            value_ = target;
        } else {
            value_ -= fall_;
        }
        return value_;
    }

private:
    uint32_t fade_    = 240;
    uint32_t made_    = 0;
    uint32_t rampLen_ = 0;
    uint32_t start_   = 0;
    float    rise_    = 1.0f / 240.0f;
    float    fall_    = 1.0f / 240.0f;
    bool     forced_  = false;
    bool     starved_ = false;
    float    value_   = 0.0f;
};

} // namespace audiomon
