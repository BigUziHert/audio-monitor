#include "util/Log.h"

#include <windows.h>
#include <audioclient.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <deque>
#include <cstring>

namespace audiomon::log {
namespace {

std::mutex  g_mutex;
FILE*       g_file = nullptr;
bool        g_echo = false;
constexpr size_t kRecentLimit = 128 * 1024;
std::deque<std::string> g_recent;
size_t g_recentBytes = 0;

const char* wasapiName(long hr) {
    switch (hr) {
        case AUDCLNT_E_NOT_INITIALIZED:          return "AUDCLNT_E_NOT_INITIALIZED";
        case AUDCLNT_E_ALREADY_INITIALIZED:      return "AUDCLNT_E_ALREADY_INITIALIZED";
        case AUDCLNT_E_WRONG_ENDPOINT_TYPE:      return "AUDCLNT_E_WRONG_ENDPOINT_TYPE";
        case AUDCLNT_E_DEVICE_INVALIDATED:       return "AUDCLNT_E_DEVICE_INVALIDATED";
        case AUDCLNT_E_NOT_STOPPED:              return "AUDCLNT_E_NOT_STOPPED";
        case AUDCLNT_E_BUFFER_TOO_LARGE:         return "AUDCLNT_E_BUFFER_TOO_LARGE";
        case AUDCLNT_E_OUT_OF_ORDER:             return "AUDCLNT_E_OUT_OF_ORDER";
        case AUDCLNT_E_UNSUPPORTED_FORMAT:       return "AUDCLNT_E_UNSUPPORTED_FORMAT";
        case AUDCLNT_E_INVALID_SIZE:             return "AUDCLNT_E_INVALID_SIZE";
        case AUDCLNT_E_DEVICE_IN_USE:            return "AUDCLNT_E_DEVICE_IN_USE";
        case AUDCLNT_E_BUFFER_OPERATION_PENDING: return "AUDCLNT_E_BUFFER_OPERATION_PENDING";
        case AUDCLNT_E_THREAD_NOT_REGISTERED:    return "AUDCLNT_E_THREAD_NOT_REGISTERED";
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED:   return "AUDCLNT_E_ENDPOINT_CREATE_FAILED";
        case AUDCLNT_E_SERVICE_NOT_RUNNING:      return "AUDCLNT_E_SERVICE_NOT_RUNNING";
        case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:  return "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
        case AUDCLNT_E_RESOURCES_INVALIDATED:    return "AUDCLNT_E_RESOURCES_INVALIDATED";
        case AUDCLNT_S_BUFFER_EMPTY:             return "AUDCLNT_S_BUFFER_EMPTY";
        default:                                 return nullptr;
    }
}

} // namespace

void init(const std::wstring& appDataDir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    std::wstring path = appDataDir + L"\\audio-monitor.log";
    // "wb", NOT "w, ccs=UTF-8".
    //
    // A ccs= encoding puts the stream into Unicode mode, and MSVC's CRT then
    // treats a narrow fputs on it as an invalid parameter -- which calls
    // __fastfail and kills the process on the very first log line, faulting
    // inside ucrtbase with 0xC0000409. The file is opened, so it appears as an
    // empty log and the crash looks like it came from somewhere else entirely.
    //
    // We already format UTF-8 bytes ourselves, so binary mode is exactly right:
    // no encoding translation, and no CRLF rewriting either.
    _wfopen_s(&g_file, path.c_str(), L"wb");
}

void setEcho(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_echo = enabled;
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

void write(const char* level, const char* fmt, ...) {
    char body[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[1200];
    std::snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %-5s %s\n",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level, body);

    OutputDebugStringA(line);

    std::lock_guard<std::mutex> lock(g_mutex);
    // Diagnostic retention must never turn a best-effort logger into a worker
    // failure if the process is under memory pressure.
    try {
        const size_t bytes = std::strlen(line);
        g_recent.emplace_back(line);
        g_recentBytes += bytes;
        while (g_recentBytes > kRecentLimit && !g_recent.empty()) {
            g_recentBytes -= g_recent.front().size();
            g_recent.pop_front();
        }
    } catch (...) {}
    if (g_file) { fputs(line, g_file); fflush(g_file); }
    if (g_echo) { fputs(line, stdout); fflush(stdout); }
}

std::string recentText() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string text;
    text.reserve(g_recentBytes);
    for (const auto& line : g_recent) text += line;
    return text;
}

std::string hrString(long hr) {
    char buf[128];
    if (const char* n = wasapiName(hr)) std::snprintf(buf, sizeof(buf), "%s (0x%08lX)", n,
                                                      static_cast<unsigned long>(hr));
    else                                std::snprintf(buf, sizeof(buf), "0x%08lX",
                                                      static_cast<unsigned long>(hr));
    return std::string(buf);
}

} // namespace audiomon::log
