#pragma once
#include "audio/RingBuffer.h"
#include <array>
#include <complex>
#include <cmath>
#include <algorithm>
namespace audiomon {
// UI-thread FFT. The audio thread only copies into a bounded SPSC queue and
// drops visualization samples when the window is hidden; it never waits for UI.
class Spectrum {
  public:
    static constexpr size_t kSize = 2048, kBands = 64;
    std::array<float, kBands> levels{};
    Spectrum() {
        levels.fill(-60.0f);
    }
    void update(StereoRing &ring, uint32_t rate, float dt, bool active) {
        for (auto &level : levels)
            level = std::max(-60.0f, level - dt * 36.0f);
        const auto n = ring.beginRead();
        for (uint32_t i = 0; i < n; ++i) {
            float l, r;
            ring.readFrame(i, l, r);
            left_[cursor_] = l;
            right_[cursor_] = r;
            cursor_ = (cursor_ + 1) % kSize;
            filled_ = std::min(kSize, filled_ + 1);
        }
        ring.endRead(n);
        if (!active) {
            filled_ = 0;
            return;
        }
        if (!n || filled_ < kSize || !rate)
            return;
        std::array<float, kSize / 2> power{};
        for (const auto *samples : {&left_, &right_}) {
            std::array<std::complex<float>, kSize> fft;
            for (size_t i = 0; i < kSize; ++i) {
                const float window = 0.5f - 0.5f * std::cos(6.283185307f * float(i) / float(kSize - 1));
                fft[i] = (*samples)[(cursor_ + i) % kSize] * window;
            }
            for (size_t i = 1, j = 0; i < kSize; ++i) {
                size_t bit = kSize >> 1;
                for (; j & bit; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                    std::swap(fft[i], fft[j]);
            }
            for (size_t len = 2; len <= kSize; len <<= 1) {
                const std::complex<float> rotation = std::polar(1.0f, -6.283185307f / float(len));
                for (size_t i = 0; i < kSize; i += len) {
                    std::complex<float> w = 1.0f;
                    for (size_t j = 0; j < len / 2; ++j) {
                        const auto u = fft[i + j], v = fft[i + j + len / 2] * w;
                        fft[i + j] = u + v;
                        fft[i + j + len / 2] = u - v;
                        w *= rotation;
                    }
                }
            }
            for (size_t i = 1; i < kSize / 2; ++i)
                power[i] = std::max(power[i], std::abs(fft[i]) * 4.0f / float(kSize));
        }
        const float high = std::min(20000.0f, float(rate) * 0.5f);
        for (size_t band = 0; band < kBands; ++band) {
            const float lowHz = 30.0f * std::pow(high / 30.0f, float(band) / float(kBands));
            const float highHz = 30.0f * std::pow(high / 30.0f, float(band + 1) / float(kBands));
            const size_t a = std::clamp(size_t(lowHz * kSize / rate), size_t(1), kSize / 2 - 1);
            const size_t b = std::clamp(size_t(std::ceil(highHz * kSize / rate)), a + 1, kSize / 2);
            float peak = 0.0f;
            for (size_t i = a; i < b; ++i)
                peak = std::max(peak, power[i]);
            levels[band] =
                std::max(levels[band], std::clamp(20.0f * std::log10(std::max(peak, 0.001f)), -60.0f, 0.0f));
        }
    }

  private:
    std::array<float, kSize> left_{}, right_{};
    size_t cursor_ = 0, filled_ = 0;
};
} // namespace audiomon
