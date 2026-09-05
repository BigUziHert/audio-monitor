#include "audio/SampleFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace audiomon {
namespace {

constexpr float kInv32768 = 1.0f / 32768.0f;
constexpr float kInv2147483648 = 1.0f / 2147483648.0f;

constexpr uint32_t kFrontLeft          = 0x001;
constexpr uint32_t kFrontRight         = 0x002;
constexpr uint32_t kFrontCenter        = 0x004;
constexpr uint32_t kLowFrequency       = 0x008;
constexpr uint32_t kBackLeft           = 0x010;
constexpr uint32_t kBackRight          = 0x020;
constexpr uint32_t kFrontLeftOfCenter  = 0x040;
constexpr uint32_t kFrontRightOfCenter = 0x080;
constexpr uint32_t kBackCenter         = 0x100;
constexpr uint32_t kSideLeft           = 0x200;
constexpr uint32_t kSideRight          = 0x400;

template <typename T>
T loadSample(const uint8_t* source) noexcept {
    T value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

template <typename T>
void storeSample(uint8_t* destination, T value) noexcept {
    std::memcpy(destination, &value, sizeof(value));
}

float read24(const uint8_t* p) noexcept {
    const uint32_t u = (static_cast<uint32_t>(p[0]) << 8) |
                       (static_cast<uint32_t>(p[1]) << 16) |
                       (static_cast<uint32_t>(p[2]) << 24);
    return static_cast<float>(static_cast<int32_t>(u)) * kInv2147483648;
}

void write24(uint8_t* p, float v) noexcept {
    const int32_t sample = static_cast<int32_t>(std::clamp(v, -1.0f, 1.0f) * 8388607.0f);
    p[0] = static_cast<uint8_t>(sample & 0xFF);
    p[1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((sample >> 16) & 0xFF);
}

float readSample(const uint8_t* frame, uint16_t channel, SampleType type) noexcept {
    const uint8_t* sample = frame + static_cast<size_t>(channel) * bytesPerSample(type);
    switch (type) {
        case SampleType::Float32: return loadSample<float>(sample);
        case SampleType::Float64: return static_cast<float>(loadSample<double>(sample));
        case SampleType::Int16: return static_cast<float>(loadSample<int16_t>(sample)) * kInv32768;
        case SampleType::Int24Packed: return read24(sample);
        case SampleType::Int32: return static_cast<float>(loadSample<int32_t>(sample)) * kInv2147483648;
        default: return 0.0f;
    }
}

void writeSample(uint8_t* frame, uint16_t channel, SampleType type, float value) noexcept {
    uint8_t* sample = frame + static_cast<size_t>(channel) * bytesPerSample(type);
    const float v = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
    switch (type) {
        case SampleType::Float32: storeSample(sample, v); break;
        case SampleType::Float64: storeSample(sample, static_cast<double>(v)); break;
        case SampleType::Int16: storeSample(sample, static_cast<int16_t>(v * 32767.0f)); break;
        case SampleType::Int24Packed: write24(sample, v); break;
        case SampleType::Int32: storeSample(sample, static_cast<int32_t>(v * 2147483520.0f)); break;
        default: break;
    }
}

} // namespace

std::string StreamFormat::describe() const {
    const char* text = "unknown";
    switch (type) {
        case SampleType::Float32: text = "f32"; break;
        case SampleType::Float64: text = "f64"; break;
        case SampleType::Int16: text = "i16"; break;
        case SampleType::Int24Packed: text = "i24"; break;
        case SampleType::Int32: text = "i32"; break;
        default: break;
    }
    char description[96];
    std::snprintf(description, sizeof(description), "%u Hz, %u ch, %s", sampleRate, channels, text);
    return description;
}

void FormatConverter::configure(const StreamFormat& fmt) noexcept {
    fmt_ = fmt;
    activeChannels_ = std::min<uint16_t>(fmt.channels, kMaxChannels);
    std::fill(std::begin(coefL_), std::end(coefL_), 0.0f);
    std::fill(std::begin(coefR_), std::end(coefR_), 0.0f);

    if (fmt.channels == 1) {
        coefL_[0] = coefR_[0] = 1.0f;
        return;
    }
    if (fmt.channels == 2 || fmt.channelMask == 0) {
        coefL_[0] = 1.0f;
        coefR_[1] = 1.0f;
        return;
    }

    struct Slot { uint32_t bit; float left; float right; };
    constexpr float kMinus3Db = 0.7071068f;
    constexpr Slot slots[] = {
        {kFrontLeft, 1.0f, 0.0f}, {kFrontRight, 0.0f, 1.0f},
        {kFrontCenter, kMinus3Db, kMinus3Db}, {kLowFrequency, 0.0f, 0.0f},
        {kBackLeft, kMinus3Db, 0.0f}, {kBackRight, 0.0f, kMinus3Db},
        {kFrontLeftOfCenter, kMinus3Db, 0.0f},
        {kFrontRightOfCenter, 0.0f, kMinus3Db},
        {kBackCenter, 0.5f, 0.5f}, {kSideLeft, kMinus3Db, 0.0f},
        {kSideRight, 0.0f, kMinus3Db},
    };
    uint16_t index = 0;
    for (const auto& slot : slots) {
        if (index >= activeChannels_) break;
        if (fmt.channelMask & slot.bit) {
            coefL_[index] = slot.left;
            coefR_[index] = slot.right;
            ++index;
        }
    }
    if (index == 0 && activeChannels_ >= 2) {
        coefL_[0] = 1.0f;
        coefR_[1] = 1.0f;
    }
}

void FormatConverter::toStereoFloat(const void* src, float* dst, uint32_t frames) const noexcept {
    if (!fmt_.valid()) {
        std::fill_n(dst, static_cast<size_t>(frames) * 2, 0.0f);
        return;
    }
    const auto* bytes = static_cast<const uint8_t*>(src);
    if (fmt_.type == SampleType::Float32 && fmt_.channels == 2 && fmt_.blockAlign == 8) {
        std::memcpy(dst, bytes, static_cast<size_t>(frames) * 2 * sizeof(float));
        return;
    }
    for (uint32_t frameIndex = 0; frameIndex < frames; ++frameIndex) {
        const uint8_t* frame = bytes + static_cast<size_t>(frameIndex) * fmt_.blockAlign;
        float left = 0.0f;
        float right = 0.0f;
        for (uint16_t channel = 0; channel < activeChannels_; ++channel) {
            const float sample = readSample(frame, channel, fmt_.type);
            left += sample * coefL_[channel];
            right += sample * coefR_[channel];
        }
        dst[frameIndex * 2] = left;
        dst[frameIndex * 2 + 1] = right;
    }
}

void FormatConverter::fromStereoFloat(const float* src, void* dst, uint32_t frames) const noexcept {
    if (!fmt_.valid()) return;
    auto* bytes = static_cast<uint8_t*>(dst);
    for (uint32_t frameIndex = 0; frameIndex < frames; ++frameIndex) {
        uint8_t* frame = bytes + static_cast<size_t>(frameIndex) * fmt_.blockAlign;
        std::memset(frame, 0, fmt_.blockAlign);
        if (fmt_.channels == 1) {
            writeSample(frame, 0, fmt_.type,
                        (src[frameIndex * 2] + src[frameIndex * 2 + 1]) * 0.5f);
            continue;
        }
        const uint16_t channels = std::min<uint16_t>(fmt_.channels, 2);
        for (uint16_t channel = 0; channel < channels; ++channel)
            writeSample(frame, channel, fmt_.type, src[frameIndex * 2 + channel]);
    }
}

} // namespace audiomon
