#pragma once
//
// Endpoint format description and conversion to the mixer's internal format
// (interleaved stereo float32).
//
// We cannot assume anything about what these endpoints present. The Arctis
// pair are normally 48k stereo float, but a game switching a headset into
// virtual 7.1 makes the loopback stream 8 channels, and a Blue Yeti may come
// up mono or at 44.1kHz. Everything below is detection, not assumption.
//
#include <windows.h>
#include <mmreg.h>
#include <cstdint>
#include <string>

namespace audiomon {

inline constexpr uint16_t kMaxChannels = 8;

enum class SampleType { Unknown, Float32, Float64, Int16, Int24Packed, Int32 };

struct StreamFormat {
    uint32_t   sampleRate    = 0;
    uint16_t   channels      = 0;
    uint16_t   blockAlign    = 0;
    uint16_t   bitsPerSample = 0;
    uint16_t   validBits     = 0;
    uint32_t   channelMask   = 0;
    SampleType type          = SampleType::Unknown;

    bool valid() const noexcept {
        return sampleRate > 0 && channels > 0 && type != SampleType::Unknown && blockAlign > 0;
    }
    std::string describe() const;
};

// Reads a WAVEFORMATEX / WAVEFORMATEXTENSIBLE as handed back by
// GetMixFormat or GetCurrentSharedModeEnginePeriod.
bool parseWaveFormat(const WAVEFORMATEX* wfx, StreamFormat& out) noexcept;

// Builds a WAVEFORMATEXTENSIBLE describing float32 stereo at the given rate,
// for probing an exclusive-mode endpoint.
void buildFloat32Format(WAVEFORMATEXTENSIBLE& out, uint32_t sampleRate, uint16_t channels) noexcept;

// Builds a WAVEFORMATEXTENSIBLE describing packed integer PCM. HDMI endpoints
// frequently refuse float in exclusive mode and require 16- or 24-bit PCM.
void buildPcmFormat(WAVEFORMATEXTENSIBLE& out, uint32_t sampleRate, uint16_t channels,
                    uint16_t bitsPerSample) noexcept;

// ---------------------------------------------------------------------------
// Converts a raw endpoint buffer into interleaved stereo float, folding any
// channel count down to two. Allocation-free and const, so it is safe to call
// from a capture thread.
// ---------------------------------------------------------------------------
class FormatConverter {
public:
    void configure(const StreamFormat& fmt) noexcept;

    // src: `frames` frames of the configured format. dst: frames*2 floats.
    void toStereoFloat(const void* src, float* dst, uint32_t frames) const noexcept;

    // Writes stereo float into the configured format, for the render side.
    void fromStereoFloat(const float* src, void* dst, uint32_t frames) const noexcept;

    const StreamFormat& format() const noexcept { return fmt_; }

private:
    StreamFormat fmt_{};
    float        coefL_[kMaxChannels]{};
    float        coefR_[kMaxChannels]{};
    uint16_t     activeChannels_ = 0;
};

} // namespace audiomon
