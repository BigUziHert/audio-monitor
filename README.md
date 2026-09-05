# audio-monitor

A small Windows tray application that mixes several audio sources together and
sends the mix to one or more playback devices or capture cards, **without installing a virtual audio driver
and without ever changing your system's audio devices.**

It was built for a two-PC streaming setup: the gaming PC runs this, sums game
audio, chat audio and a microphone, and feeds the result to an Elgato HDMI
capture card. The streaming PC has no microphone of its own — everything
arrives over HDMI as a single feed.

```
  Arctis "Game"  endpoint ──(WASAPI loopback)──┐              ┌──► Elgato HDMI
  Arctis "Chat"  endpoint ──(WASAPI loopback)──┼──► mix bus ──┼──► Speakers
  Blue Yeti      microphone ─(shared capture)──┘              └──► Recorder
```

## What it does

- **Dashboard mixer.** A two-column layout with Dark, Light, and Windows-following
  System themes, source cards, a live frequency spectrum, output selection, and
  audio health status.
- **Up to 16 sources.** Add playback devices, microphones, or individual
  applications such as Discord. The original Game, Chat, and Microphone
  selections are migrated automatically.
- **Independent source controls.** The checkmark includes/excludes a source;
  the speaker mutes it while retaining its levels. The arrow opens source
  selection, naming, mix gain boost, and removal. Card sliders and their editable
  percentage fields provide a separate 0-100% volume control without changing
  Windows or headset volumes.
- **Continuous source meters.** Start/Stop Monitoring changes output forwarding
  while source capture and metering continue on the same clock. Playback-device,
  microphone, and application meters keep the same response without resetting
  or reopening their streams. Output devices open and close in the background
  so the dashboard stays responsive. With monitoring stopped, hiding the window
  in the tray also closes source capture.
- **Stereo / Mono.** Click the Channels card to switch live. Mono averages
  left and right and sends the same signal to both sides, with a short ramp
  to avoid a click. The selection is saved.
- **Useful status.** Click Status for details about unavailable devices,
  clipping, recent buffer drops, duplicate sources, or output feedback risk.
  Selecting a playback endpoint and an app routed through it reports
  **Duplicate audio**. Unconfirmed app routing reports **Possible duplicate**.
  Warnings update about every two seconds and clear when sources are muted,
  disabled, removed, or routed separately. Detection uses Windows session
  routing; it does not analyze acoustic microphone echo.
- **Up to four simultaneous outputs.** Add playback destinations from the
  Output Devices panel, cycle between their cards, and configure independent
  device selection, naming, icon, mix gain, volume, and mute. Output percentages
  can be typed directly from 0 to 100%, just like source percentages. Each output has
  its own buffered clock-drift correction, so one stalled device cannot block
  the others. Stop/Start Monitoring preserves the complete route.
- **Tray support.** Minimize keeps audio running and releases the renderer.
  Close exits by default; Settings can make Close hide to the tray instead.
  Optional Windows startup and hidden manual launch remain available.

Application capture requires **Windows build 20348 or later** (Windows 11
recommended) and a Windows SDK that includes `audioclientactivationparams.h`.
It captures the selected process and its children, regardless of output device.
Start audio in an app and use Refresh if it is missing from the picker. The
executable path is saved, so a normal app restart reconnects automatically;
after an app update moves its executable, select it again. Multiple independent
instances at the same path are reported as ambiguous; close extra instances.
See Microsoft's [process loopback documentation](https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ns-audioclientactivationparams-audioclient_activation_params).

Adding, removing, or replacing a source or output briefly restarts a running mix.
Volume, mute, stereo/mono, and buffer changes apply live. Sample Rate displays
the fixed 48,000 Hz internal mix rate; its Settings link opens the classic Windows
Sound control panel for device-format changes. A Keybinds settings category is
reserved for future shortcut controls.

## Why loopback instead of a virtual audio driver

This is the whole point of the project, so it is worth being explicit.

The usual way to solve this problem — Elgato Wave Link, VoiceMeeter, VB-Cable —
is to install a **virtual audio driver**: a fake sound card that Windows sees
as a real endpoint. Applications are then pointed at the fake device, the tool
mixes what arrives there, and the result is forwarded to real hardware.

That works, but it has a structural problem on a machine that reboots:

- The virtual endpoints must become your **default** devices for the routing
  to work at all.
- Virtual audio drivers enumerate late in the boot sequence — often after the
  audio service has already picked defaults.
- When the device Windows remembers as default is missing at that moment,
  Windows falls back to whatever *is* present, which is your headset.
- So every reboot silently reverts your defaults, and you fix them by hand.

**WASAPI loopback removes the need for the fake device entirely.** Loopback
capture attaches to a *render* endpoint and receives a copy of whatever is
being played to it. Applications keep using the real headset, the real headset
stays the default output, and this app is a passive observer of the same
stream. There is nothing to install, nothing to enumerate late, and nothing to
revert on boot.

The same reasoning applies to the microphone. WASAPI **shared-mode** capture
genuinely multiplexes one capture endpoint across several clients — the audio
engine hands each client its own buffer — so Discord and this app can hold the
Yeti at the same time. Only exclusive mode would lock Discord out, and this app
never requests exclusive mode on an input.

Passivity is enforced structurally rather than by good intentions:

- `IMMNotificationClient::OnDefaultDeviceChanged` is deliberately inert. The
  app observes the default changing and does nothing, so it can never
  *follow* a default onto a device you did not choose.
- Nothing calls `GetDefaultAudioEndpoint` to decide what to open. Devices are
  bound by endpoint ID, with a friendly-name fallback.
- Exclusive mode on the output is **refused automatically** if that endpoint is
  a system default for any role, because taking exclusive control of the
  default output would break every other application on the machine. If it
  *becomes* a default later, the app notices and reopens in shared mode --
  yielding control, never following the default.
- No endpoint volume, mute, or default-device state is ever written.

## Building

Requires **Visual Studio 2022** (or the standalone Build Tools) with the
Desktop development with C++ workload, a **Windows 11 SDK** (22621 or newer
recommended), and **CMake 3.21+**. Dear ImGui is fetched automatically by CMake, so
the first configure needs a network connection.

```powershell
.\scripts\build.ps1
```

That script exists because of a specific papercut: Visual Studio ships its own
CMake but only puts it on `PATH` inside the **Developer PowerShell for VS 2022**
shell, so a normal PowerShell window reports `cmake is not recognized` on a
machine that already has everything it needs. The script locates Visual Studio
via `vswhere`, falls back to the bundled CMake, and tells you exactly what to
install if something really is missing.

If you would rather drive CMake yourself, either open **Developer PowerShell for
VS 2022** from the Start menu, or install CMake standalone
(`winget install Kitware.CMake`, then open a new terminal), and run:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The binaries land in `build\Release\`:

| Binary | What it is |
| --- | --- |
| `audio-monitor.exe` | the tray application |
| `audiomon-cli.exe`  | a headless console tool, useful for bring-up |

### Build and test

Exit any running Audio Monitor instance first (tray menu > Exit). Windows
cannot replace an executable that is still running, and launching a second
instance only shows the first one's window.

```powershell
git switch main
git pull --ff-only origin main
.\scripts\build.ps1 -Test
.\build\Release\audio-monitor.exe
```

`-BuildDir` is optional and defaults to `build`. `-Test` runs the DSP and sample
format checks, configuration persistence and migration tests, deterministic
audio-engine tests, device-name matching, headless dashboard and D3D renderer
tests, concurrent stream lifecycle stress, process-capture activation/restart,
and loopback/shared-render integration.

The process-capture test only opens its own process; it does not play sound or
modify Windows audio routing, and it skips when process capture is unsupported.
The loopback/render test opens the first active render endpoint, restarts its
loopback capture three times, and renders silence in shared mode. When two
endpoints are available it also opens both through the multi-output engine; it
skips hardware coverage when no render endpoint exists. The dashboard and renderer tests exercise layouts,
controls, and device recovery without a visible desktop window. Audible behavior
still needs a test with your devices.

To test overlap, add your speaker/headphone endpoint and add Discord as an
Application audio source. Play Discord audio through that endpoint: Status
should turn amber with Duplicate audio and identify both sources. Muting or
disabling either source clears the overlap warning. Click Channels to compare
Stereo and Mono; Stop Monitoring should silence the forwarded mix while the
source meters continue to move, and leave its configuration ready to resume.

### Bring-up: get audio flowing before worrying about the UI

Start with the console tool. It runs the entire mixer with no window, so it
isolates "is the audio path working" from "is the UI working".

```powershell
# List every endpoint the machine has, with its ID
.\build\Release\audiomon-cli.exe --list

# Run the mixer and print live meters
.\build\Release\audiomon-cli.exe

# Same, plus startup breadcrumbs on the console (device open failures)
.\build\Release\audiomon-cli.exe --verbose
```

If the meters move and the capture card receives audio, everything that
matters works. If a channel says `unavailable`, the name match did not find
your endpoint — see Configuration below.

### Cross-compiling (CI / portability check only)

The tree also builds with mingw-w64 on Linux. This is **not** the shipping
build; it exists so the WASAPI code can be compile- and link-checked on a
machine that is not Windows.

```bash
./scripts/crossbuild.sh        # builds both executables
./scripts/run_tests.sh         # host-native DSP, format, JSON and policy tests
```

## Configuration

Settings live in `%APPDATA%\audio-monitor\config.json` and are written
whenever you change something in the UI.

```json
{
  "version": 5,
  "sources": [
    { "label": "Headphones", "kind": "playback", "enabled": true, "processPath": "", "deviceId": "", "deviceName": "Arctis Pro Wireless Game", "gain": 1, "volume": 1, "muted": false },
    { "label": "Chat Audio", "kind": "playback", "enabled": true, "processPath": "", "deviceId": "", "deviceName": "Arctis Pro Wireless Chat", "gain": 1, "volume": 1, "muted": false },
    { "label": "Microphone", "kind": "microphone", "enabled": true, "processPath": "", "deviceId": "", "deviceName": "USB Advanced Audio Device", "gain": 1, "volume": 1, "muted": false }
  ],
  "output": { "label": "", "kind": "playback", "enabled": true, "processPath": "", "deviceId": "", "deviceName": "Elgato 4K", "gain": 1, "volume": 1, "muted": false },
  "outputs": [
    { "label": "", "kind": "playback", "enabled": true, "processPath": "", "deviceId": "", "deviceName": "Elgato 4K", "gain": 1, "volume": 1, "muted": false }
  ],
  "mono": false,
  "closeToTray": false,
  "exclusiveOutput": true,
  "startWithWindows": false,
  "startMinimized": false,
  "colorTheme": "dark",
  "bufferMillis": 50
}
```

`gain` is the 0-400% mix gain configured in the editor. `volume` is the
independent 0-100% dashboard fader; the effective level is their product.
`colorTheme` accepts `dark`, `light`, or `system`; `system` follows the Windows
app-theme preference. `outputs` is the ordered list of simultaneous destinations,
up to four. `output` mirrors its first item so older builds can still load the
primary destination.

On the first save of a version 1 configuration, the original is preserved as
`config.json.v1.bak` beside `config.json`. To return to an older build, exit the
app and copy that backup over `config.json`. If an existing configuration is
malformed, it is preserved as `config.json.corrupt.bak` before replacement.
`processPath` is empty for device sources and contains the selected executable's
full path for application sources. Process IDs are discovered at runtime and
are deliberately not persisted.


Each device is stored **twice**: as an exact endpoint ID and as a
case-insensitive friendly-name substring. The ID is tried first. If it no
longer resolves — which is what happens after a driver reinstall, since that
reissues endpoint IDs — the name substring is used instead and the refreshed ID
is written back. That is what stops a driver update from silently orphaning
your configuration.

A name substring must match **exactly one** endpoint. If it matches several the
app refuses to attach and logs the candidates rather than picking one, because
guessing wrong means audio silently goes somewhere plausible-looking with no
error. This is not hypothetical: on a machine with Elgato Wave Link installed,
`"Elgato"` matches three render endpoints and the shortest is Wave Link's
*virtual driver* rather than the capture card. The defaults are chosen to be
unambiguous, not merely plausible.

If a default picks nothing or is ambiguous on your machine, open the source card's **arrow**
in the app and choose the endpoint explicitly; the exact ID is then persisted.

`startWithWindows` registers `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
— per-user, so it needs no administrator rights — with a `--tray` flag so a
boot launch goes straight to the notification area.

## Design notes

Three problems dominate this kind of application. Each is handled explicitly.

### Clock drift

The Arctis, the Yeti and the capture card all run on independent crystal
oscillators. Nominal 48 kHz is really 48000·(1+ε) with ε of order tens to
hundreds of parts per million, drifting with temperature. Uncorrected, every
buffer slowly fills or drains until it glitches — minutes, not hours. At
100 ppm a buffer gains about 4.8 frames per second, which is a full 50 ms
buffer every eight minutes.

Each channel therefore runs a control loop that holds its buffer depth at a
setpoint by nudging a resampling ratio, bounded to [0.995, 1.005]. The gains
are derived rather than tuned by hand. With `e` the depth error in frames and
`u = ratio − 1`, the plant is `de/dt = D − fs·u`, so PI control gives

```
e'' + fs·Kp·e' + fs·Ki·e = 0
```

Choosing `Kp = 1/(fs·Tp)` makes the proportional time constant exactly `Tp`,
and `Ki = 1/(4·fs·Tp²)` then yields ζ = 1 — critically damped, no overshoot.
`Tp` is 10 seconds. A slew limit of 2·10⁻⁶ per block guarantees the ratio
creeps rather than steps regardless of controller state, and the depth
measurement is low-passed at 0.5 s because instantaneous depth jumps by a whole
packet every period.

This part is covered by tests that run without any audio hardware
(`scripts/run_tests.sh`): 20 simulated minutes at ±100 ppm and +400 ppm settle
to exactly the true clock error with zero underruns, and a two-second capture
stall recovers in about 20 seconds.

### Loopback silence

WASAPI loopback delivers **no packets at all** when nothing is playing to that
endpoint — not silence, nothing. Two consequences:

- **It cannot be event-driven.** With `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, a
  silent endpoint never signals the event, so the wait blocks forever the
  moment the game goes quiet. Loopback is polled here, on a high-resolution
  waitable timer at half the device period.
- **The render side still needs data every period.** Silence is synthesised on
  the consumer: when a ring cannot supply a full block the mixer fills the
  remainder with zeros and fades that channel out over ~5 ms, fading back in
  when audio returns. Without the fade you get an audible click every time the
  game falls silent and resumes.

"No packets" is explicitly *not* treated as a dead device — otherwise pausing a
game would tear down and rebuild the stream.

### Real-time safety

A fixed 48 kHz MMCSS **"Pro Audio"** pump is the single consumer of every
capture ring. It creates the canonical mix once, then writes an independent
lock-free SPSC bus for each output. Each WASAPI render thread consumes only its
own bus and resamples gently onto that endpoint's clock. The pump and render
callbacks perform no allocation, locking, logging, or COM calls; everything
they touch is preallocated at startup.

Thread topology is one capture thread per enabled source, one mix pump, and one
render thread per configured output. Captures never run on the pump: their
`GetBuffer`/`ReleaseBuffer` COM calls have no non-blocking guarantee, so a
stalled source can only starve its own ring. Likewise, a stalled output can
only overflow its own fan-out bus and cannot hold up another destination.

Meters are polled by the UI from atomics. The audio thread only ever folds a
new maximum into an atomic; all decay, hold and dB conversion happen on the UI
timer, where wall-clock time actually exists. Reading is destructive
(max-since-last-read), so no peak is missed between frames.

### Device resilience

An `IMMNotificationClient` watches for endpoint changes. Its callbacks fire on
a WASAPI-owned thread while MMDevice holds an internal lock, so they do the
absolute minimum — set a flag, signal an event, return — and a supervisor
thread performs every rebuild. Releasing an `IAudioClient` from inside one of
those callbacks can deadlock against the audio service.

Notifications are debounced by 750 ms, because a driver reinstall fires a burst
of them. A capture that fails is retried; the capture card enumerating late on
a cold boot is treated as a normal startup state rather than an error, and the
app attaches to it automatically when it appears.

### Spectrum display

The mix pump feeds a bounded visualization queue. A 2,048-point Hann-windowed
FFT runs on the UI thread and displays 64 logarithmic bands (30 Hz to 20 kHz,
limited by Nyquist). The two sides are analyzed separately so opposite-polarity
stereo signals do not disappear from the graph. Visualization samples are
dropped when the queue fills; the audio path never waits for the window.

### Output format negotiation

`GetMixFormat` reports the shared-mode *engine* format, which is always float —
it says nothing about what the hardware accepts in exclusive mode. HDMI carries
LPCM on the wire and has no float representation, so the exclusive-mode probe
tries the endpoint's own configured format first, then integer PCM (16/24/32),
and only then float. It also implements the `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED`
retry, which needs a *fresh* `IAudioClient` because a failed `Initialize`
leaves the object spent.

If exclusive mode is unavailable — the endpoint's "Allow applications to take
exclusive control" box is unchecked, another application holds it, or the
device is a system default — the app falls back to shared mode rather than
leaving the stream PC with no audio.

## Project layout

```
src/audio/     WASAPI capture and render, mixing, drift correction, metering
src/ui/        ImGui mixer panel, tray icon, D3D11 renderer
src/config/    JSON config in %APPDATA%
src/util/      logging, COM smart pointer, autostart registration
tests/         DSP/unit, headless UI/renderer, and Windows audio integration tests
```

## Licence

MIT. See [LICENSE](LICENSE).
