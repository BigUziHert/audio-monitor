#include "util/UpdateDownload.h"
#include "util/Text.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace audiomon::updates {
namespace {

constexpr char kAssetRoot[] = "https://api.github.com/repos/BigUziHert/audio-monitor/releases/assets/";
constexpr auto kDownloadDeadline = std::chrono::minutes(5);

bool lowerHex(const std::string& value, size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string assetUrl(const UpdateResult& update) {
    return std::string(kAssetRoot) + std::to_string(update.assetId);
}

bool validMetadata(const UpdateResult& update) {
    return update.status == UpdateStatus::Available &&
        (update.channel == "dev" || update.channel == "main") &&
        lowerHex(update.commit, 40) && lowerHex(update.sha256, 64) &&
        update.assetId > 0 && update.assetId <= 9007199254740991ull &&
        update.downloadSize > 0 && update.downloadSize <= kMaximumUpdateBytes;
}

bool splitHttpsUrl(const std::string& url, std::string& host, std::string& path) {
    if (url.rfind("https://", 0) != 0 || url.size() > 16384 ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) {
            return c <= 0x20 || c >= 0x7f || c == '\\' || c == '#';
        })) return false;
    const auto slash = url.find('/', 8);
    if (slash == std::string::npos || slash == 8) return false;
    host = url.substr(8, slash - 8);
    path = url.substr(slash);
    // A literal authority excludes credentials, arbitrary ports and encoded
    // hostname tricks. Real GitHub redirects use these canonical hostnames.
    return host.find_first_of("@:%?") == std::string::npos;
}

bool trustedRedirect(const std::string& url) {
    std::string host, path;
    return splitHttpsUrl(url, host, path) &&
        (host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com") &&
        path.rfind("/github-production-release-asset", 0) == 0;
}

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET handle) : handle_(handle) {}
    ~InternetHandle() { if (handle_) WinHttpCloseHandle(handle_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }
private:
    HINTERNET handle_;
};

class FileHandle {
public:
    explicit FileHandle(const std::wstring& path) : handle_(CreateFileW(path.c_str(), GENERIC_WRITE,
        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr)) {
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Could not create the update download. Check free space and try again.");
    }
    ~FileHandle() { CloseHandle(handle_); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    void write(std::span<const uint8_t> bytes) {
        while (!bytes.empty()) {
            DWORD written = 0;
            if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
                written == 0)
                throw std::runtime_error("Could not save the update download. Check free space and try again.");
            bytes = bytes.subspan(written);
        }
    }
    void flush() {
        if (!FlushFileBuffers(handle_))
            throw std::runtime_error("Could not finish saving the update download. Try again.");
    }
private:
    HANDLE handle_;
};

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("Windows could not initialize update verification.");
        DWORD objectLength = 0, copied = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &copied, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw std::runtime_error("Windows could not initialize update verification.");
        }
        try {
            object_.resize(objectLength);
            if (BCryptCreateHash(algorithm_, &hash_, object_.data(), objectLength, nullptr, 0, 0) < 0)
                throw std::runtime_error("Windows could not initialize update verification.");
        } catch (...) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw;
        }
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    void add(std::span<const uint8_t> bytes) {
        if (BCryptHashData(hash_, const_cast<PUCHAR>(bytes.data()),
                           static_cast<ULONG>(bytes.size()), 0) < 0)
            throw std::runtime_error("Windows could not verify the update download.");
    }
    std::string finish() {
        std::array<uint8_t, 32> digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("Windows could not verify the update download.");
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (auto byte : digest) {
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
        return result;
    }
private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<uint8_t> object_;
};

std::wstring createStage() {
    std::vector<wchar_t> temp(32768);
    const DWORD count = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (!count || count >= temp.size())
        throw std::runtime_error("Windows could not find a temporary directory for the update.");
    std::wstring parent(temp.data(), count);
    if (parent.back() != L'\\') parent += L'\\';
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        std::array<uint8_t, 16> random{};
        if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Windows could not create a private update directory.");
        constexpr wchar_t hex[] = L"0123456789abcdef";
        std::wstring directory = parent + L"AudioMonitorUpdate-";
        for (auto byte : random) {
            directory += hex[byte >> 4];
            directory += hex[byte & 15];
        }
        if (CreateDirectoryW(directory.c_str(), nullptr)) return directory;
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    throw std::runtime_error("Could not create the update directory. Check free space and try again.");
}

detail::DownloadResponse networkFailure() {
    const DWORD code = GetLastError();
    return {0, {}, code == ERROR_WINHTTP_TIMEOUT
        ? "The update download timed out. Check your connection and try again."
        : "The update download failed (Windows error " + std::to_string(code) + "). Try again."};
}

detail::DownloadResponse networkGet(const std::string& url, const detail::DownloadSink& sink,
                                    DownloadProgress& progress) {
    if (progress.cancel.load()) return {0, {}, "Update download canceled."};
    std::string host, path;
    if (!splitHttpsUrl(url, host, path)) return {0, {}, "Invalid update download address."};
    InternetHandle session(WinHttpOpen(L"AudioMonitor-UpdateDownload/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session || !WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 5000))
        return networkFailure();
    const auto wideHost = toWide(host), widePath = toWide(path);
    InternetHandle connection(WinHttpConnect(session.get(), wideHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) return networkFailure();
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", widePath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) return networkFailure();
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)))
        return networkFailure();
    const wchar_t* headers = host == "api.github.com"
        ? L"Accept: application/octet-stream\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
        : L"Accept: application/octet-stream\r\n";
    if (!WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr))
        return networkFailure();
    DWORD status = 0, bytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
        return networkFailure();
    detail::DownloadResponse response{static_cast<unsigned>(status), {}, {}};
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        std::array<wchar_t, 16385> location{};
        bytes = static_cast<DWORD>(location.size() * sizeof(wchar_t));
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                location.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return networkFailure();
        response.location = toUtf8(location.data());
        return response;
    }
    if (status != 200) return response;
    std::array<uint8_t, 64 * 1024> buffer{};
    for (;;) {
        if (progress.cancel.load()) return {0, {}, "Update download canceled."};
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read))
            return networkFailure();
        if (!read) return response;
        if (!sink({buffer.data(), read})) return {0, {}, "Update download stopped."};
    }
}

} // namespace

PreparedUpdate downloadUpdate(const UpdateResult& update, DownloadProgress& progress) {
    return detail::downloadUpdate(update, progress, networkGet, prepareUpdatePackage);
}

namespace detail {

PreparedUpdate downloadUpdate(const UpdateResult& update, DownloadProgress& progress,
                              const DownloadGet& get, const PreparePackage& prepare) {
    std::wstring directory;
    progress.received = 0;
    progress.total = 0;
    try {
        if (!validMetadata(update))
            throw std::runtime_error("No verified update is available. Check for updates again.");
        if (progress.cancel.load()) throw std::runtime_error("Update download canceled.");
        progress.total = update.downloadSize;
        directory = createStage();
        const auto deadline = std::chrono::steady_clock::now() + kDownloadDeadline;
        const auto checkRunning = [&] {
            if (progress.cancel.load()) throw std::runtime_error("Update download canceled.");
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("The update download took too long. Check your connection and try again.");
        };
        {
            FileHandle file(directory + L"\\update.zip");
            Sha256 hash;
            uint64_t received = 0;
            std::string sinkError;
            const DownloadSink sink = [&](std::span<const uint8_t> chunk) {
                try {
                    checkRunning();
                    if (chunk.size() > update.downloadSize - received)
                        throw std::runtime_error("The update size does not match the published download. Check for updates again.");
                    file.write(chunk);
                    hash.add(chunk);
                    received += chunk.size();
                    progress.received = received;
                    return true;
                } catch (const std::exception& error) {
                    sinkError = error.what();
                    return false;
                }
            };
            std::string url = assetUrl(update);
            for (unsigned redirects = 0;; ++redirects) {
                checkRunning();
                const auto response = get(url, sink, progress);
                checkRunning();
                if (!sinkError.empty()) throw std::runtime_error(sinkError);
                if (!response.error.empty()) throw std::runtime_error(response.error);
                if (response.status == 200) break;
                if (response.status == 301 || response.status == 302 || response.status == 303 ||
                    response.status == 307 || response.status == 308) {
                    if (redirects >= 4 || received || !trustedRedirect(response.location))
                        throw std::runtime_error("GitHub returned an unexpected update download redirect. Try again later.");
                    url = response.location;
                    continue;
                }
                throw std::runtime_error(response.status == 404
                    ? "This update was replaced while downloading. Check for updates again."
                    : "The update download returned HTTP " + std::to_string(response.status) + ". Try again later.");
            }
            if (received != update.downloadSize)
                throw std::runtime_error("The update download was incomplete. Check your connection and try again.");
            if (hash.finish() != update.sha256)
                throw std::runtime_error("The update failed SHA-256 verification. Nothing was installed; check for updates and retry.");
            file.flush();
        }
        checkRunning();
        std::string error;
        if (!prepare(directory, error))
            throw std::runtime_error(error.empty() ? "Could not prepare the downloaded update." : error);
        checkRunning();
        return {directory, {}};
    } catch (const std::exception& error) {
        if (!directory.empty()) discardPreparedUpdate({directory, {}});
        return {{}, error.what()};
    } catch (...) {
        if (!directory.empty()) discardPreparedUpdate({directory, {}});
        return {{}, "The update download could not finish. Try again."};
    }
}

} // namespace detail

} // namespace audiomon::updates
