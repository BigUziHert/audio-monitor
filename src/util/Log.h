#pragma once
//
// Diagnostic logging. NEVER call any of this from an audio thread -- it takes
// a lock and touches a file. Audio threads report status by storing into
// atomics that the UI or a worker thread picks up later.
//
#include <string>

namespace audiomon::log {

void init(const std::wstring& appDataDir);
void shutdown();

// Also echo every line to stdout, flushed immediately. The console survives a
// __fastfail termination that can otherwise leave buffered file output lost,
// and it works even when the log file could not be opened at all.
void setEcho(bool enabled);

void write(const char* level, const char* fmt, ...);
// Last 128 KiB, retained even when no log file is open. Non-real-time only.
std::string recentText();

#define LOG_INFO(...)  ::audiomon::log::write("INFO",  __VA_ARGS__)
#define LOG_WARN(...)  ::audiomon::log::write("WARN",  __VA_ARGS__)
#define LOG_ERR(...)   ::audiomon::log::write("ERROR", __VA_ARGS__)

// Formats an HRESULT as both the numeric code and, where we recognise it, the
// WASAPI-specific name -- AUDCLNT_E_* codes are otherwise unreadable.
std::string hrString(long hr);

} // namespace audiomon::log
