#pragma once
//
// Fractional resampler for clock-drift correction.
//
// The ratio is "input frames consumed per output frame produced" and sits at
// ~1.000 in steady state -- this exists to absorb the few-hundred-ppm
// disagreement between four independent crystals, not to do musical sample
// rate conversion. It also covers a genuine rate mismatch (a 44.1kHz mic into
// a 48kHz mix) at ratio ~0.919.
//
// 4-point Catmull-Rom. Allocation-free; all state lives in the object, so it
// is safe to drive from the render thread.
//
#include "audio/RingBuffer.h"
#include <cstdint>

namespace audiomon {

class DriftResampler {
public:
    void reset() noexcept {
        for (int i = 0; i < 4; ++i) { hl_[i] = 0.0f; hr_[i] = 0.0f; }
        phase_  = 0.0;
        primed_ = false;
    }

    // Pull `n` output frames into `out` (interleaved stereo), consuming from
    // `ring` at `ratio` input frames per output frame.
    //
    // Returns the number of frames actually produced. A short return means the
    // ring starved: the caller fills the remainder with silence and ramps the
    // channel down so the gap does not click.
    uint32_t produce(StereoRing& ring, float* out, uint32_t n, double ratio) noexcept {
        const uint32_t avail = ring.beginRead();
        uint32_t taken = 0;   // input frames consumed
        uint32_t made  = 0;   // output frames produced

        // Prime the 4-point kernel before the first sample can be interpolated.
        if (!primed_) {
            if (avail < 4) { ring.endRead(0); return 0; }
            for (int i = 0; i < 4; ++i) ring.readFrame(taken++, hl_[i], hr_[i]);
            phase_  = 0.0;
            primed_ = true;
        }

        while (made < n) {
            // Advance whole input frames until the read position sits between
            // h[1] and h[2].
            while (phase_ >= 1.0) {
                if (taken >= avail) {        // starved: stop cleanly, keep state
                    ring.endRead(taken);
                    return made;
                }
                hl_[0] = hl_[1]; hl_[1] = hl_[2]; hl_[2] = hl_[3];
                hr_[0] = hr_[1]; hr_[1] = hr_[2]; hr_[2] = hr_[3];
                ring.readFrame(taken++, hl_[3], hr_[3]);
                phase_ -= 1.0;
            }

            const float t = static_cast<float>(phase_);
            out[made * 2]     = catmullRom(hl_[0], hl_[1], hl_[2], hl_[3], t);
            out[made * 2 + 1] = catmullRom(hr_[0], hr_[1], hr_[2], hr_[3], t);
            ++made;
            phase_ += ratio;
        }

        ring.endRead(taken);
        return made;
    }

private:
    // Interpolates between p1 and p2; p0 and p3 shape the curve.
    static inline float catmullRom(float p0, float p1, float p2, float p3, float t) noexcept {
        const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
        const float b =         p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
        const float c = -0.5f * p0                + 0.5f * p2;
        return ((a * t + b) * t + c) * t + p1;
    }

    float  hl_[4]{}, hr_[4]{};
    double phase_  = 0.0;   // in [0,1); never accumulates, so it cannot drift
    bool   primed_ = false;
};

} // namespace audiomon
