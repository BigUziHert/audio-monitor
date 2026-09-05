#pragma once
//
// Adaptive rate correction.
//
// Each capture device and the render device run on independent crystals, so
// nominal 48kHz is really 48000*(1+e) with e of order tens to hundreds of ppm,
// drifting slowly with temperature. Left alone, every ring buffer
// monotonically fills or drains until it glitches -- minutes, not hours.
//
// The loop measures ring depth, low-passes it hard, and nudges the resampler
// ratio to hold depth at a setpoint.
//
// Design of the gains
// -------------------
// Let e = depth - target (frames) and u = ratio - 1. The plant is
//
//     de/dt = D - fs*u          D = clock drift in frames/second
//
// With PI control u = Kp*e + Ki*integral(e), the closed loop is
//
//     e'' + fs*Kp*e' + fs*Ki*e = 0
//
// so wn = sqrt(fs*Ki) and zeta = fs*Kp / (2*sqrt(fs*Ki)). Choosing
// Kp = 1/(fs*Tp) makes the proportional time constant exactly Tp, and
// Ki = 1/(4*fs*Tp^2) then gives zeta = 1: critically damped, no overshoot,
// which is what "creeps rather than jumps" requires. Tp = 10s puts settling
// around a minute -- far slower than any audible artefact, far faster than
// the drift it is tracking.
//
#include <algorithm>
#include <cmath>

namespace audiomon {

class RateController {
public:
    // A 0.5% correction is orders of magnitude more than any real crystal
    // needs, so reaching a clamp means something other than drift is wrong.
    static constexpr double kMinRatio = 0.995;
    static constexpr double kMaxRatio = 1.005;

    // Hard guarantee of gentleness, independent of controller state: the ratio
    // can never move faster than this per 10 ms of audio. Scale by the actual
    // callback duration so 5/10/20 ms devices share the same slew per second.
    static constexpr double kMaxRatioStep = 2.0e-6;

    // How much of the correction the integrator alone is allowed to supply.
    // 2000ppm is an order of magnitude beyond any real crystal error, so this
    // never binds in normal operation -- it exists so a pathological transient
    // cannot charge the integrator into a correction that takes minutes to
    // bleed off.
    static constexpr double kIntegralAuthority = 0.002;

    // sampleRate must be the rate the MEASURED DEPTH is denominated in -- that
    // is, the capture device's rate, not the render device's -- because the
    // plant model below relates depth error to consumption in those units.
    // updatePeriodSeconds is real elapsed time between update() calls, which is
    // set by the render device and is unrelated to sampleRate when the two
    // clocks run at different rates.
    void configure(double sampleRate, double targetDepthFrames,
                   double updatePeriodSeconds = 0.01) noexcept {
        fs_     = sampleRate > 0.0 ? sampleRate : 48000.0;
        target_ = targetDepthFrames;

        kp_ = 1.0 / (fs_ * kProportionalSeconds);
        ki_ = 1.0 / (4.0 * fs_ * kProportionalSeconds * kProportionalSeconds);

        // One-pole smoothing on the depth measurement. Instantaneous depth
        // steps by a whole capture packet every period; feeding that straight
        // in would make the ratio jitter.
        setUpdatePeriod(updatePeriodSeconds);

        reset();
    }

    // Move the setpoint WITHOUT disturbing convergence state.
    //
    // Distinct from configure(): that resets, which re-primes the channel and
    // drops audio. A user moving the buffer slider wants the depth to migrate
    // to a new value, not a dropout -- so smoothed depth, the integrator (which
    // holds the converged clock-drift correction) and the current ratio all
    // survive. The loop simply sees a step in error and walks there under its
    // slew limit, which for the full 20..250ms range takes tens of seconds and
    // is inaudible.
    void setTarget(double targetDepthFrames) noexcept { target_ = targetDepthFrames; }

    // Shared-mode callbacks can vary in length with endpoint padding. Keep
    // filter/integrator time correct without throwing away clock convergence.
    void setUpdatePeriod(double seconds) noexcept {
        dt_ = seconds > 0.0 ? seconds : 0.01;
        smoothA_ = std::exp(-dt_ / kMeasureSeconds);
    }

    // Call after any discontinuity: device reconnect, DATA_DISCONTINUITY, a
    // genuine starve, or an overflow drop. A loop this slow would otherwise
    // spend minutes chasing a step that was not really drift.
    void reset() noexcept {
        smoothed_ = target_;
        integral_ = 0.0;
        ratio_    = 1.0;
        primed_   = false;
    }

    // Feed the current ring depth; returns the ratio to hand the resampler.
    double update(double depthFrames) noexcept {
        // On the first sample after a reset, snap the filter to reality rather
        // than sliding over from a stale value.
        if (!primed_) { smoothed_ = depthFrames; primed_ = true; }
        else          { smoothed_ = smoothA_ * smoothed_ + (1.0 - smoothA_) * depthFrames; }

        const double error = smoothed_ - target_;   // >0 means too full: consume faster

        const double raw     = 1.0 + kp_ * error + ki_ * integral_;
        const double clamped = std::clamp(raw, kMinRatio, kMaxRatio);

        const bool saturated = (raw != clamped);
        const bool unwinding = (clamped >= kMaxRatio && error < 0.0) ||
                               (clamped <= kMinRatio && error > 0.0);
        // Conditional integration. Freeze the integrator while saturated, but
        // still allow it to unwind in the direction that leaves saturation --
        // plain "freeze when clamped" would trap it at the rail forever.
        //
        // This also covers a deliberate setpoint move. It looked like it might
        // wind the integrator up over the tens of seconds the depth takes to
        // migrate, but it cannot: kIntegralErrorLimit * kp_ is exactly the
        // clamp width (2400 / (48000*10) = 0.005), so an error large enough to
        // matter is by definition already saturating, and saturation freezes
        // the integrator. Measured both ways -- a live 50<->150ms change
        // settles back to the true clock error either way.
        if (!saturated || unwinding) {
            integral_ += std::clamp(error, -kIntegralErrorLimit, kIntegralErrorLimit) * dt_;
            const double bound = kIntegralAuthority / ki_;
            integral_ = std::clamp(integral_, -bound, bound);
        }

        // Slew limit, then clamp again so the limiter can never carry the
        // ratio outside the legal range.
        // Returning toward unity is recovery, not a new audible correction.
        // Let it unwind faster so a large downward setpoint move cannot drain
        // the ring before the controller gets back from its upper rail.
        const bool towardUnity = std::fabs(clamped - 1.0) < std::fabs(ratio_ - 1.0);
        const double limit = kMaxRatioStep * (dt_ / 0.01) * (towardUnity ? 4.0 : 1.0);
        const double step = std::clamp(clamped - ratio_, -limit, limit);
        ratio_ = std::clamp(ratio_ + step, kMinRatio, kMaxRatio);
        return ratio_;
    }

    double ratio()    const noexcept { return ratio_; }
    double smoothed() const noexcept { return smoothed_; }
    double target()   const noexcept { return target_; }

private:
    static constexpr double kProportionalSeconds = 10.0;
    static constexpr double kMeasureSeconds      = 0.5;
    static constexpr double kIntegralErrorLimit  = 2400.0;   // frames (50ms @ 48k)

    double fs_       = 48000.0;
    double dt_       = 0.01;
    double target_   = 0.0;
    double kp_       = 0.0;
    double ki_       = 0.0;
    double smoothA_  = 0.0;
    double smoothed_ = 0.0;
    double integral_ = 0.0;
    double ratio_    = 1.0;
    bool   primed_   = false;
};

} // namespace audiomon
