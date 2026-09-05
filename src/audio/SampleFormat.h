#pragma once

#include <cstdint>
#include <string>

namespace audiomon {

inline constexpr uint16_t kMaxChannels = 8;

enum class SampleType { Unknown, Float32, Float64, Int16, Int24Packed, Int32 };

inline constexpr uint16_t bytesPerSample(SampleType type) noexcept {
    switch (type) {
        case SampleType::Float32: return 4;
        case SampleType::Float64: return 8;
        case SampleType::Int16: return 2;
        case SampleType::Int24Packed: return 3;
        case SampleType::Int32: return 4;
        default: return 0;
    }
}

struct StreamFormat {
    uint32_t   sampleRate    = 0;
    uint16_t   channels      = 0;
    uint16_t   blockAlign    = 0;
    uint16_t   bitsPerSample = 0;
    uint16_t   validBits     = 0;
    uint32_t   channelMask   = 0;
    SampleType type          = SampleType::Unknown;

    bool valid() const noexcept {
        const uint32_t minimumStride =
            static_cast<uint32_t>(channels) * bytesPerSample(type);
        return sampleRate > 0 && channels > 0 && type != SampleType::Unknown &&
               minimumStride > 0 && blockAlign >= minimumStride;
    }
    std::string describe() const;
};

// Converts raw endpoint samples to and from interleaved stereo float. This
// layer deliberately has no Windows dependency so every sample format can be
// exercised by the host-native DSP test.
class FormatConverter {
public:
    void configure(const StreamFormat& fmt) noexcept;
    void toStereoFloat(const void* src, float* dst, uint32_t frames) const noexcept;
    void fromStereoFloat(const float* src, void* dst, uint32_t frames) const noexcept;
    const StreamFormat& format() const noexcept { return fmt_; }

private:
    StreamFormat fmt_{};
    float        coefL_[kMaxChannels]{};
    float        coefR_[kMaxChannels]{};
    uint16_t     activeChannels_ = 0;
};

} // namespace audiomon
