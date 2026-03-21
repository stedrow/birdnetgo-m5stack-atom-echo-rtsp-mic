# Architecture & Troubleshooting Guide

## Hardware

- **Board**: AtomS3 Lite
- **Microphone**: Unit Mini PDM
- **Audio**: 16-bit PCM, 16kHz mono (configurable 8–48kHz)
- **Streaming**: RTSP/RTP over TCP, port 8554

### Pin Configuration
```cpp
I2S_CLK_PIN     = 1   // PDM clock (G1)
I2S_DATA_IN_PIN = 2   // PDM data (G2)
WS2812_LED_PIN  = 35  // Built-in RGB LED
```

## Architecture

### Dual-Core Design (v2.3.0)

- **Core 1**: Complete audio pipeline (I2S capture → HPF → adaptive noise suppressor → gain → limiter → optional AGC → RTP → WiFi) + RTSP keepalive/TEARDOWN processing
- **Core 0**: Web UI, RTSP negotiation, diagnostics, client management

Core 1 **exclusively owns** the WiFiClient socket during streaming — Core 0 never touches it:

1. Core 0 handles RTSP negotiation (OPTIONS → DESCRIBE → SETUP → PLAY)
2. On PLAY, Core 0 hands off the socket to Core 1 via `streamClient` pointer with memory barrier
3. Core 1 reads audio, processes, sends RTP packets, and handles RTSP keepalives/TEARDOWN
4. On disconnect, Core 1 closes the socket itself; Core 0 detects the transition and updates LED/logs
5. When Core 0 needs to stop streaming (TEARDOWN, overheat, WiFi loss), it signals via `requestStreamStop()` and waits for Core 1 to confirm cleanup

### Cross-Core Safety
- FreeRTOS semaphore with 2s timeout for confirmed task exit
- `portMUX_TYPE` spinlock for shared log buffer
- Xtensa `memw` memory barriers on critical flag transitions
- `core1OwnsLED` flag prevents concurrent FastLED/RMT driver access

## Audio Tuning

### Active Filters
- **High-pass filter**: 2nd-order Butterworth, roughly 12 dB/octave, default 450 Hz.
- **Adaptive noise suppressor**: envelope follower + hold-time gate. This was retuned to stop the audible tap-tap artifact that happened when gain changed too abruptly as the live dB meter fell.
- **Limiter**: catches peaks before full-scale clipping.
- **AGC**: optional slow loudness rider after the cleanup stages.

### Signal Levels
- **Target**: 30–70% (about -10 to -3 dBFS)
- **LED green**: Good level
- **LED orange**: Getting hot — consider reducing gain
- **LED red**: Clipping — reduce gain immediately
- **LED dim purple**: Very quiet — increase gain or enable AGC

### AGC vs Manual Gain
- **AGC OFF**: Consistent, predictable levels (e.g., close-range recording)
- **AGC ON**: Outdoor BirdNET-Go deployment where bird distance varies. AGC multiplies on top of your manual gain setting.

### Buffer Size Profiles

| Size | Latency | Stability | Use Case |
|------|---------|-----------|----------|
| 256 | 16ms | Low | Ultra-low latency, may drop packets |
| 512 | 32ms | Medium | Balanced for good WiFi |
| **1024** | **64ms** | **High** | **Recommended — stable streaming** |
| 2048+ | 128ms+ | Very High | Poor WiFi, maximum stability |

## Web UI Features

- IP address and WiFi signal strength
- WiFi TX power control (-1.0 to 19.5 dBm)
- Free heap memory and system uptime
- RTSP connection status and packet rate
- Realtime audio-pipeline load graph and temperature history
- Real-time signal level and clipping detection
- Audio settings (sample rate, gain, buffer, HPF, AGC)
- CPU frequency selection (80, 120, 160, 240 MHz)
- Thermal protection config (30–95°C limit)
- Auto recovery and scheduled resets
- Timestamped log viewer with copy button

## Troubleshooting

### LED is Yellow (Stuck in Startup)
- Check Serial Monitor for errors
- WiFi credentials may be incorrect
- Reset WiFi: connect to `ESP32-RTSP-Mic-AP` and reconfigure

### LED is Red (Not Streaming)
- Thermal protection triggered
- Allow to cool down, then clear latch via Web UI
- Consider lowering CPU frequency or improving ventilation

### No Audio / Low Volume
1. Check signal level in Web UI
2. Enable AGC (auto-adjusts gain)
3. Increase gain (try 5.0–10.0x)
4. Disable high-pass filter temporarily to test

### Audio Clipping / Distortion
1. Decrease gain
2. Start around 1.0x manual gain for hot PDM mics
3. Enable AGC only after manual gain is in a sane range
4. Check signal level — aim for 30–70%

### First Connection Fails
Some RTSP clients (VLC) probe the server on first connect. The second connection works immediately.

### VLC / RTSP Transport
- The firmware serves **RTP interleaved over RTSP/TCP** today; UDP transport is not implemented in this firmware.
- Many clients work best when they are explicitly told to use TCP transport.
- Prefer `ffplay -rtsp_transport tcp ...` for testing and force TCP in VLC when possible.

### Stream Drops / Connection Issues
1. Check WiFi signal strength (RSSI > -70 dBm)
2. Increase buffer size to 2048 or 4096
3. Keep gain conservative; constant clipping can sound like choppy/echoy transport failure
4. Enable auto recovery
5. Reduce WiFi TX power if causing interference

### Reachable RTSP but Nearly Silent Audio
If the client can reach `:8554` and `/api/audio_status` is updating, transport is healthy.
Focus on signal conditioning:
1. Check `peak_dbfs` in Web UI while speaking/clapping near the mic.
2. If levels stay around `-45 dBFS` to `-55 dBFS`, raise manual gain and re-test.
3. Enable AGC and verify `agc_multiplier`/`effective_gain` increase when quiet.
4. Keep HPF enabled for wind/rumble, but briefly disable once as an A/B test.
5. Aim for typical peaks around `-20 dBFS` to `-10 dBFS` without constant clipping.

## Version History

### v2.3.0
- Socket ownership model — Core 1 exclusively owns WiFiClient during streaming
- Confirmed task exit via FreeRTOS semaphore (prevents double-task creation)
- In-stream RTSP processing (TEARDOWN + keepalive on Core 1)
- LED ownership guards, log buffer spinlock, memory barriers
- Proactive WiFi disconnect handling
- Default CPU frequency lowered to 160MHz
- Copy logs button in Web UI

### v2.5.0
- Restored Arduino OTA updates for direct PlatformIO uploads over Wi‑Fi
- Added dual-slot OTA partition table and dedicated PlatformIO OTA environment

### v2.2.0
- RTSP receive buffer drain on disconnect
- Large RTSP session timeout (86400s)
- Write failure tolerance (100 consecutive failures before disconnect)
- Auto-recovery disabled by default (false positive prevention)
- NTP time sync (EST timestamps)
- Configurable LED mode (Off / Static / Level)
- Blue = ready, green = streaming LED colors
- Disconnect diagnostics (session duration, dropped packets, RSSI)

### v2.1.0
- Lock-free pointer handoff (removed mutex)
- Fixed first-connection race condition (VLC probe)
- Fixed `i2s_read` blocking, Core 1 heap contention
- mDNS discovery (`atoms3mic.local`)
- Automatic Gain Control (AGC)
- LED audio level indicator
- RTSP idle timeout (60s)
- Periodic heap monitoring
- Removed OTA (serial flash only)
- Default gain 3.0x, HPF cutoff 300Hz

### v2.0.0
- Dual-core architecture

### v1.0.0
- Initial release

## Toward better Atom Echo support

The codebase is now less branding-specific, and the API/UI expose the active hardware/filter profile. The remaining blocker for real Atom Echo compatibility is the capture backend: this firmware still assumes ESP32 **PDM RX** input, while Echo-class devices typically need a codec/I2S profile. The clean long-term direction is to split capture into pluggable frontends (PDM unit vs codec/I2S) while reusing the rest of the RTP, Web UI, and diagnostics stack.

## Remaining tapping diagnosis

There is no dedicated mic-enable GPIO being toggled in the normal capture path. If tapping still coincides with the meter dropping, suspect **I2S read gaps** or a timing/cadence problem before assuming a mute/enable pin issue. The firmware now counts fallback blocks and keeps RTP timing continuous during short read stalls so those events are visible in the UI instead of becoming silent packet gaps.
