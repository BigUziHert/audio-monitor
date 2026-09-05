#pragma once

#include <algorithm>
#include <cwctype>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace audiomon {

// Kept independent of WASAPI so endpoint-name fallback decisions can be tested
// on any host. Each pair is {endpoint id, friendly name}.
enum class DeviceNameMatchKind { NotFound, Unique, Ambiguous };

struct DeviceNameMatchDecision {
    DeviceNameMatchKind kind = DeviceNameMatchKind::NotFound;
    size_t index = std::numeric_limits<size_t>::max();
    std::vector<size_t> matchingIndices;
};

inline std::wstring lowerDeviceName(std::wstring_view value) {
    std::wstring lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return lowered;
}

inline DeviceNameMatchDecision decideDeviceNameMatch(
    std::span<const std::pair<std::wstring, std::wstring>> devices,
    std::wstring_view requestedName) {
    DeviceNameMatchDecision decision;
    if (requestedName.empty()) return decision;

    const std::wstring needle = lowerDeviceName(requestedName);
    std::vector<size_t> partials;

    for (size_t i = 0; i < devices.size(); ++i) {
        const std::wstring name = lowerDeviceName(devices[i].second);
        if (name == needle) {
            decision.matchingIndices.push_back(i);
        } else if (name.find(needle) != std::wstring::npos) {
            partials.push_back(i);
        }
    }

    // Full-name matches take precedence over substring matches, but are only
    // safe when exactly one endpoint has that friendly name.
    if (decision.matchingIndices.empty()) {
        decision.matchingIndices = std::move(partials);
    }

    if (decision.matchingIndices.size() == 1) {
        decision.kind = DeviceNameMatchKind::Unique;
        decision.index = decision.matchingIndices.front();
    } else if (decision.matchingIndices.size() > 1) {
        decision.kind = DeviceNameMatchKind::Ambiguous;
    }
    return decision;
}

} // namespace audiomon
