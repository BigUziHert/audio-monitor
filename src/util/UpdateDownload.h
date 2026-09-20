#pragma once

#include "util/UpdateInstaller.h"
#include "util/Updates.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>

namespace audiomon::updates {

struct DownloadProgress {
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
};

// Downloads, verifies and stages a published asset. Never opens a browser,
// stops audio, installs files or launches the staged application.
PreparedUpdate downloadUpdate(const UpdateResult& update, DownloadProgress& progress);

namespace detail {
struct DownloadResponse {
    unsigned status = 0;
    std::string location;
    std::string error;
};
using DownloadSink = std::function<bool(std::span<const uint8_t>)>;
// Stream body bytes only for HTTP 200. Redirects are handled by the downloader,
// with the same trust restrictions for real and injected transports.
using DownloadGet = std::function<DownloadResponse(const std::string&, const DownloadSink&,
                                                  DownloadProgress&)>;
using PreparePackage = std::function<bool(const std::wstring&, std::string&)>;
PreparedUpdate downloadUpdate(const UpdateResult& update, DownloadProgress& progress,
                              const DownloadGet& get, const PreparePackage& prepare);
}

} // namespace audiomon::updates
