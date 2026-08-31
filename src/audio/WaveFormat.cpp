#include "audio/WaveFormat.h"
#include "audio/AudioGuids.h"

#include <ksmedia.h>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace audiomon {
namespace {

constexpr float kInv32768   = 1.0f / 32768.0f;
constexpr float kInv2147483 = 1.0f / 2147483648.0f;

// Reads a 24-bit little-endian sample and sign-extends it.
inline float read24(const uint8_t* p) noexcept {
    // Assemble in unsigned to avoid signed-shift overflow on the top byte,
    // then reinterpret. Landing the 24 bits in the high end of a 32-bit word
    // means the int32 scale factor applies unchanged.
    const uint32_t u = (static_cast<uint32_t>(p[0]) << 8) |
                       (static_cast<uint32_t>(p[1]) << 16) |
                       (static_cast<uint32_t>(p[2]) << 24);
    return static_cast<float>(static_cast<int32_t>(u)) * kInv2147483;
}

inline void write24(uint8_t* p, float v) noexcept {
    const float   c = std::clamp(v, -1.0f, 1.0f);
    const int32_t s = static_cast<int32_t>(c * 8388607.0f);
    p[0] = static_cast<uint8_t>(s & 0xFF);
    p[1] = static_cast<uint8_t>((s >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((s >> 16) & 0xFF);
}

} // namespace

std::string StreamFormat::describe() const {
    const char* t = "?";
    switch (type) {
        case SampleType::Float32:     t = "f32";     break;
        case SampleType::Float64:     t = "f64";     break;
        case SampleType::Int16:       t = "i16";     break;
        case SampleType::Int24Packed: t = "i24";     break;
        case SampleType::Int32:       t = "i32";     break;
        default:                      t = "unknown"; break;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%u Hz, %u ch, %s", sampleRate, channels, t);
    return std::string(buf);
}

bool parseWaveFormat(const WAVEFORMATEX* wfx, StreamFormat& out) noexcept {
    if (!wfx) return false;

    out = StreamFormat{};
    out.sampleRate    = wfx->nSamplesPerSec;
    out.channels      = wfx->nChannels;
    out.blockAlign    = wfx->nBlockAlign;
    out.bitsPerSample = wfx->wBitsPerSample;
    out.validBits     = wfx->wBitsPerSample;

    uint16_t tag = wfx->wFormatTag;

    // WAVEFORMATEXTENSIBLE is the normal case for a mix format; the real type
    // lives in SubFormat, not wFormatTag.
    if (tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        out.channelMask = ext->dwChannelMask;
        if (ext->Samples.wValidBitsPerSample) out.validBits = ext->Samples.wValidBitsPerSample;

        if      (guidEquals(ext->SubFormat, kSubtypeIeeeFloat)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (guidEquals(ext->SubFormat, kSubtypePcm))        tag = WAVE_FORMAT_PCM;
        else return false;   // compressed / AC3 / DTS passthrough: not mixable
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        if      (wfx->wBitsPerSample == 32) out.type = SampleType::Float32;
        else if (wfx->wBitsPerSample == 64) out.type = SampleType::Float64;
        else return false;
    } else if (tag == WAVE_FORMAT_PCM) {
        if      (wfx->wBitsPerSample == 16) out.type = SampleType::Int16;
        else if (wfx->wBitsPerSample == 24) out.type = SampleType::Int24Packed;
        else if (wfx->wBitsPerSample == 32) out.type = SampleType::Int32;
        else return false;
    } else {
        return false;
    }

    return out.valid();
}

void buildFloat32Format(WAVEFORMATEXTENSIBLE& out, uint32_t sampleRate, uint16_t channels) noexcept {
    std::memset(&out, 0, sizeof(out));
    out.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    out.Format.nChannels       = channels;
    out.Format.nSamplesPerSec  = sampleRate;
    out.Format.wBitsPerSample  = 32;
    out.Format.nBlockAlign     = static_cast<WORD>(channels * 4);
    out.Format.nAvgBytesPerSec = sampleRate * out.Format.nBlockAlign;
    out.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    out.Samples.wValidBitsPerSample = 32;
    out.dwChannelMask = (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    out.SubFormat     = kSubtypeIeeeFloat;
}

void buildPcmFormat(WAVEFORMATEXTENSIBLE& out, uint32_t sampleRate, uint16_t channels,
                    uint16_t bitsPerSample) noexcept {
    std::memset(&out, 0, sizeof(out));
    const uint16_t containerBits = (bitsPerSample == 24) ? 24 : bitsPerSample;
    out.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    out.Format.nChannels       = channels;
    out.Format.nSamplesPerSec  = sampleRate;
    out.Format.wBitsPerSample  = containerBits;
    out.Format.nBlockAlign     = static_cast<WORD>(channels * (containerBits / 8));
    out.Format.nAvgBytesPerSec = sampleRate * out.Format.nBlockAlign;
    out.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    out.Samples.wValidBitsPerSample = bitsPerSample;
    out.dwChannelMask = (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    out.SubFormat     = kSubtypePcm;
}

// ---------------------------------------------------------------------------

void FormatConverter::configure(const StreamFormat& fmt) noexcept {
    fmt_            = fmt;
    activeChannels_ = std::min<uint16_t>(fmt.channels, kMaxChannels);

    for (uint16_t i = 0; i < kMaxChannels; ++i) { coefL_[i] = 0.0f; coefR_[i] = 0.0f; }

    if (fmt.channels == 1) {
        coefL_[0] = 1.0f;
        coefR_[0] = 1.0f;
        return;
    }
    if (fmt.channels == 2 || fmt.channelMask == 0) {
        // No mask to reason about: front two channels are L/R by convention.
        coefL_[0] = 1.0f;
        coefR_[1] = 1.0f;
        return;
    }

    // Multichannel with a mask: standard ITU-R BS.775 style fold-down. Channel
    // order in the buffer follows ascending SPEAKER_* bit order.
    struct Slot { uint32_t bit; float l, r; };
    static constexpr float kM3dB = 0.7071068f;
    static constexpr float kSurr = 0.7071068f;
    static const Slot kSlots[] = {
        { SPEAKER_FRONT_LEFT,            1.0f,  0.0f  },
        { SPEAKER_FRONT_RIGHT,           0.0f,  1.0f  },
        { SPEAKER_FRONT_CENTER,          kM3dB, kM3dB },
        { SPEAKER_LOW_FREQUENCY,         0.0f,  0.0f  },   // LFE is dropped
        { SPEAKER_BACK_LEFT,             kSurr, 0.0f  },
        { SPEAKER_BACK_RIGHT,            0.0f,  kSurr },
        { SPEAKER_FRONT_LEFT_OF_CENTER,  kM3dB, 0.0f  },
        { SPEAKER_FRONT_RIGHT_OF_CENTER, 0.0f,  kM3dB },
        { SPEAKER_BACK_CENTER,           kSurr * kM3dB, kSurr * kM3dB },
        { SPEAKER_SIDE_LEFT,             kSurr, 0.0f  },
        { SPEAKER_SIDE_RIGHT,            0.0f,  kSurr },
    };

    uint16_t idx = 0;
    for (const auto& s : kSlots) {
        if (idx >= activeChannels_) break;
        if (fmt.channelMask & s.bit) {
            coefL_[idx] = s.l;
            coefR_[idx] = s.r;
            ++idx;
        }
    }
    if (idx == 0) { coefL_[0] = 1.0f; coefR_[1] = 1.0f; }   // mask disagreed with reality
}

void FormatConverter::toStereoFloat(const void* src, float* dst, uint32_t frames) const noexcept {
    const auto*    bytes  = static_cast<const uint8_t*>(src);
    const uint16_t nch    = fmt_.channels;
    const uint16_t stride = fmt_.blockAlign;
    const uint16_t used   = activeChannels_;

    // Fast path: the overwhelmingly common case is 32-bit float stereo, and it
    // is worth not paying for the general gather.
    if (fmt_.type == SampleType::Float32 && nch == 2) {
        std::memcpy(dst, bytes, static_cast<size_t>(frames) * 2 * sizeof(float));
        return;
    }

    for (uint32_t f = 0; f < frames; ++f) {
        const uint8_t* frame = bytes + static_cast<size_t>(f) * stride;
        float l = 0.0f, r = 0.0f;

        for (uint16_t c = 0; c < used; ++c) {
            float s = 0.0f;
            switch (fmt_.type) {
                case SampleType::Float32:
                    s = reinterpret_cast<const float*>(frame)[c];
                    break;
                case SampleType::Float64:
                    s = static_cast<float>(reinterpret_cast<const double*>(frame)[c]);
                    break;
                case SampleType::Int16:
                    s = static_cast<float>(reinterpret_cast<const int16_t*>(frame)[c]) * kInv32768;
                    break;
                case SampleType::Int32:
                    s = static_cast<float>(reinterpret_cast<const int32_t*>(frame)[c]) * kInv2147483;
                    break;
                case SampleType::Int24Packed:
                    s = read24(frame + static_cast<size_t>(c) * 3);
                    break;
                default:
                    break;
            }
            l += s * coefL_[c];
            r += s * coefR_[c];
        }

        dst[f * 2]     = l;
        dst[f * 2 + 1] = r;
    }
}

void FormatConverter::fromStereoFloat(const float* src, void* dst, uint32_t frames) const noexcept {
    auto*          bytes  = static_cast<uint8_t*>(dst);
    const uint16_t nch    = fmt_.channels;
    const uint16_t stride = fmt_.blockAlign;

    if (fmt_.type == SampleType::Float32 && nch == 2) {
        std::memcpy(bytes, src, static_cast<size_t>(frames) * 2 * sizeof(float));
        return;
    }

    for (uint32_t f = 0; f < frames; ++f) {
        uint8_t* frame = bytes + static_cast<size_t>(f) * stride;
        std::memset(frame, 0, stride);

        for (uint16_t c = 0; c < nch && c < 2; ++c) {
            const float v = std::clamp(src[f * 2 + c], -1.0f, 1.0f);
            switch (fmt_.type) {
                case SampleType::Float32:
                    reinterpret_cast<float*>(frame)[c] = v;
                    break;
                case SampleType::Float64:
                    reinterpret_cast<double*>(frame)[c] = v;
                    break;
                case SampleType::Int16:
                    reinterpret_cast<int16_t*>(frame)[c] = static_cast<int16_t>(v * 32767.0f);
                    break;
                case SampleType::Int32:
                    reinterpret_cast<int32_t*>(frame)[c] = static_cast<int32_t>(v * 2147483520.0f);
                    break;
                case SampleType::Int24Packed:
                    write24(frame + static_cast<size_t>(c) * 3, v);
                    break;
                default:
                    break;
            }
        }
        // Mono render endpoint: fold the pair down rather than dropping right.
        if (nch == 1) {
            const float v = std::clamp((src[f * 2] + src[f * 2 + 1]) * 0.5f, -1.0f, 1.0f);
            switch (fmt_.type) {
                case SampleType::Float32: reinterpret_cast<float*>(frame)[0] = v; break;
                case SampleType::Int16:   reinterpret_cast<int16_t*>(frame)[0] =
                                              static_cast<int16_t>(v * 32767.0f); break;
                case SampleType::Int32:   reinterpret_cast<int32_t*>(frame)[0] =
                                              static_cast<int32_t>(v * 2147483520.0f); break;
                case SampleType::Int24Packed: write24(frame, v); break;
                default: break;
            }
        }
    }
}

} // namespace audiomon
