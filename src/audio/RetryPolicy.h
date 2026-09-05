#pragma once

#include "audio/StreamTypes.h"

#include <algorithm>
#include <cstdint>

namespace audiomon {

// A device notification is the one reason to retry a non-retryable failure:
// an ambiguous friendly name can become unique when an endpoint disappears.
inline constexpr bool shouldRestart(StreamState state, bool wantsRetry, bool woken) noexcept {
    return state == StreamState::Failed && (wantsRetry || woken);
}

inline constexpr uint32_t retryBackoffMillis(uint32_t consecutiveFailures) noexcept {
    if (consecutiveFailures == 0) return 0;
    const uint32_t shift = std::min(consecutiveFailures - 1, 4u);
    return std::min(2000u << shift, 30000u);
}

} // namespace audiomon
