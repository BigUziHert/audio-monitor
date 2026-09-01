# audio-monitor

A small Windows tray application that mixes several audio sources together and
sends the mix to a capture card, **without installing a virtual audio driver
and without ever changing your system's audio devices.**

It was built for a two-PC streaming setup: the gaming PC runs this, sums game
audio, chat audio and a microphone, and feeds the result to an Elgato HDMI
capture card. The streaming PC has no microphone of its own — everything
arrives over HDMI as a single feed.

```
  Arctis "Game"  endpoint ──(WASAPI loopback)──┐
  Arctis "Chat"  endpoint ──(WASAPI loopback)──┼──► mix ──► Elgato HDMI (exclusive)
  Blue Yeti      microphone ─(shared capture)──┘
```

## What it does

- **Three input channels.** Two are WASAPI *loopback* captures of render
  endpoints (the Arctis Game and Chat outputs); the third is an ordinary
  shared-mode capture of a USB microphone.
- **One output.** All three are summed and rendered to the capture card's
  endpoint, in exclusive mode where possible.
- **An OBS-style mixer.** Per channel: a dB-linear fader, a mute button, and a
  stereo peak meter with peak-hold and OBS's colour zones.
- **Faders are monitor-only.** Moving a fader changes what reaches the capture
  card and nothing else. The level in your own headset is untouched, exactly
  like OBS monitoring.
- **Lives in the tray.** Optionally starts with Windows, minimised. Minimising
  or closing hides it from the taskbar and Alt-Tab entirely, leaving only the
  tray icon.

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

Requires **Visual Studio 2022** (or the standalone Build Tools) with the C++
workload, and **CMake 3.21+**. Dear ImGui is fetched automatically by CMake, so
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

### Bring-up: get audio flowing before worrying about the UI

Start with the console tool. It runs the entire mixer with no window, so it
isolates "is the audio path working" from "is the UI working".

```powershell
# List every endpoint the machine has, with its ID
.\build\Release\audiomon-cli.exe --list

# Run the mixer and print live meters
.\build\Release\audiomon-cli.exe
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
./scripts/run_tests.sh         # host-native tests for the DSP and config code
```

## Configuration

Settings live in `%APPDATA%\audio-monitor\config.json` and are written
whenever you change something in the UI.

```json
{
  "version": 1,
  "game":   { "deviceId": "{0.0.0.00000000}.{...}", "deviceName": "Arctis Pro Wireless Game", "gain": 1, "muted": false },
  "chat":   { "deviceId": "",                       "deviceName": "Arctis Pro Wireless Chat", "gain": 1, "muted": false },
  "mic":    { "deviceId": "",                       "deviceName": "USB Advanced Audio Device", "gain": 1, "muted": false },
  "output": { "deviceId": "",                       "deviceName": "Elgato 4K", "gain": 1, "muted": false },
  "exclusiveOutput": true,
  "startWithWindows": false,
  "startMinimized": false,
  "bufferMillis": 50
}
```

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
`"Elgato"` matches three render endpoints and the shortest is Wave Link'''s
*virtual driver* rather than the capture card. The defaults are chosen to be
unambiguous, not merely plausible.

If a default picks nothing or is ambiguous on your machine, open **Settings**
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

The render thread is the master clock. It runs in the MMCSS **"Pro Audio"**
task class so a fullscreen game cannot starve it, with denormal flushing (FTZ
*and* DAZ) enabled per-thread, and it performs no allocation, no locking, no
logging and no COM calls. Everything it touches is preallocated at startup.

Thread topology is three capture threads plus one render thread, communicating
through lock-free SPSC ring buffers. The alternative — draining all three
captures on the render thread — was rejected because `GetBuffer`/`ReleaseBuffer`
are COM calls with no non-blocking guarantee, and one stalled capture device
would then stall the output. Here a stalled capture simply stops filling its
ring and the mixer emits silence for that channel alone.

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
tests/         host-native tests for the platform-independent core
```

## Licence

MIT. See [LICENSE](LICENSE).
