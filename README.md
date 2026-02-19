# ESP32 RTSP Microphone for BirdNET-Go (M5Stack Atom Echo)

A high-quality RTSP audio streaming server for ESP32, specifically configured for the **M5Stack Atom Echo** with its built-in SPM1423 PDM microphone. Stream live audio to BirdNET-Go or any RTSP-compatible client.

## Hardware Requirements

### M5Stack Atom Echo
- **Board**: M5Stack Atom Echo (ESP32-PICO-D4)
- **Microphone**: SPM1423 PDM MEMS microphone (built-in)
- **Speaker**: Built-in I2S speaker (not used in this application)
- **Button**: Single programmable button (built-in)
- **LED**: RGB LED (built-in, WS2812C)

### Technical Specifications
- **Processor**: ESP32-PICO-D4 @ 240MHz Dual Core (configurable 80-240MHz)
- **WiFi**: 2.4GHz 802.11 b/g/n
- **Audio Format**: 16-bit PCM, 16kHz default (configurable 8-48kHz)
- **Streaming**: RTSP over TCP/IP on port 8554

## Architecture

### Dual-Core Design (v2.1.0)
- **Core 1**: Complete audio pipeline (I2S capture → HPF → AGC → gain → RTP → WiFi)
- **Core 0**: Web UI, RTSP negotiation, diagnostics, client management

Cores communicate via a simple pointer handoff — no mutex, no contention:
1. Core 0 handles RTSP negotiation (OPTIONS → DESCRIBE → SETUP → PLAY)
2. On PLAY, Core 0 sets `streamClient` pointer and `isStreaming` flag
3. Core 1 reads audio, processes, and sends RTP packets via the pointer
4. On disconnect, Core 1 clears the pointer; Core 0 detects and cleans up

## Pin Configuration

```cpp
I2S_BCLK_PIN    = 19  // Bit Clock
I2S_LRCLK_PIN   = 33  // Left-Right Clock / Word Select
I2S_DATA_IN_PIN = 23  // Microphone Data Input (PDM)
I2S_DATA_OUT_PIN = 22 // Speaker Data Output (not used)
```

## LED Status Indicators

LED behavior depends on the configurable LED Mode (Off / Static / Level):

| Color | Meaning |
|-------|---------|
| Yellow | System starting up |
| Blue | Ready — RTSP server running, waiting for connection |
| Green | Streaming — static mode (default) |
| Bright Green | Streaming — good audio level (30-70%) — level mode |
| Dim Green | Streaming — low audio level — level mode |
| Orange | Streaming — hot signal (>70%) — level mode |
| Dim Purple | Streaming — very quiet (<5%) — level mode |
| Red | Streaming — clipping! (or thermal protection activated) |
| Off | LED disabled via Web UI |

## Features

### Core Features
- **RTSP Streaming**: Industry-standard RTSP/RTP streaming on port 8554
- **mDNS Discovery**: Access via `rtsp://atomecho.local:8554/audio` — no IP needed
- **Web Interface**: Full-featured web UI for configuration and monitoring
- **PDM Support**: Native support for PDM MEMS microphones
- **Auto Recovery**: Automatic recovery from sustained streaming issues (3 consecutive failures required, 2-min cooldown)
- **Persistent Settings**: Configuration saved to flash memory
- **Dual-Core**: Audio pipeline on Core 1, everything else on Core 0
- **NTP Time Sync**: Real timestamps in logs (EST, synced at boot via pool.ntp.org)
- **Configurable LED**: Off / Static / Level indicator modes via Web UI

### Audio Processing
- **Automatic Gain Control (AGC)**: Auto-adjusts volume for varying distances — ideal for outdoor bird recording. Fast attack prevents clipping, slow release boosts quiet signals.
- **High-Pass Filter**: Configurable 2nd-order Butterworth filter (default 300Hz) to remove wind/traffic rumble
- **Digital Gain**: Software gain control (0.1x to 100x, default 3.0x for BirdNET-Go)
- **Clipping Detection**: Real-time audio level monitoring and clipping alerts
- **Multiple Sample Rates**: 8kHz to 96kHz (16kHz recommended for Atom Echo)

### Performance & Reliability
- **Configurable CPU Frequency**: 80MHz, 120MHz, 160MHz, or 240MHz
- **WiFi Power Control**: Adjustable TX power to reduce RF noise
- **Buffer Profiles**: Multiple latency/stability profiles (256 to 8192 samples)
- **Thermal Protection**: Automatic shutdown on overheating (configurable 30-95C)
- **Connection Resilience**: Write failure tolerance (30 consecutive failures before disconnect), receive buffer drain for keepalives, large session timeout — long-lived stable streams
- **Smart Auto Recovery**: Requires 3 consecutive low-rate checks before restarting, with 2-minute cooldown to prevent restart loops
- **RTSP Idle Timeout**: Auto-disconnects clients that connect but never stream (60s)
- **Disconnect Diagnostics**: Logs session duration, dropped packets, and WiFi RSSI on every disconnect
- **Scheduled Resets**: Optional periodic reboots for long-term stability
- **Heap Monitoring**: Periodic heap logging for detecting leaks in long deployments
- **Timestamped Logs**: NTP-synced timestamps (EST) on all log messages for easier debugging

## Getting Started

### 1. Hardware Setup
1. Connect M5Stack Atom Echo via USB-C
2. No additional wiring required — microphone is built-in

### 2. Flash Firmware
```bash
pio run --target upload
```

### 3. Initial Configuration

#### WiFi Setup
On first boot, the device creates a WiFi access point:
- **SSID**: `ESP32-RTSP-Mic-AP`
- **Portal**: Opens automatically when you connect
- Select your WiFi network and enter credentials
- Device will reboot and connect to your network

#### Finding Your Device
The LED turns **blue** when ready. Check Serial Monitor (115200 baud) for:
```
WiFi connected: 192.168.1.XXX
mDNS: atomecho.local
RTSP URL: rtsp://192.168.1.XXX:8554/audio
RTSP URL: rtsp://atomecho.local:8554/audio
Web UI: http://192.168.1.XXX/
```

## Usage

### RTSP Stream
```
rtsp://atomecho.local:8554/audio
rtsp://[device-ip]:8554/audio
```

**Audio Format:**
- Codec: L16 (16-bit PCM)
- Sample Rate: 16000 Hz (default, configurable)
- Channels: Mono
- Transport: RTP over RTSP/TCP

### Testing with VLC
```bash
vlc rtsp://atomecho.local:8554/audio
```

### Testing with ffplay
```bash
ffplay -rtsp_transport tcp rtsp://atomecho.local:8554/audio
```

### BirdNET-Go Integration
Set audio source to: `rtsp://atomecho.local:8554/audio`

Recommended settings for BirdNET-Go:
- Sample rate: 16000 Hz
- Gain: 3.0x (default) — adjust based on environment
- AGC: ON — auto-adjusts for varying bird distances
- High-pass filter: ON, 300 Hz — reduces wind/traffic while keeping bird calls
- Buffer: 1024 samples

## Audio Tuning Guide

### Recommended Settings (BirdNET-Go)
| Setting | Value | Notes |
|---------|-------|-------|
| Sample Rate | 16000 Hz | Optimal for PDM on Atom Echo |
| Gain | 3.0x | Good starting point for outdoor use |
| AGC | ON | Auto-adjusts for near/far birds |
| High-Pass | ON, 300 Hz | Removes rumble, keeps most bird calls |
| Buffer | 1024 samples | 64ms latency, good stability |
| I2S Shift | 0 bits | Fixed for PDM — do not change |

### Signal Level Guide
- **Target**: 30-70% (about -10 to -3 dBFS)
- **LED green**: Good level
- **LED orange**: Getting hot — consider reducing gain
- **LED red**: Clipping — reduce gain immediately
- **LED dim purple**: Very quiet — increase gain or enable AGC

### AGC vs Manual Gain
- **AGC OFF**: Use when you want consistent, predictable levels (e.g., close-range recording)
- **AGC ON**: Use for outdoor BirdNET-Go deployment where bird distance varies. AGC multiplies on top of your manual gain setting.

### Buffer Size Profiles

| Size | Latency | Stability | Use Case |
|------|---------|-----------|----------|
| 256 | 16ms | Low | Ultra-low latency, may drop packets |
| 512 | 32ms | Medium | Balanced for good WiFi |
| **1024** | **64ms** | **High** | **Recommended — stable streaming** |
| 2048+ | 128ms+ | Very High | Poor WiFi, maximum stability |

## Troubleshooting

### LED is Yellow (Stuck in Startup)
- Check Serial Monitor for errors
- WiFi credentials may be incorrect
- Reset WiFi: Connect to `ESP32-RTSP-Mic-AP` and reconfigure

### LED is Red (Not Streaming)
- Thermal protection triggered
- Allow to cool down, then clear latch via Web UI
- Consider lowering CPU frequency or improving ventilation

### No Audio / Low Volume
1. Check signal level in Web UI
2. Enable AGC (auto-adjusts gain)
3. Increase Gain (try 5.0-10.0x)
4. Disable High-Pass filter temporarily to test

### Audio Clipping / Distortion
1. Decrease Gain
2. Enable AGC (it has fast attack to prevent clipping)
3. Check "Signal Level" — aim for 30-70%

### First Connection Fails (Fixed in v2.1.0)
Some RTSP clients (VLC) probe the server on first connect. v2.1.0 handles this gracefully — the second connection works immediately if the first is a probe.

### Stream Drops / Connection Issues
1. Check WiFi signal strength (RSSI > -70 dBm)
2. Increase buffer size to 2048 or 4096
3. Enable Auto Recovery
4. Reduce WiFi TX power if causing interference

## Development

### Building from Source
```bash
# Build
pio run

# Upload (serial)
pio run --target upload

# Monitor
pio device monitor -b 115200
```

### Dependencies
```ini
lib_deps =
    tzapu/WiFiManager @ ^2.0.17
    m5stack/M5Atom @ ^0.1.3
    fastled/FastLED @ ^3.10.3
```

### Key Files
- `src/esp32_rtsp_mic_birdnetgo.ino` — Main firmware (dual-core audio, RTSP, I2S)
- `src/WebUI.cpp` — Web interface (HTML/JS/CSS, API endpoints)
- `src/WebUI.h` — WebUI header
- `CLAUDE.md` — Detailed development notes and architecture docs

## Version History

### v2.2.0
- Added **RTSP receive buffer drain** — drains stale keepalive data on disconnect, keeping the TCP window clean
- Added **large RTSP session timeout** (86400s) — reduces ffmpeg keepalive frequency from every 30s to every ~12 hours
- Added **write failure tolerance** — 30 consecutive failures (~2s) before disconnecting, survives brief WiFi hiccups
- Fixed **auto-recovery restart loop** — requires 3 consecutive failures with 2-minute cooldown
- Lowered auto-recovery threshold from 70% to 50% of expected packet rate
- Added **NTP time sync** — real timestamps (EST) in all log messages
- Added **configurable LED mode** — Off / Static / Level via Web UI
- Changed LED colors: **blue** = ready, **green** = streaming, level mode uses green/orange/red
- Added **disconnect diagnostics** — logs session duration, dropped packets, and RSSI on disconnect

### v2.1.0
- Removed mutex — replaced with lock-free pointer handoff between cores
- Fixed first-connection race condition (VLC probe behavior)
- Fixed `i2s_read` blocking task exit (`portMAX_DELAY` → 100ms timeout)
- Fixed Core 1 heap contention (no more `String` allocation on audio core)
- Fixed `restartI2S()` and `checkTemperature()` not clearing `streamClient`
- Added **mDNS** discovery (`atomecho.local`)
- Added **Automatic Gain Control** (AGC) with fast attack / slow release
- Added **LED audio level indicator** (color-coded signal strength while streaming)
- Added **RTSP idle timeout** (60s — disconnects stale clients)
- Added **periodic heap monitoring** (every 10 min, useful for long deployments)
- Removed OTA (serial flash only)
- Default gain increased to 3.0x (better for outdoor bird recording)
- Default HPF cutoff lowered to 300 Hz (preserves more bird calls)
- Default buffer restored to 1024 samples
- RTSP parse buffer restored to 1024 bytes

### v2.0.0
- Dual-core architecture (Core 1 audio, Core 0 everything else)
- Mutex-based client sharing between cores

### v1.0.0
- Initial release with single-core audio pipeline

## License

[Add your license information here]

## Acknowledgments

- M5Stack for the Atom Echo hardware
- BirdNET-Go community
- Original RTSP microphone implementation
