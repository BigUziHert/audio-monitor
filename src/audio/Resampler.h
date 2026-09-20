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
// 4-point Catmull-Rom, preceded by an anti-alias low-pass for genuine rate
// reductions. Allocation-free; all state belongs to the consumer thread.
//
#include "audio/RingBuffer.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace audiomon {

class DriftResampler {
public:
    // Configure from the nominal rate, not the continuously changing drift
    // correction. Seven fixed biquads are substantially cheaper than a long
    // FIR at 96/192 kHz, and need no extra ring lookahead or priming frames.
    // The ordinary same-rate drift path remains the exact cubic interpolator.
    void configure(double nominalRatio) noexcept {
        configured_ = true;
        filtered_ = nominalRatio > 1.01;
        if (filtered_) {
            constexpr double rippleDb = 0.1;
            constexpr double order = 2.0 * kFilterSections;
            const double epsilon = std::sqrt(std::pow(10.0, rippleDb / 10.0) - 1.0);
            const double mu = std::asinh(1.0 / epsilon) / order;
            // Passband ends at 86% of the new Nyquist. Reserve the entire
            // +/-0.5% drift range without redesigning the filter each block.
            const double warped = std::tan(std::numbers::pi * 0.43 /
                                            (nominalRatio * 1.005));
            for (size_t i = 0; i < sections_.size(); ++i) {
                // Low-Q sections first keep intermediate signals bounded.
                const double theta = std::numbers::pi *
                    (2.0 * (kFilterSections - 1 - i) + 1.0) / (2.0 * order);
                const double real = -std::sinh(mu) * std::sin(theta);
                const double imaginary = std::cosh(mu) * std::cos(theta);
                const double a = -2.0 * real * warped;
                const double b = (real * real + imaginary * imaginary) * warped * warped;
                const double denominator = 1.0 + a + b;
                auto& section = sections_[i];
                section.b0 = b / denominator;
                section.b1 = 2.0 * section.b0;
                section.b2 = section.b0;
                section.a1 = 2.0 * (b - 1.0) / denominator;
                section.a2 = (1.0 - a + b) / denominator;
            }
            // Even-order Chebyshev filters have their ripple minimum at DC.
            // Scale once so the passband never amplifies a full-scale input.
            const double gain = std::pow(10.0, -rippleDb / 20.0);
            sections_[0].b0 *= gain;
            sections_[0].b1 *= gain;
            sections_[0].b2 *= gain;
        }
        reset();
    }

    void reset() noexcept {
        for (int i = 0; i < 4; ++i) { hl_[i] = 0.0f; hr_[i] = 0.0f; }
        for (auto& section : sections_) {
            section.z1L = section.z2L = section.z1R = section.z2R = 0.0;
        }
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
        // Standalone users may omit configure(); production supplies the
        // nominal ratio at format changes, before its first audio block.
        if (!configured_) configure(ratio);
        const uint32_t avail = ring.beginRead();
        uint32_t taken = 0;   // input frames consumed
        uint32_t made  = 0;   // output frames produced

        // Prime the 4-point kernel before the first sample can be interpolated.
        if (!primed_) {
            if (avail < 4) { ring.endRead(0); return 0; }
            for (int i = 0; i < 4; ++i) readFrame(ring, taken++, hl_[i], hr_[i]);
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
                readFrame(ring, taken++, hl_[3], hr_[3]);
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
    static constexpr size_t kFilterSections = 7;
    struct Section {
        double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1L = 0.0, z2L = 0.0, z1R = 0.0, z2R = 0.0;
    };

    void readFrame(const StereoRing& ring, uint32_t index, float& l, float& r) noexcept {
        ring.readFrame(index, l, r);
        if (!filtered_) return;
        // A non-finite device sample must not poison recursive filter history
        // forever. Inspect bits so this also survives fast-math builds.
        if ((std::bit_cast<uint32_t>(l) & 0x7f800000u) == 0x7f800000u) l = 0.0f;
        if ((std::bit_cast<uint32_t>(r) & 0x7f800000u) == 0x7f800000u) r = 0.0f;
        double left = l, right = r;
        for (auto& section : sections_) {
            const double nextL = section.b0 * left + section.z1L;
            const double nextR = section.b0 * right + section.z1R;
            section.z1L = section.b1 * left - section.a1 * nextL + section.z2L;
            section.z1R = section.b1 * right - section.a1 * nextR + section.z2R;
            section.z2L = section.b2 * left - section.a2 * nextL;
            section.z2R = section.b2 * right - section.a2 * nextR;
            left = nextL;
            right = nextR;
        }
        l = static_cast<float>(left);
        r = static_cast<float>(right);
    }

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
    bool   configured_ = false;
    bool   filtered_ = false;
    std::array<Section, kFilterSections> sections_{};
};

} // namespace audiomon
