# PhoneMonitor

> 🇷🇺 Русская версия: [README.ru.md](README.ru.md)

**Turn an Android phone into a second monitor for Windows 10/11 over USB — with ~26 ms end-to-end latency.**

Written from scratch: a Windows indirect display driver, a streaming app and a native Android client. No third-party dependencies — only the Windows SDK/WDK and Android platform APIs.

```
Windows desktop → virtual monitor (IddCx) → shared GPU memory
    → hardware H.264 → TCP over adb (USB) → MediaCodec → SurfaceView
```

## Status: v1.0, working

Verified end-to-end on real hardware, not just "it compiles".

**Test rig:** Windows 11, Intel Arc 140T iGPU with Intel QuickSync encoder · Android 16 phone, 1216×2640 @ 120 Hz, MediaTek decoder.

| Metric | Measured |
|---|---|
| Windows side: driver publishes frame → socket send | **10.3 ms** avg, 15.6 ms peak |
| Encoding (Intel QuickSync, 1080p) | 9.4 ms |
| Decoding on the phone | 6–14 ms |
| Android composition at 120 Hz | ~8.3 ms |
| **End-to-end estimate** | **≈26 ms** |
| Dropped frames per run (2,220 frames) | 0 sent, 0 received |
| Decoder queue depth | 0–1 packets |

The < 30 ms target is met on a 120 Hz panel. At 60 Hz expect ≈34 ms: Android composition costs one panel period and is the largest irreducible term.

### What works

- The driver exposes a virtual monitor; Windows parses the generated EDID and creates the `DISPLAY\PMN0001` device.
- Frames travel through shared GPU memory with **zero copies through system RAM**.
- The client receives the stream, decodes it in hardware and renders to a `SurfaceView`.
- Geometry negotiation, thermal reporting, automatic `adb forward`, reconnect after disconnect.

### Known limitations

1. **Active resolution doesn't switch** — the driver publishes a new mode list, but Windows keeps the monitor at 1920×1080; an explicit `SetDisplayConfig` call is needed.
2. **First swapchain assignment race** — `IddCxSwapChainSetDevice` returns `0x887A0026` once, the retry succeeds. Invisible to the user.
3. **Intra-Refresh on Intel QuickSync** is accepted only as a flag; the encoder picks the wave step itself.

## Key engineering decisions

- **The driver never encodes or touches the network.** `WUDFHost.exe` is supervised by the PnP manager — any long operation there causes a device start timeout. The driver does exactly two things: exposes the virtual monitor and copies frames into shared GPU memory.
- **Frames never leave GPU memory on the Windows side.** The driver copies the swapchain surface into a shared texture; the streamer opens it on the same adapter and feeds it straight to the encoder.
- **Keyed mutex instead of a fence.** A shared `ID3D11Fence` with `DuplicateHandle` failed: the driver runs under a restricted account and can't `OpenProcess` the user process, while the reverse direction needs admin rights. `IDXGIKeyedMutex` belongs to the texture itself, opens by name and needs no privileges.
- **The phone dictates the resolution.** The client's first packet reports both screen geometry **and the hardware decoder limit** — many 2400×1080 phones can only decode up to 1920×1080.
- **No latency hidden in buffers.** Encoder VBV = 1 frame, `SO_SNDBUF` = 2 frames, decoder queue = 8 NALs. A big buffer doesn't remove congestion — it turns it into hidden latency.
- **React to heat on the first signal.** `THERMAL_STATUS_LIGHT` switches to 720p; waiting for hard throttling is too late.

## Encoder support

| GPU | Path | Intra-Refresh |
|---|---|---|
| NVIDIA | NVENC directly (`nvEncodeAPI64.dll`, loaded dynamically) | 5 % frame-width stripe, precise control |
| AMD | Media Foundation MFT (official AMF wrapper) | via `CODECAPI_AVEncVideoGradualIntraRefresh` |
| Intel | Media Foundation MFT (QuickSync) | flag only, encoder picks the step |

## Components

| Directory | Component | Technologies |
|---|---|---|
| `driver/` | Indirect display driver | UMDF 2.0, IddCx 1.10, D3D11.4, C++17 |
| `streamer/` | Streaming app | Win32, NVENC / Media Foundation, Winsock, C++17 |
| `android-client/` | Client | Kotlin, MediaCodec, SurfaceView, minSdk 26 |
| `common/` | Shared contracts | `pm_ipc.h` (v4), `pm_protocol.h` (v1) |
| `tools/` | Scripts | driver install, security check, progress indicator |
| `docs/` | Docs | architecture, build, diagnostics (in Russian) |

## Quick start

**Requirements:** Visual Studio 2022 Build Tools (C++ desktop workload), Windows Driver Kit matching the SDK, CMake 3.21+, Android SDK Platform-Tools (`adb`), JDK 17 + Android SDK. Details and common pitfalls: [`docs/BUILD.md`](docs/BUILD.md).

```powershell
# 1. Build
cmake -S driver   -B build/driver   -A x64 && cmake --build build/driver   --config Release
cmake -S streamer -B build/streamer -A x64 && cmake --build build/streamer --config Release
cd android-client && .\gradlew.bat assembleDebug

# 2. Allow test-signed drivers (see the warning below), then reboot
bcdedit /set testsigning on

# 3. Sign and install the driver (creates a test certificate; -Remove to uninstall)
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install-driver.ps1

# 4. Install the client, start the app on the phone, then run the streamer
adb install -r android-client\app\build\outputs\apk\debug\app-debug.apk
build\streamer\Release\phonemonitor-streamer.exe
```

> ⚠️ **Test signing lowers system protection.** Secure Boot must be disabled in UEFI; if BitLocker is enabled, **save the recovery key and suspend protection first**, or the next boot will ask for the 48-digit key. Safe order of steps: [`docs/BUILD.md`](docs/BUILD.md), section 2.1. Check the current state any time with `tools\check-security.ps1`.

## Diagnostics

| Where | What |
|---|---|
| `C:\ProgramData\PhoneMonitor\driver.log` | driver log, every step timestamped |
| `build\driver-install.log` | driver install result |
| `build\security-status.log` | BitLocker, Secure Boot, test-signing state |
| streamer output | periodic summary: frame rate, bitrate, latency |
| `adb logcat -s PhoneMonitor:V VideoDecoder:V StreamServer:V` | client log |

Code comments and log output are in Russian.

## Author

**Daniil Burnashov**
