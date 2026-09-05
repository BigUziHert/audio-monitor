#include "audio/DeviceMatch.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

int main() {
    using namespace audiomon;
    using Entry = std::pair<std::wstring, std::wstring>;

    int failures = 0;
    const auto check = [&](bool condition, const char* label) {
        if (!condition) {
            std::printf("FAIL: %s\n", label);
            ++failures;
        }
    };

    {
        const std::vector<Entry> devices{{L"id-1", L"Speakers"},
                                         {L"id-2", L"Speakers"}};
        const auto match = decideDeviceNameMatch(devices, L"speakers");
        check(match.kind == DeviceNameMatchKind::Ambiguous,
              "identical exact friendly names are ambiguous");
        check(match.matchingIndices.size() == 2,
              "both identical exact matches are reported");
    }
    {
        const std::vector<Entry> devices{{L"id-1", L"Elgato"},
                                         {L"id-2", L"Elgato Wave Link"}};
        const auto match = decideDeviceNameMatch(devices, L"ELGATO");
        check(match.kind == DeviceNameMatchKind::Unique && match.index == 0,
              "one exact match wins over substring matches");
    }
    {
        const std::vector<Entry> devices{{L"id-1", L"Elgato 4K"},
                                         {L"id-2", L"Speakers"}};
        const auto match = decideDeviceNameMatch(devices, L"4k");
        check(match.kind == DeviceNameMatchKind::Unique && match.index == 0,
              "one substring match resolves uniquely");
    }
    {
        const std::vector<Entry> devices{{L"id-1", L"Elgato 4K"},
                                         {L"id-2", L"Elgato Chat"}};
        check(decideDeviceNameMatch(devices, L"Elgato").kind ==
                  DeviceNameMatchKind::Ambiguous,
              "multiple substring matches are ambiguous");
        check(decideDeviceNameMatch(devices, L"Missing").kind ==
                  DeviceNameMatchKind::NotFound,
              "missing name is not found");
    }

    std::printf("Device matching tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
