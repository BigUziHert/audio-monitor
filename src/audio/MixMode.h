#pragma once
#include <algorithm>
namespace audiomon {
// Crossfade the side signal to zero; the mid signal remains at unity. A mono
// endpoint is not required: the same averaged signal is delivered to L and R.
class MixMode {
    float blend_ = 0;

  public:
    void reset(bool mono) {
        blend_ = mono ? 1.0f : 0.0f;
    }
    void process(float &left, float &right, bool mono, float step) noexcept {
        const float target = mono ? 1.0f : 0.0f;
        blend_ += std::clamp(target - blend_, -step, step);
        const float mid = (left + right) * 0.5f;
        left += (mid - left) * blend_;
        right += (mid - right) * blend_;
    }
};
} // namespace audiomon
