#pragma once
#include "config/Config.h"
#include <algorithm>
namespace audiomon {
struct SourceRoute {
    SourceKind kind = SourceKind::Playback;
    bool audible = false;
    std::wstring endpoint;
    uint32_t processId = 0;
    std::vector<std::wstring> appEndpoints;
    bool routingKnown = false;
};
enum class Overlap { None, Possible, Confirmed };
inline Overlap sourceOverlap(const SourceRoute &a, const SourceRoute &b) {
    if (!a.audible || !b.audible)
        return Overlap::None;
    if (a.kind == b.kind) {
        if (a.kind == SourceKind::Application)
            return a.processId && a.processId == b.processId ? Overlap::Confirmed : Overlap::None;
        return !a.endpoint.empty() && a.endpoint == b.endpoint ? Overlap::Confirmed : Overlap::None;
    }
    const auto &app = a.kind == SourceKind::Application ? a : b;
    const auto &device = a.kind == SourceKind::Application ? b : a;
    if (app.kind != SourceKind::Application || device.kind != SourceKind::Playback || device.endpoint.empty())
        return Overlap::None;
    if (!app.routingKnown)
        return Overlap::Possible;
    return std::find(app.appEndpoints.begin(), app.appEndpoints.end(), device.endpoint) !=
                   app.appEndpoints.end()
               ? Overlap::Confirmed
               : Overlap::None;
}
} // namespace audiomon
