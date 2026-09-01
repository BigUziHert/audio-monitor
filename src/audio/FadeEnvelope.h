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

    // Call once per block before iterating frames. `made` is how many frames
    // of real audio the block contains; `frames` is the block length.
    void beginBlock(uint32_t made, uint32_t frames) noexcept {
        made_    = made;
        frames_  = frames;
        starved_ = (made < frames);
        rampLen_ = starved_ ? std::min(made, fade_) : 0;
        start_   = starved_ ? (made - rampLen_) : frames;
        rise_    = 1.0f / float(fade_);
        // Fall fast enough to still reach zero by `made` when the block
        // starved earlier than a full fade would allow.
        fall_    = 1.0f / float(std::max<uint32_t>(1, rampLen_));
    }

    // Advance one frame and return the amplitude to apply.
    float next(uint32_t f) noexcept {
        float target = 1.0f;
        if (starved_) {
            if (f >= made_)        target = 0.0f;
            else if (f >= start_)  target = 1.0f - float(f - start_) / float(rampLen_);
        }
        if (value_ < target) value_ = std::min(target, value_ + rise_);
        else                 value_ = std::max(target, value_ - fall_);
        return value_;
    }

private:
    uint32_t fade_    = 240;
    uint32_t made_    = 0;
    uint32_t frames_  = 0;
    uint32_t rampLen_ = 0;
    uint32_t start_   = 0;
    float    rise_    = 1.0f / 240.0f;
    float    fall_    = 1.0f / 240.0f;
    bool     starved_ = false;
    float    value_   = 0.0f;
};

} // namespace audiomon
