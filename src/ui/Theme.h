#pragma once
//
// Visual style. Modelled on the OBS audio mixer: dark, flat, and legible at a
// glance rather than decorative.
//
namespace audiomon::ui {

inline constexpr float kColBackground[3] = { 0.129f, 0.133f, 0.145f };

// Meter zones, matching the convention OBS uses so the colours mean the same
// thing to someone who already reads that mixer.
inline constexpr float kMeterGreenDb  = -20.0f;   // nominal below this
inline constexpr float kMeterYellowDb = -9.0f;    // hot above this
inline constexpr float kMeterRedDb    = -2.0f;    // clipping risk above this

void applyTheme();

} // namespace audiomon::ui
