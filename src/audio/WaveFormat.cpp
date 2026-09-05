#include "audio/WaveFormat.h"
#include "audio/AudioGuids.h"

#include <cstring>

namespace audiomon {

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

} // namespace audiomon
