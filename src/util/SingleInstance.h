#pragma once

#include <windows.h>
#include <chrono>

namespace audiomon {

// Holds ownership until application teardown finishes, including workers that
// outlive the main window. Use a separate name for isolated tests.
class SingleInstance {
public:
    enum class Result { Primary, Existing, Error };

    SingleInstance() = default;
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    Result enter(const wchar_t* mutexName, const wchar_t* windowClass,
                 UINT showMessage, bool trayLaunch,
                 std::chrono::milliseconds timeout = std::chrono::seconds(10));
    DWORD error() const { return error_; }

private:
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
    DWORD error_ = ERROR_SUCCESS;
};

} // namespace audiomon
