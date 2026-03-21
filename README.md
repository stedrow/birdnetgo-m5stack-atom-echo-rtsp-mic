# AtomS3 Lite + Unit Mini PDM — RTSP Microphone for BirdNET-Go

A high-quality RTSP audio streaming server for the **AtomS3 Lite + Unit Mini PDM**, streaming live audio to [BirdNET-Go](https://github.com/tphakala/birdnet-go) or any RTSP-compatible client.

<p align="left">
  <img src="https://shop.m5stack.com/cdn/shop/files/3_e4ea519e-765f-4f30-aad1-7855ff9f8744_1200x1200.jpg" alt="M5Stack Atom Echo" width="300">
</p>

**Buy**: [M5Stack Store](https://shop.m5stack.com/products/atom-echo-smart-speaker-dev-kit) | [Amazon](https://www.amazon.com/M5Stack-Atom-Echo-Smart-Speaker/dp/B0C7QSVPB2)

## Features

- **Dual-core architecture** — Core 1 handles full audio pipeline, Core 0 handles Web UI and RTSP negotiation
- **mDNS discovery** — `atoms3mic.local` (default; use the hostname shown in boot logs if customized), no IP needed
- **Web UI** — configure settings, view signal levels, logs, and diagnostics
- **AGC** — automatic gain control for varying bird distances
- **High-pass filter** — 2nd-order Butterworth (default 300Hz) removes wind/traffic
- **Thermal protection** — configurable auto-shutdown on overheating
- **LED indicator** — Off / Static / Level modes
- **WiFiManager** — captive portal for initial WiFi setup
- **Persistent settings** — saved to flash

## Quick Start

### 1. Flash
```bash
pio run --target upload
```

### 2. Connect to WiFi
On first boot, connect to the `ESP32-RTSP-Mic-AP` access point and configure your WiFi. The LED turns **blue** when ready.

### 3. Stream
```bash
vlc rtsp://atoms3mic.local:8554/audio
# or
ffplay -rtsp_transport tcp rtsp://atoms3mic.local:8554/audio
```

This firmware currently supports **RTSP interleaved over TCP** only. If VLC is used, force TCP transport in the client settings or CLI.

**BirdNET-Go**: set audio source to `rtsp://atoms3mic.local:8554/audio`

**Web UI**: `http://atoms3mic.local/`

**Browser streamer page**: `http://atoms3mic.local/streamer`

## Recommended Settings

| Setting | Default | Notes |
|---------|---------|-------|
| Sample Rate | 16000 Hz | Optimal for Unit Mini PDM |
| Gain | 1.0x | Conservative baseline to keep the PDM capsule out of constant hiss/clipping |
| AGC | OFF | Enable for varying bird distances |
| High-Pass | ON, 450 Hz | Reduces rumble and low-frequency room boom |
| Buffer | 2048 samples | 128ms latency, more tolerant of VLC / weak Wi‑Fi |
| CPU | 160 MHz | Sufficient, reduces heat |
| I2S Shift | 0 bits | Fixed for PDM — do not change |

## LED Status

| Color | Meaning |
|-------|---------|
| Yellow | Starting up |
| Blue | Ready, waiting for connection |
| Green | Streaming |
| Orange | Signal hot, >70% (level mode) |
| Red | Clipping or thermal protection |

## Mic Link Diagnostic (no audio case)

If the stream is silent, you can verify whether the ESP32 is actually receiving changing samples from the PDM mic. These counters are updated continuously after boot (you do not need an active RTSP PLAY session).

```bash
curl -s http://atoms3mic.local/api/audio_status
```

Check these fields in the JSON:
- `i2s_reads_ok`: should increase steadily; if it stays `0`, firmware is not capturing samples yet (or running an older build).
- `i2s_link_ok`: `true` means raw samples are changing.
- `i2s_raw_peak` / `i2s_raw_rms`: should be above near-zero when speaking near the mic.
- `i2s_raw_min` and `i2s_raw_max`: should not be identical for long.
- `i2s_raw_zero_pct`: very high values (e.g. ~100%) suggest no real data.
- `i2s_hint`: quick wiring hint if signal looks flat.
- If VLC shows `No route to host`, this is a network path issue (not microphone capture): verify your computer is on the same subnet as the device IP, disable client/AP isolation on the Wi‑Fi, and confirm `rtsp://<device-ip>:8554/audio` is reachable from the same VLAN.

Quick network-layer checks from the same client machine:

```bash
ping -c 3 <device-ip>
ip route get <device-ip>
nc -vz <device-ip> 8554
curl -s http://<device-ip>/api/audio_status
```

Expected behavior:
- `ping` should return replies.
- `ip route get` should show the expected interface/subnet (no unexpected VPN route).
- `nc` should report port 8554 reachable.
- `audio_status` should return JSON with increasing `i2s_reads_ok`.

If `ping` fails or `nc` says `No route to host`, fix routing/VLAN/AP isolation/firewall first. RTSP negotiation cannot start until plain TCP connectivity to `<device-ip>:8554` works.

If all network checks pass but audio still sounds nearly silent:
- Use the Web UI meter (`peak_dbfs`) while speaking/clapping near the mic.
- Around `-45 dBFS` to `-55 dBFS` with visible sample movement means capture is alive but too quiet for practical decoding.
- Increase manual gain first (for example 1.0x → 2.0x → 4.0x) and re-check `peak_dbfs`.
- Then enable AGC so distant calls are lifted automatically; watch `agc_multiplier` and `effective_gain` in `/api/status`.
- Keep high-pass filter enabled for outdoor noise, but temporarily disable HPF once to compare whether calls become more audible.
- Target typical speech/claps around `-20 dBFS` to `-10 dBFS` without frequent clipping.

If the meter is frequently red / clipping:
- Drop gain back toward `1.0x`.
- Keep HPF enabled and try 450–600 Hz to tame room boom / handling noise.
- Use a larger buffer (2048 or 4096) before chasing Wi‑Fi issues, because underruns can sound choppy or “echoy”.

Interpretation tip for raw counters:
- A non-zero `i2s_raw_rms` with close `i2s_raw_min`/`i2s_raw_max` can indicate strong DC bias and low AC amplitude.
- This is a signal-conditioning issue (gain/AGC/HPF tuning), not a transport/network failure.

For Unit PDM wiring use **CLK=G1** and **DATA=G2**, plus GND and 3V3.

## Building

```bash
pio run                      # Build
pio run --target upload      # Flash
pio device monitor -b 115200 # Serial monitor
```

### Dependencies
```ini
lib_deps =
    tzapu/WiFiManager @ ^2.0.17
    fastled/FastLED @ ^3.10.3
```

## Documentation

- [Architecture & Troubleshooting Guide](docs/DETAILS.md) — dual-core design, audio tuning, troubleshooting, version history

## Acknowledgments

This project is largely based on [birdnetgo-esp32-rtsp-mic](https://github.com/Sukecz/birdnetgo-esp32-rtsp-mic) by [@Sukecz](https://github.com/Sukecz) — thank you for the excellent foundation!

- M5Stack for AtomS3 Lite and Unit Mini PDM hardware
- [BirdNET-Go](https://github.com/tphakala/birdnet-go) community
