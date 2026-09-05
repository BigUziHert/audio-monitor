#pragma once
#include "audio/SampleFormat.h"
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

namespace audiomon {

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

} // namespace audiomon
