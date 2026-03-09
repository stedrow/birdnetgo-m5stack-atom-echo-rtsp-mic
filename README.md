# M5Stack Atom Echo — RTSP Microphone for BirdNET-Go

A high-quality RTSP audio streaming server for the **M5Stack Atom Echo**, streaming live audio to [BirdNET-Go](https://github.com/tphakala/birdnet-go) or any RTSP-compatible client.

<p align="left">
  <img src="https://shop.m5stack.com/cdn/shop/files/3_e4ea519e-765f-4f30-aad1-7855ff9f8744_1200x1200.jpg" alt="M5Stack Atom Echo" width="300">
</p>

**Buy**: [M5Stack Store](https://shop.m5stack.com/products/atom-echo-smart-speaker-dev-kit) | [Amazon](https://www.amazon.com/M5Stack-Atom-Echo-Smart-Speaker/dp/B0C7QSVPB2)

## Features

- **Dual-core architecture** — Core 1 handles full audio pipeline, Core 0 handles Web UI and RTSP negotiation
- **mDNS discovery** — `atomecho.local`, no IP needed
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
vlc rtsp://atomecho.local:8554/audio
# or
ffplay -rtsp_transport tcp rtsp://atomecho.local:8554/audio
```

**BirdNET-Go**: set audio source to `rtsp://atomecho.local:8554/audio`

**Web UI**: `http://atomecho.local/`

## Recommended Settings

| Setting | Default | Notes |
|---------|---------|-------|
| Sample Rate | 16000 Hz | Optimal for PDM on Atom Echo |
| Gain | 3.0x | Good for outdoor use |
| AGC | OFF | Enable for varying bird distances |
| High-Pass | ON, 300 Hz | Removes rumble, keeps bird calls |
| Buffer | 1024 samples | 64ms latency, stable streaming |
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
    m5stack/M5Atom @ ^0.1.3
    fastled/FastLED @ ^3.10.3
```

## Documentation

- [Architecture & Troubleshooting Guide](docs/DETAILS.md) — dual-core design, audio tuning, troubleshooting, version history

## Acknowledgments

This project is largely based on [birdnetgo-esp32-rtsp-mic](https://github.com/Sukecz/birdnetgo-esp32-rtsp-mic) by [@Sukecz](https://github.com/Sukecz) — thank you for the excellent foundation!

- M5Stack for the Atom Echo hardware
- [BirdNET-Go](https://github.com/tphakala/birdnet-go) community
