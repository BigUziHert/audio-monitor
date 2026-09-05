cmake_minimum_required(VERSION 3.21)
project(audio-monitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AUDIOMON_BUILD_GUI "Build the Dear ImGui tray application" ON)
option(AUDIOMON_BUILD_CLI "Build the headless console bring-up tool" ON)

if(NOT WIN32)
  message(FATAL_ERROR "audio-monitor targets Windows only (WASAPI). Use cmake/mingw-w64-x86_64.cmake to cross-compile for portability checks.")
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(dsp-test tests/dsp_test.cpp)
  target_link_libraries(dsp-test PRIVATE audiomon_core)
  add_test(NAME dsp COMMAND dsp-test)
  add_executable(config-test tests/config_test.cpp)
  target_link_libraries(config-test PRIVATE audiomon_core)
  add_test(NAME config COMMAND config-test)
  add_executable(audio-engine-test tests/audio_engine_test.cpp)
  target_link_libraries(audio-engine-test PRIVATE audiomon_core)
  add_test(NAME audio-engine COMMAND audio-engine-test)
  add_executable(output-bus-test tests/output_bus_test.cpp)
  target_link_libraries(output-bus-test PRIVATE audiomon_core)
  add_test(NAME output-bus COMMAND output-bus-test)
  add_executable(source-drift-test tests/source_drift_test.cpp)
  target_link_libraries(source-drift-test PRIVATE audiomon_core)
  add_test(NAME source-drift COMMAND source-drift-test)
  set_tests_properties(source-drift output-bus PROPERTIES TIMEOUT 90)
  add_executable(app-capture-test tests/app_capture_test.cpp)
  target_link_libraries(app-capture-test PRIVATE audiomon_core)
  add_test(NAME app-capture COMMAND app-capture-test)
  set_tests_properties(app-capture PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 35)
  add_executable(device-match-test tests/device_match_test.cpp)
  target_link_libraries(device-match-test PRIVATE audiomon_core)
  add_test(NAME device-match COMMAND device-match-test)
  add_executable(loopback-render-test tests/loopback_render_test.cpp)
  target_link_libraries(loopback-render-test PRIVATE audiomon_core)
  add_test(NAME loopback-render COMMAND loopback-render-test)
  set_tests_properties(loopback-render PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
  add_executable(lifecycle-test tests/lifecycle_test.cpp)
  target_link_libraries(lifecycle-test PRIVATE audiomon_core)
  add_test(NAME lifecycle COMMAND lifecycle-test)
  set_tests_properties(lifecycle PROPERTIES TIMEOUT 60)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- compiler ---
add_library(audiomon_flags INTERFACE)
# Deliberately NOT using the aggressive NOxxx trimming macros here. They save
# a trivial amount of compile time and break third-party code that legitimately
# uses the excluded APIs -- ImGui needs the clipboard, for one.
target_compile_definitions(audiomon_flags INTERFACE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
)

if(MSVC)
  target_compile_options(audiomon_flags INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc
    /fp:fast            # audio maths; we never rely on strict IEEE ordering
    $<$<CONFIG:Release>:/O2 /GL /Gy /Gw>
  )
  target_link_options(audiomon_flags INTERFACE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
else()
  # mingw-w64 portability path
  target_compile_options(audiomon_flags INTERFACE
    -Wall -Wextra -Wno-unknown-pragmas -Wno-cast-function-type
    -ffast-math
    $<$<CONFIG:Release>:-O2>
  )
endif()

# -------------------------------------------------------------------- core ---
# Everything that is not UI. Linked by both the GUI app and the CLI tool so the
# audio path can be brought up and verified without any window existing.
add_library(audiomon_core STATIC
  src/audio/AppAudio.cpp
  src/audio/DeviceManager.cpp
  src/audio/CaptureStream.cpp
  src/audio/RenderStream.cpp
  src/audio/OutputBus.cpp
  src/audio/MixPumpScheduler.cpp
  src/audio/AudioEngine.cpp
  src/audio/SampleFormat.cpp
  src/audio/WaveFormat.cpp
  src/audio/Guids.cpp
  src/config/Config.cpp
  src/config/Json.cpp
  src/util/Log.cpp
  src/util/Text.cpp
  src/util/Startup.cpp
)
target_include_directories(audiomon_core PUBLIC src)
target_link_libraries(audiomon_core PUBLIC
  audiomon_flags
  mmdevapi ole32 oleaut32 uuid avrt advapi32 shell32 shlwapi user32
)

# --------------------------------------------------------------------- cli ---
if(AUDIOMON_BUILD_CLI)
  add_executable(audiomon-cli src/cli_main.cpp)
  target_link_libraries(audiomon-cli PRIVATE audiomon_core)
endif()

# Bring-up diagnostic: logs one line on the first mix block so a crash can be
# localised to before/inside/after it. This logs from the render thread, which
# is NOT real-time safe, so it is opt-in and off by default.
option(AUDIOMON_TRACE_FIRST_MIX "Log the first mix block (bring-up only)" OFF)
if(AUDIOMON_TRACE_FIRST_MIX)
  target_compile_definitions(audiomon_core PUBLIC AUDIOMON_TRACE_FIRST_MIX)
endif()

# --------------------------------------------------------------------- gui ---
if(AUDIOMON_BUILD_GUI)
  include(FetchContent)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(imgui)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  )
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC audiomon_flags d3d11 dxgi d3dcompiler dwmapi)

  add_executable(audio-monitor WIN32
    src/gui_main.cpp
    src/ui/MixerWindow.cpp
    src/ui/TrayIcon.cpp
    src/ui/Renderer.cpp
    src/ui/Theme.cpp
    res/app.rc
  )
  target_link_libraries(audio-monitor PRIVATE audiomon_core imgui)
  if(BUILD_TESTING)
    add_executable(ui-test tests/ui_test.cpp src/ui/MixerWindow.cpp src/ui/Theme.cpp)
    target_link_libraries(ui-test PRIVATE audiomon_core imgui)
    add_test(NAME dashboard COMMAND ui-test)
    add_executable(renderer-test tests/renderer_test.cpp src/ui/Renderer.cpp src/ui/Theme.cpp)
    target_link_libraries(renderer-test PRIVATE audiomon_core imgui)
    add_test(NAME renderer COMMAND renderer-test)
    set_tests_properties(renderer PROPERTIES TIMEOUT 30)
  endif()

  # The entry point is wWinMain. MSVC resolves that from /SUBSYSTEM:WINDOWS
  # with UNICODE defined; mingw needs -municode to select the wide runtime
  # startup, or it looks for WinMain and fails to link.
  if(NOT MSVC)
    target_compile_options(audio-monitor PRIVATE -municode)
    target_link_options(audio-monitor PRIVATE -municode)
  endif()
endif()
