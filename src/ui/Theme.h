#pragma once
//
// Dashboard palette and conventional dBFS meter zones.
//
namespace audiomon::ui {

inline constexpr float kColBackground[3] = { 0.035f, 0.047f, 0.055f };

// Meter zones, matching the convention OBS uses so the colours mean the same
// thing to someone who already reads that mixer.
inline constexpr float kMeterGreenDb  = -20.0f;   // nominal below this
inline constexpr float kMeterYellowDb = -9.0f;    // hot above this
inline constexpr float kMeterRedDb    = -2.0f;    // clipping risk above this

void applyTheme();

} // namespace audiomon::ui
