#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include "driver/i2s.h"
#include <Preferences.h>
#include <math.h>
#include <FastLED.h>
#include "WebUI.h"

// ================== DUAL-CORE AUDIO ARCHITECTURE ==================
// Core 1: Complete audio pipeline (I2S → process → RTP → WiFi)
// Core 0: Web UI, diagnostics, RTSP protocol, client management
// PDM microphone outputs 16-bit samples directly

// Pointer handoff: Core 0 sets on PLAY, Core 1 uses for streaming, clears on failure
WiFiClient* volatile streamClient = NULL;
TaskHandle_t audioCaptureTaskHandle = NULL;
volatile bool audioTaskRunning = false;

// Cross-core synchronization primitives
portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;  // spinlock for log ring buffer
volatile bool stopStreamRequested = false;   // Core 0 asks Core 1 to stop
volatile bool streamCleanupDone = false;     // Core 1 confirms cleanup complete
SemaphoreHandle_t taskExitSemaphore = NULL;  // confirmed task exit
volatile bool core1OwnsLED = false;          // LED ownership flag

// ================== SETTINGS (ESP32 RTSP Mic for BirdNET-Go) ==================
#define FW_VERSION "2.6.0"
// Expose FW version as a global C string for WebUI/API
const char* FW_VERSION_STR = FW_VERSION;
static const uint16_t OTA_PORT = 3232;

// -- DEFAULT PARAMETERS (configurable via Web UI / API)
#define DEFAULT_SAMPLE_RATE 16000  // Unit Mini PDM / BirdNET-Go preferred rate
#define DEFAULT_GAIN_FACTOR 1.0f
#define DEFAULT_BUFFER_SIZE 1024   // 64ms @ 16kHz - lower default latency while keeping WiFi headroom
#define DEFAULT_WIFI_TX_DBM 19.5f  // Default WiFi TX power in dBm
#define DEFAULT_NETWORK_HOSTNAME "atoms3mic"
// High-pass filter defaults (to remove low-frequency rumble)
#define DEFAULT_HPF_ENABLED true
#define DEFAULT_HPF_CUTOFF_HZ 450

// Thermal protection defaults
#define DEFAULT_OVERHEAT_PROTECTION true
#define DEFAULT_OVERHEAT_LIMIT_C 80
#define OVERHEAT_MIN_LIMIT_C 30
#define OVERHEAT_MAX_LIMIT_C 95
#define OVERHEAT_LIMIT_STEP_C 5

// -- Pins (AtomS3 Lite + Unit Mini PDM)
// M5's reference wiring for the PDM Unit is CLK=G1 and DATA=G2.
#define I2S_CLK_PIN      1  // PDM CLK (G1)
#define I2S_DATA_IN_PIN  2  // PDM DATA (G2)
#define WS2812_LED_PIN  35  // AtomS3 Lite built-in RGB LED

static CRGB statusLed[1];

inline void setStatusLed(const CRGB& color) {
    statusLed[0] = color;
    FastLED.show();
}

// -- Servers
WiFiServer rtspServer(8554);
WiFiClient rtspClient;
String networkHostname = DEFAULT_NETWORK_HOSTNAME;
bool otaPreviousRtspEnabled = true;

// -- RTSP Streaming
String rtspSessionId = "";
volatile bool isStreaming = false;
uint16_t rtpSequence = 0;
uint32_t rtpTimestamp = 0;
uint32_t rtpSSRC = 0x43215678;
unsigned long lastRTSPActivity = 0;

// -- Buffers
uint8_t rtspParseBuffer[1024];
int rtspParseBufferPos = 0;
// Note: Audio buffers now allocated by Core 1 task

// -- Global state
unsigned long audioPacketsSent = 0;
unsigned long audioPacketsDropped = 0;  // Track dropped frames
unsigned long lastStatsReset = 0;
bool rtspServerEnabled = true;

// -- Audio parameters (runtime configurable)
uint32_t currentSampleRate = DEFAULT_SAMPLE_RATE;
float currentGainFactor = DEFAULT_GAIN_FACTOR;
uint16_t currentBufferSize = DEFAULT_BUFFER_SIZE;
// PDM microphones output decimated samples directly
// The hardware PDM decimation produces samples already close to correct range
// Typical range: 0-3 for PDM vs 11-13 for I2S microphones
uint8_t i2sShiftBits = 0;  // Fixed for Unit Mini PDM capture

// -- Audio metering / clipping diagnostics
uint16_t lastPeakAbs16 = 0;       // last block peak absolute value (0..32767)
uint32_t audioClipCount = 0;      // total blocks where clipping occurred
bool audioClippedLastBlock = false; // clipping occurred in last processed block
uint16_t peakHoldAbs16 = 0;       // peak hold (recent window)
unsigned long peakHoldUntilMs = 0; // when to clear hold

// -- I2S raw capture diagnostics (helps verify PDM communication/wiring)
volatile uint32_t i2sReadOkCount = 0;
volatile uint32_t i2sReadErrCount = 0;
volatile uint32_t i2sReadZeroCount = 0;
volatile uint16_t i2sLastSamplesRead = 0;
volatile int16_t i2sLastRawMin = 0;
volatile int16_t i2sLastRawMax = 0;
volatile uint16_t i2sLastRawPeakAbs = 0;
volatile uint16_t i2sLastRawRms = 0;
volatile uint16_t i2sLastRawZeroPct = 0;
volatile bool i2sLikelyUnsignedPcm = false;
volatile uint32_t audioFallbackBlockCount = 0;
volatile uint32_t i2sLastGapMs = 0;
volatile float audioPipelineLoadPct = 0.0f;

// -- Lightweight spectrum diagnostics for Web UI waterfall
volatile uint8_t fftBins[32] = {0};
volatile uint32_t fftFrameSeq = 0;

// -- Browser WebAudio diagnostics stream (latest processed PCM block)
portMUX_TYPE webAudioMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t webAudioFrameSeq = 0;
volatile uint16_t webAudioFrameSamples = 0;
volatile uint32_t webAudioSampleRate = DEFAULT_SAMPLE_RATE;
static const uint16_t WEB_AUDIO_MAX_SAMPLES = 2048;
int16_t webAudioFrame[WEB_AUDIO_MAX_SAMPLES] = {0};

// -- Lightweight telemetry history for Web UI graphs
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;
static const uint16_t TELEMETRY_HISTORY_LEN = 120;
volatile uint32_t telemetryHistorySeq = 0;
uint8_t telemetryCpuLoadPct[TELEMETRY_HISTORY_LEN] = {0};
int16_t telemetryTempDeciC[TELEMETRY_HISTORY_LEN] = {0};
uint16_t telemetryHistoryHead = 0;
uint16_t telemetryHistoryCount = 0;

// -- LED mode: 0=off, 1=static, 2=level
uint8_t ledMode = 1;  // Default: static purple during streaming

// -- Automatic Gain Control (AGC)
bool agcEnabled = false;
volatile float agcMultiplier = 1.0f;   // Current AGC multiplier (read by WebUI)
const float AGC_TARGET_RMS = 0.10f;    // Conservative target RMS (~-20 dBFS) to avoid constant hot audio
const float AGC_MIN_MULT = 0.1f;
const float AGC_MAX_MULT = 6.0f;       // Keep AGC from driving persistent background noise too hard
const float AGC_ATTACK_RATE = 0.05f;   // Fast attack (gain reduction) per buffer
const float AGC_RELEASE_RATE = 0.001f; // Slow release (gain increase) per buffer

// -- Adaptive background-noise filter (auto gate/bed suppression)
// Tuned to avoid the old "tap tap tap" artifact caused by per-sample gain pumping.
volatile bool noiseFilterEnabled = true;
volatile float noiseFloorDbfs = -90.0f;
volatile float noiseGateDbfs = -90.0f;
volatile float noiseReductionDb = 0.0f;
const float NOISE_FLOOR_INIT = 300.0f;
const float NOISE_FLOOR_FAST = 0.015f;
const float NOISE_FLOOR_SLOW = 0.0008f;
const float NOISE_GATE_RATIO = 2.6f;
const float NOISE_GATE_MARGIN = 220.0f;
const float NOISE_GATE_MIN = 220.0f;
const float NOISE_GATE_MAX = 5200.0f;
const float NOISE_GATE_CLOSE_RATIO = 0.72f;
const float NOISE_FILTER_MIN_GAIN = 0.55f;
const float NOISE_FILTER_ATTACK = 0.006f;
const float NOISE_FILTER_RELEASE = 0.0008f;
const float NOISE_ENV_ATTACK = 0.010f;
const float NOISE_ENV_RELEASE = 0.0012f;
const uint16_t NOISE_GATE_HOLD_MS = 120;
const char* DEVICE_TITLE = "M5 Atom RTSP Microphone";
const char* DEVICE_INPUT_PROFILE = "PDM input profile (AtomS3 Lite + Unit Mini PDM tuned)";
const char* FILTER_CHAIN_BASE = "Input normalize -> 2nd-order Butterworth high-pass -> adaptive noise bed suppressor -> manual gain -> limiter -> optional AGC";

// -- High-pass filter (biquad) to cut low-frequency rumble
struct Biquad {
    float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
    float x1{0.0f}, x2{0.0f}, y1{0.0f}, y2{0.0f};
    inline float process(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
    inline void reset() { x1 = x2 = y1 = y2 = 0.0f; }
};
bool highpassEnabled = DEFAULT_HPF_ENABLED;
uint16_t highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
Biquad hpf;
uint32_t hpfConfigSampleRate = 0;
uint16_t hpfConfigCutoff = 0;

// -- Preferences for persistent settings
Preferences audioPrefs;

// -- Diagnostics, auto-recovery and temperature monitoring
unsigned long lastMemoryCheck = 0;
unsigned long lastPerformanceCheck = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastTempCheck = 0;
unsigned long lastTelemetrySampleMs = 0;
uint32_t minFreeHeap = 0xFFFFFFFF;
uint32_t maxPacketRate = 0;
uint32_t minPacketRate = 0xFFFFFFFF;
bool autoRecoveryEnabled = false;
bool autoThresholdEnabled = true; // auto compute minAcceptableRate from sample rate and buffer size
// Deferred reboot scheduling (to restart safely outside HTTP context)
volatile bool scheduledFactoryReset = false;
volatile unsigned long scheduledRebootAt = 0;
unsigned long bootTime = 0;
unsigned long lastI2SReset = 0;
float maxTemperature = 0.0f;
float lastTemperatureC = 0.0f;
bool lastTemperatureValid = false;
bool overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
float overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
bool overheatLockoutActive = false;
float overheatTripTemp = 0.0f;
unsigned long overheatTriggeredAt = 0;
String overheatLastReason = "";
String overheatLastTimestamp = "";
bool overheatSensorFault = false;
bool overheatLatched = false;

// -- Scheduled reset
bool scheduledResetEnabled = false;
uint32_t resetIntervalHours = 24; // Default 24 hours

// -- Configurable thresholds
uint32_t minAcceptableRate = 50;        // Minimum acceptable packet rate (restart below this)
uint32_t performanceCheckInterval = 15; // Check interval in minutes
uint8_t cpuFrequencyMhz = 160;          // CPU frequency (default 160 MHz — sufficient for this workload)

// -- WiFi TX power (configurable)
float wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
wifi_power_t currentWifiPowerLevel = WIFI_POWER_19_5dBm;

// -- RTSP connect/PLAY statistics
unsigned long lastRtspClientConnectMs = 0;
unsigned long lastRtspPlayMs = 0;
uint32_t rtspConnectCount = 0;
uint32_t rtspPlayCount = 0;

// ===============================================

// Helper: convert WiFi power enum to dBm (for logs)
float wifiPowerLevelToDbm(wifi_power_t lvl) {
    switch (lvl) {
        case WIFI_POWER_19_5dBm:    return 19.5f;
        case WIFI_POWER_19dBm:      return 19.0f;
        case WIFI_POWER_18_5dBm:    return 18.5f;
        case WIFI_POWER_17dBm:      return 17.0f;
        case WIFI_POWER_15dBm:      return 15.0f;
        case WIFI_POWER_13dBm:      return 13.0f;
        case WIFI_POWER_11dBm:      return 11.0f;
        case WIFI_POWER_8_5dBm:     return 8.5f;
        case WIFI_POWER_7dBm:       return 7.0f;
        case WIFI_POWER_5dBm:       return 5.0f;
        case WIFI_POWER_2dBm:       return 2.0f;
        case WIFI_POWER_MINUS_1dBm: return -1.0f;
        default:                    return 19.5f;
    }
}

// Helper: pick the highest power level not exceeding requested dBm
static wifi_power_t pickWifiPowerLevel(float dbm) {
    if (dbm <= -1.0f) return WIFI_POWER_MINUS_1dBm;
    if (dbm <= 2.0f)  return WIFI_POWER_2dBm;
    if (dbm <= 5.0f)  return WIFI_POWER_5dBm;
    if (dbm <= 7.0f)  return WIFI_POWER_7dBm;
    if (dbm <= 8.5f)  return WIFI_POWER_8_5dBm;
    if (dbm <= 11.0f) return WIFI_POWER_11dBm;
    if (dbm <= 13.0f) return WIFI_POWER_13dBm;
    if (dbm <= 15.0f) return WIFI_POWER_15dBm;
    if (dbm <= 17.0f) return WIFI_POWER_17dBm;
    if (dbm <= 18.5f) return WIFI_POWER_18_5dBm;
    if (dbm <= 19.0f) return WIFI_POWER_19dBm;
    return WIFI_POWER_19_5dBm;
}

// Apply WiFi TX power
// Logs only when changed; can be muted with log=false
void applyWifiTxPower(bool log = true) {
    wifi_power_t desired = pickWifiPowerLevel(wifiTxPowerDbm);
    if (desired != currentWifiPowerLevel) {
        WiFi.setTxPower(desired);
        currentWifiPowerLevel = desired;
        if (log) {
            simplePrintln("WiFi TX power set to " + String(wifiPowerLevelToDbm(currentWifiPowerLevel), 1) + " dBm");
        }
    }
}

String describeHardwareProfile() {
    return String(DEVICE_TITLE) + " / " + DEVICE_INPUT_PROFILE;
}

String describeFilterChain() {
    String chain = FILTER_CHAIN_BASE;
    chain += highpassEnabled
        ? (String(" (HPF ON @ ") + String(highpassCutoffHz) + " Hz)")
        : String(" (HPF bypassed)");
    chain += noiseFilterEnabled
        ? String(", noise suppressor active")
        : String(", noise suppressor bypassed");
    chain += agcEnabled
        ? String(", AGC active")
        : String(", AGC bypassed");
    return chain;
}

static void fillConcealmentBlock(int16_t* buffer, uint16_t samples, int16_t lastSample) {
    if (!buffer || samples == 0) return;
    for (uint16_t i = 0; i < samples; ++i) {
        float remain = 1.0f - ((float)(i + 1) / (float)samples);
        if (remain < 0.0f) remain = 0.0f;
        buffer[i] = (int16_t)((float)lastSample * remain);
    }
}

void recordTelemetrySample(float cpuLoadPct, float tempC, bool tempValid) {
    if (cpuLoadPct < 0.0f) cpuLoadPct = 0.0f;
    if (cpuLoadPct > 100.0f) cpuLoadPct = 100.0f;
    int16_t tempDeci = tempValid ? (int16_t)lroundf(tempC * 10.0f) : INT16_MIN;

    portENTER_CRITICAL(&telemetryMux);
    telemetryCpuLoadPct[telemetryHistoryHead] = (uint8_t)lroundf(cpuLoadPct);
    telemetryTempDeciC[telemetryHistoryHead] = tempDeci;
    telemetryHistoryHead = (telemetryHistoryHead + 1) % TELEMETRY_HISTORY_LEN;
    if (telemetryHistoryCount < TELEMETRY_HISTORY_LEN) telemetryHistoryCount++;
    telemetryHistorySeq++;
    portEXIT_CRITICAL(&telemetryMux);
}

// Recompute HPF coefficients (2nd-order Butterworth high-pass)
void updateHighpassCoeffs() {
    if (!highpassEnabled) {
        hpf.reset();
        hpfConfigSampleRate = currentSampleRate;
        hpfConfigCutoff = highpassCutoffHz;
        return;
    }
    float fs = (float)currentSampleRate;
    float fc = (float)highpassCutoffHz;
    if (fc < 10.0f) fc = 10.0f;
    if (fc > fs * 0.45f) fc = fs * 0.45f; // keep reasonable

    const float pi = 3.14159265358979323846f;
    float w0 = 2.0f * pi * (fc / fs);
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float Q = 0.70710678f; // Butterworth-like
    float alpha = sinw0 / (2.0f * Q);

    float b0 =  (1.0f + cosw0) * 0.5f;
    float b1 = -(1.0f + cosw0);
    float b2 =  (1.0f + cosw0) * 0.5f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    hpf.b0 = b0 / a0;
    hpf.b1 = b1 / a0;
    hpf.b2 = b2 / a0;
    hpf.a1 = a1 / a0;
    hpf.a2 = a2 / a0;
    hpf.reset();

    hpfConfigSampleRate = currentSampleRate;
    hpfConfigCutoff = (uint16_t)fc;
}

// Uptime -> "Xd Yh Zm Ts"
String formatUptime(unsigned long seconds) {
    unsigned long days = seconds / 86400;
    seconds %= 86400;
    unsigned long hours = seconds / 3600;
    seconds %= 3600;
    unsigned long minutes = seconds / 60;
    seconds %= 60;

    String result = "";
    if (days > 0) result += String(days) + "d ";
    if (hours > 0 || days > 0) result += String(hours) + "h ";
    if (minutes > 0 || hours > 0 || days > 0) result += String(minutes) + "m ";
    result += String(seconds) + "s";
    return result;
}

// Format "X ago" for events based on millis()
String formatSince(unsigned long eventMs) {
    if (eventMs == 0) return String("never");
    unsigned long seconds = (millis() - eventMs) / 1000;
    return formatUptime(seconds) + " ago";
}

static bool isTemperatureValid(float temp) {
    if (isnan(temp) || isinf(temp)) return false;
    if (temp < -20.0f || temp > 130.0f) return false;
    return true;
}

// Format current local time, fallback to uptime when no RTC/NTP time available
static void persistOverheatNote() {
    audioPrefs.begin("audio", false);
    audioPrefs.putString("ohReason", overheatLastReason);
    audioPrefs.putString("ohStamp", overheatLastTimestamp);
    audioPrefs.putFloat("ohTripC", overheatTripTemp);
    audioPrefs.putBool("ohLatched", overheatLatched);
    audioPrefs.end();
}

void recordOverheatTrip(float temp) {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    overheatTripTemp = temp;
    overheatTriggeredAt = millis();
    overheatLastTimestamp = formatUptime(uptimeSeconds);
    overheatLastReason = String("Thermal shutdown: ") + String(temp, 1) + " C reached (limit " +
                         String(overheatShutdownC, 1) + " C). Stream disabled; acknowledge in UI.";
    overheatLatched = true;
    simplePrintln("THERMAL PROTECTION: " + overheatLastReason);
    simplePrintln("TIP: Improve cooling or lower WiFi TX power/CPU MHz if overheating persists.");
    persistOverheatNote();
}

// Temperature monitoring + thermal protection
void checkTemperature() {
    float temp = temperatureRead(); // ESP32 internal sensor (approximate)
    bool tempValid = isTemperatureValid(temp);
    if (!tempValid) {
        lastTemperatureValid = false;
        if (!overheatSensorFault) {
            overheatSensorFault = true;
            overheatLastReason = "Thermal protection disabled: temperature sensor unavailable.";
            overheatLastTimestamp = "";
            overheatTripTemp = 0.0f;
            overheatTriggeredAt = 0;
            persistOverheatNote();
            simplePrintln("WARNING: Temperature sensor unavailable. Thermal protection paused.");
        }
        return;
    }

    lastTemperatureC = temp;
    lastTemperatureValid = true;

    if (overheatSensorFault) {
        overheatSensorFault = false;
        overheatLastReason = "Thermal protection restored: temperature sensor reading valid.";
        overheatLastTimestamp = formatUptime((millis() - bootTime) / 1000);
        persistOverheatNote();
        simplePrintln("Temperature sensor restored. Thermal protection active again.");
    }

    if (temp > maxTemperature) {
        maxTemperature = temp;
    }

    bool protectionActive = overheatProtectionEnabled && !overheatSensorFault;
    if (protectionActive) {
        if (!overheatLockoutActive && temp >= overheatShutdownC) {
            overheatLockoutActive = true;
            recordOverheatTrip(temp);
            // Request Core 1 to stop streaming safely
            if (isStreaming) {
                requestStreamStop("overheat");
            }
            stopAudioCaptureTask();
            rtspServerEnabled = false;
            rtspServer.stop();
        } else if (overheatLockoutActive && temp <= (overheatShutdownC - OVERHEAT_LIMIT_STEP_C)) {
            // Allow re-arming after we cool down by at least one step
            overheatLockoutActive = false;
        }
    } else {
        overheatLockoutActive = false;
    }

    // Only warn occasionally on high temperature; no periodic logging
    static unsigned long lastTempWarn = 0;
    float warnThreshold = max(overheatShutdownC - 5.0f, (float)OVERHEAT_MIN_LIMIT_C);
    if (temp > warnThreshold && (millis() - lastTempWarn) > 600000UL) { // 10 min cooldown
        simplePrintln("WARNING: High temperature detected (" + String(temp, 1) + " C). Approaching shutdown limit.");
        lastTempWarn = millis();
    }
}

// Performance diagnostics
void checkPerformance() {
    uint32_t currentHeap = ESP.getFreeHeap();
    if (currentHeap < minFreeHeap) {
        minFreeHeap = currentHeap;
    }

    if (isStreaming && (millis() - lastStatsReset) > 30000) {
        // Cooldown: skip performance check for 2 minutes after last I2S reset
        if (lastI2SReset > 0 && (millis() - lastI2SReset) < 120000) {
            return;
        }

        uint32_t runtime = millis() - lastStatsReset;
        uint32_t currentRate = (audioPacketsSent * 1000) / runtime;

        if (currentRate > maxPacketRate) maxPacketRate = currentRate;
        if (currentRate < minPacketRate) minPacketRate = currentRate;

        static uint8_t consecutiveLowCount = 0;
        if (currentRate < minAcceptableRate) {
            consecutiveLowCount++;
            simplePrintln("Low packet rate: " + String(currentRate) + " < " + String(minAcceptableRate) + " pkt/s (" + String(consecutiveLowCount) + "/3)");

            if (consecutiveLowCount >= 3 && autoRecoveryEnabled) {
                simplePrintln("AUTO-RECOVERY: 3 consecutive failures, restarting I2S...");
                consecutiveLowCount = 0;
                restartI2S();
                audioPacketsSent = 0;
                lastStatsReset = millis();
                lastI2SReset = millis();
            }
        } else {
            consecutiveLowCount = 0;
        }
    }
}

// WiFi health check
void checkWiFiHealth() {
    static bool prevConnected = false;
    static IPAddress prevIp(0, 0, 0, 0);

    bool connected = (WiFi.status() == WL_CONNECTED);
    if (!connected) {
        // Stop streaming before reconnecting — no point sending RTP into a dead radio
        if (isStreaming) {
            requestStreamStop("WiFi disconnect");
            stopAudioCaptureTask();
        }
        simplePrintln("WiFi disconnected! Reconnecting...");
        WiFi.reconnect();
    } else {
        IPAddress ip = WiFi.localIP();
        // On reconnect/IP change, make sure RTSP server socket is listening on new interface.
        if (!prevConnected || ip != prevIp) {
            simplePrintln("WiFi up: IP=" + ip.toString() + " GW=" + WiFi.gatewayIP().toString());
            if (rtspServerEnabled) {
                rtspServer.stop();
                delay(20);
                rtspServer.begin();
                rtspServer.setNoDelay(true);
                simplePrintln("RTSP server rebound on :8554");
            }
        }
        prevIp = ip;
    }

    prevConnected = connected;

    // Re-apply TX power WITHOUT logging (prevent periodic log spam)
    applyWifiTxPower(false);

    if (connected) {
        int32_t rssi = WiFi.RSSI();
        if (rssi < -85) {
            simplePrintln("WARNING: Weak WiFi signal: " + String(rssi) + " dBm");
        }
    }
}

// Scheduled reset
void checkScheduledReset() {
    if (!scheduledResetEnabled) return;

    unsigned long uptimeHours = (millis() - bootTime) / 3600000;
    if (uptimeHours >= resetIntervalHours) {
        simplePrintln("SCHEDULED RESET: " + String(resetIntervalHours) + " hours reached");
        delay(1000);
        ESP.restart();
    }
}

// Load settings from flash
void loadAudioSettings() {
    audioPrefs.begin("audio", false);
    bool hasSampleRatePref = audioPrefs.isKey("sampleRate");
    bool hasGainPref = audioPrefs.isKey("gainFactor");
    bool hasBufferPref = audioPrefs.isKey("bufferSize");
    bool hasHpCutoffPref = audioPrefs.isKey("hpCutoff");
    currentSampleRate = audioPrefs.getUInt("sampleRate", DEFAULT_SAMPLE_RATE);
    currentGainFactor = audioPrefs.getFloat("gainFactor", DEFAULT_GAIN_FACTOR);
    currentBufferSize = audioPrefs.getUShort("bufferSize", DEFAULT_BUFFER_SIZE);
    // i2sShiftBits is ALWAYS 0 for PDM microphones - not configurable
    i2sShiftBits = 0;
    autoRecoveryEnabled = audioPrefs.getBool("autoRecovery", false);
    scheduledResetEnabled = audioPrefs.getBool("schedReset", false);
    resetIntervalHours = audioPrefs.getUInt("resetHours", 24);
    minAcceptableRate = audioPrefs.getUInt("minRate", 50);
    performanceCheckInterval = audioPrefs.getUInt("checkInterval", 15);
    autoThresholdEnabled = audioPrefs.getBool("thrAuto", true);
    cpuFrequencyMhz = audioPrefs.getUChar("cpuFreq", 160);
    wifiTxPowerDbm = audioPrefs.getFloat("wifiTxDbm", DEFAULT_WIFI_TX_DBM);
    highpassEnabled = audioPrefs.getBool("hpEnable", DEFAULT_HPF_ENABLED);
    highpassCutoffHz = (uint16_t)audioPrefs.getUInt("hpCutoff", DEFAULT_HPF_CUTOFF_HZ);
    agcEnabled = audioPrefs.getBool("agcEnable", false);
    noiseFilterEnabled = audioPrefs.getBool("noiseFilter", true);
    if (!hasSampleRatePref) currentSampleRate = DEFAULT_SAMPLE_RATE;
    if (!hasGainPref) currentGainFactor = DEFAULT_GAIN_FACTOR;
    if (!hasBufferPref) currentBufferSize = DEFAULT_BUFFER_SIZE;
    if (!hasHpCutoffPref) highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
    ledMode = audioPrefs.getUChar("ledMode", 1);
    if (ledMode > 2) ledMode = 1;
    overheatProtectionEnabled = audioPrefs.getBool("ohEnable", DEFAULT_OVERHEAT_PROTECTION);
    uint32_t ohLimit = audioPrefs.getUInt("ohThresh", DEFAULT_OVERHEAT_LIMIT_C);
    if (ohLimit < OVERHEAT_MIN_LIMIT_C) ohLimit = OVERHEAT_MIN_LIMIT_C;
    if (ohLimit > OVERHEAT_MAX_LIMIT_C) ohLimit = OVERHEAT_MAX_LIMIT_C;
    ohLimit = OVERHEAT_MIN_LIMIT_C + ((ohLimit - OVERHEAT_MIN_LIMIT_C) / OVERHEAT_LIMIT_STEP_C) * OVERHEAT_LIMIT_STEP_C;
    overheatShutdownC = (float)ohLimit;
    overheatLastReason = audioPrefs.getString("ohReason", "");
    overheatLastTimestamp = audioPrefs.getString("ohStamp", "");
    overheatTripTemp = audioPrefs.getFloat("ohTripC", 0.0f);
    overheatLatched = audioPrefs.getBool("ohLatched", false);
    audioPrefs.end();

    if (autoThresholdEnabled) {
        minAcceptableRate = computeRecommendedMinRate();
    }
    if (overheatLatched) {
        rtspServerEnabled = false;
    }
    // Log the configured TX dBm (not the current enum), snapped for clarity
    float txShown = wifiPowerLevelToDbm(pickWifiPowerLevel(wifiTxPowerDbm));
    simplePrintln("Loaded settings: Rate=" + String(currentSampleRate) +
                  ", Gain=" + String(currentGainFactor, 1) +
                  ", Buffer=" + String(currentBufferSize) +
                  " (chunk " + String(effectiveAudioChunkSize()) + ")" +
                  ", WiFiTX=" + String(txShown, 1) + "dBm" +
                  ", shiftBits=" + String(i2sShiftBits) +
                  ", HPF=" + String(highpassEnabled?"on":"off") +
                  ", HPFcut=" + String(highpassCutoffHz) + "Hz");
}

// Save settings to flash
void saveAudioSettings() {
    audioPrefs.begin("audio", false);
    audioPrefs.putUInt("sampleRate", currentSampleRate);
    audioPrefs.putFloat("gainFactor", currentGainFactor);
    audioPrefs.putUShort("bufferSize", currentBufferSize);
    audioPrefs.putUChar("shiftBits", i2sShiftBits);
    audioPrefs.putBool("autoRecovery", autoRecoveryEnabled);
    audioPrefs.putBool("schedReset", scheduledResetEnabled);
    audioPrefs.putUInt("resetHours", resetIntervalHours);
    audioPrefs.putUInt("minRate", minAcceptableRate);
    audioPrefs.putUInt("checkInterval", performanceCheckInterval);
    audioPrefs.putBool("thrAuto", autoThresholdEnabled);
    audioPrefs.putUChar("cpuFreq", cpuFrequencyMhz);
    audioPrefs.putFloat("wifiTxDbm", wifiTxPowerDbm);
    audioPrefs.putBool("hpEnable", highpassEnabled);
    audioPrefs.putUInt("hpCutoff", (uint32_t)highpassCutoffHz);
    audioPrefs.putBool("agcEnable", agcEnabled);
    audioPrefs.putBool("noiseFilter", noiseFilterEnabled);
    audioPrefs.putUChar("ledMode", ledMode);
    audioPrefs.putBool("ohEnable", overheatProtectionEnabled);
    uint32_t ohLimit = (uint32_t)(overheatShutdownC + 0.5f);
    if (ohLimit < OVERHEAT_MIN_LIMIT_C) ohLimit = OVERHEAT_MIN_LIMIT_C;
    if (ohLimit > OVERHEAT_MAX_LIMIT_C) ohLimit = OVERHEAT_MAX_LIMIT_C;
    audioPrefs.putUInt("ohThresh", ohLimit);
    audioPrefs.putString("ohReason", overheatLastReason);
    audioPrefs.putString("ohStamp", overheatLastTimestamp);
    audioPrefs.putFloat("ohTripC", overheatTripTemp);
    audioPrefs.putBool("ohLatched", overheatLatched);
    audioPrefs.end();

    simplePrintln("Settings saved to flash");
}

// Schedule a safe reboot (optionally with factory reset) after delayMs
void scheduleReboot(bool factoryReset, uint32_t delayMs) {
    scheduledFactoryReset = factoryReset;
    scheduledRebootAt = millis() + delayMs;
}

// Keep live streaming cadence bounded even if the configured buffer is large.
// Larger UI buffer values are still accepted, but the RTP pipeline is chunked to
// at most 1024 samples so playback does not turn into long bursty writes.
uint16_t effectiveAudioChunkSize() {
    uint16_t chunk = currentBufferSize;
    if (chunk < 256) chunk = 256;
    if (chunk > 1024) chunk = 1024;
    return chunk;
}

// Compute recommended minimum packet-rate threshold based on the effective stream chunk size
uint32_t computeRecommendedMinRate() {
    uint32_t buf = max((uint16_t)1, effectiveAudioChunkSize());
    float expectedPktPerSec = (float)currentSampleRate / (float)buf;
    uint32_t rec = (uint32_t)(expectedPktPerSec * 0.5f + 0.5f); // 50% safety margin
    if (rec < 5) rec = 5;
    return rec;
}

// Restore application settings to safe defaults and persist
void resetToDefaultSettings() {
    simplePrintln("FACTORY RESET: Restoring default settings...");

    // Clear persisted settings in our namespace
    audioPrefs.begin("audio", false);
    audioPrefs.clear();
    audioPrefs.end();

    // Reset runtime variables to defaults
    currentSampleRate = DEFAULT_SAMPLE_RATE;
    currentGainFactor = DEFAULT_GAIN_FACTOR;
    currentBufferSize = DEFAULT_BUFFER_SIZE;
    i2sShiftBits = 0;  // Fixed for Unit Mini PDM capture

    autoRecoveryEnabled = false;
    autoThresholdEnabled = true;
    scheduledResetEnabled = false;
    resetIntervalHours = 24;
    minAcceptableRate = computeRecommendedMinRate();
    performanceCheckInterval = 15;
    cpuFrequencyMhz = 160;
    wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
    highpassEnabled = DEFAULT_HPF_ENABLED;
    highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
    agcEnabled = false;
    agcMultiplier = 1.0f;
    noiseFilterEnabled = true;
    noiseFloorDbfs = -90.0f;
    noiseGateDbfs = -90.0f;
    noiseReductionDb = 0.0f;
    ledMode = 1;
    overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
    overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
    overheatLockoutActive = false;
    overheatTripTemp = 0.0f;
    overheatTriggeredAt = 0;
    overheatLastReason = "";
    overheatLastTimestamp = "";
    overheatSensorFault = false;
    overheatLatched = false;
    lastTemperatureC = 0.0f;
    lastTemperatureValid = false;

    isStreaming = false;

    saveAudioSettings();

    simplePrintln("Defaults applied. Device will reboot.");
}

// Restart I2S with new parameters
void restartI2S() {
    simplePrintln("Restarting I2S with new parameters...");
    bool wasStreaming = isStreaming;

    // Request Core 1 to stop streaming safely
    if (isStreaming) {
        requestStreamStop("I2S restart");
    }

    // Stop audio pipeline task on Core 1
    stopAudioCaptureTask();

    // Restart I2S driver
    setup_i2s_driver();

    // Refresh HPF with current parameters
    updateHighpassCoeffs();
    maxPacketRate = 0;
    minPacketRate = 0xFFFFFFFF;

    // If we were streaming, restart the pipeline with the existing client
    if (wasStreaming && rtspClient && rtspClient.connected()) {
        streamClient = &rtspClient;
        isStreaming = true;
        startAudioCaptureTask();
        simplePrintln("I2S restarted, streaming resumed");
    } else {
        simplePrintln("I2S restarted");
    }
}

// Minimal print helpers: Serial + buffered for Web UI
// Timestamp prefix for log messages (NTP time if available, uptime fallback)
static String logTimestamp() {
    time_t now;
    time(&now);
    if (now > 100000) {
        struct tm ti;
        localtime_r(&now, &ti);
        char buf[24];
        strftime(buf, sizeof(buf), "[%H:%M:%S] ", &ti);
        return String(buf);
    }
    // Fallback: uptime
    unsigned long s = (millis() - bootTime) / 1000;
    unsigned long h = s / 3600; s %= 3600;
    unsigned long m = s / 60; s %= 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "[%02lu:%02lu:%02lu] ", h, m, s);
    return String(buf);
}

void simplePrint(String message) {
    Serial.print(logTimestamp() + message);
}

void simplePrintln(String message) {
    String stamped = logTimestamp() + message;
    Serial.println(stamped);
    webui_pushLog(stamped);
}

static void resumeRtspServerAfterOtaFailure() {
    if (overheatLatched) {
        rtspServerEnabled = false;
        rtspServer.stop();
        return;
    }
    if (!rtspServerEnabled) return;
    rtspServer.stop();
    delay(20);
    rtspServer.begin();
    rtspServer.setNoDelay(true);
}

static void setupArduinoOta() {
    ArduinoOTA.setHostname(networkHostname.c_str());
    ArduinoOTA.setPort(OTA_PORT);

    ArduinoOTA.onStart([]() {
        const char* updateType = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
        simplePrintln(String("OTA start: ") + updateType + " update requested");
        otaPreviousRtspEnabled = rtspServerEnabled;
        if (isStreaming) {
            requestStreamStop("OTA update");
        }
        stopAudioCaptureTask();
        rtspServer.stop();
        rtspServerEnabled = false;
        core1OwnsLED = false;
        setStatusLed(CRGB(128, 0, 128));
    });

    ArduinoOTA.onEnd([]() {
        simplePrintln("OTA update complete, rebooting");
        setStatusLed(CRGB(0, 128, 0));
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static uint8_t lastPercent = 255;
        uint8_t percent = total ? (progress * 100U) / total : 0;
        if (percent != lastPercent && (percent == 0 || percent == 100 || (percent % 10) == 0)) {
            simplePrintln("OTA progress: " + String(percent) + "%");
            lastPercent = percent;
        }
    });

    ArduinoOTA.onError([](ota_error_t error) {
        String reason = "unknown";
        switch (error) {
            case OTA_AUTH_ERROR:    reason = "auth failed"; break;
            case OTA_BEGIN_ERROR:   reason = "begin failed"; break;
            case OTA_CONNECT_ERROR: reason = "connect failed"; break;
            case OTA_RECEIVE_ERROR: reason = "receive failed"; break;
            case OTA_END_ERROR:     reason = "end failed"; break;
            default: break;
        }
        simplePrintln("OTA error: " + reason);
        startAudioCaptureTask();
        rtspServerEnabled = otaPreviousRtspEnabled;
        resumeRtspServerAfterOtaFailure();
        if (ledMode > 0) setStatusLed(CRGB(0, 0, 128));
        else setStatusLed(CRGB(0, 0, 0));
    });

    ArduinoOTA.begin();
    simplePrintln("OTA ready: pio run -e m5stack-atoms3-lite-ota -t upload");
    simplePrintln("OTA target: " + networkHostname + ".local:" + String(OTA_PORT));
}

// Drain any pending data from RTSP client receive buffer
// Called on connect/disconnect events only — prevents stale data buildup
void drainRtspReceiveBuffer(WiFiClient &client) {
    if (!client || !client.connected()) return;
    int drained = 0;
    while (client.available() > 0 && drained < 4096) {
        uint8_t buf[256];
        int n = client.read(buf, sizeof(buf));
        if (n <= 0) break;
        drained += n;
    }
    if (drained > 0) {
        simplePrintln("Drained " + String(drained) + " bytes from RTSP receive buffer");
    }
}


static void updateFftFromBlock(const int16_t* samples, uint16_t count) {
    const int N = 128;
    const int BINS = 32;
    if (!samples || count < N) return;

    float mean = 0.0f;
    for (int i = 0; i < N; i++) mean += (float)samples[i];
    mean /= (float)N;

    float mags[BINS];
    float maxMag = 0.0f;
    for (int k = 0; k < BINS; k++) {
        float re = 0.0f;
        float im = 0.0f;
        for (int n = 0; n < N; n++) {
            float x = ((float)samples[n] - mean) * (0.5f - 0.5f * cosf((2.0f * PI * (float)n) / (float)(N - 1)));
            float phase = 2.0f * PI * (float)k * (float)n / (float)N;
            re += x * cosf(phase);
            im -= x * sinf(phase);
        }
        float mag = sqrtf(re * re + im * im);
        mag *= (1.0f + 0.01f * (float)k);
        mags[k] = mag;
        if (mag > maxMag) maxMag = mag;
    }

    if (maxMag < 1.0f) maxMag = 1.0f;
    for (int k = 0; k < BINS; k++) {
        float norm = mags[k] / maxMag;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        fftBins[k] = (uint8_t)(norm * 255.0f);
    }
    fftFrameSeq++;
}

// ================== CORE 1: FULL AUDIO PIPELINE ==================
// I2S capture → process (HPF, gain, AGC) → RTP → WiFi
// Uses streamClient pointer (set by Core 0 on PLAY, cleared here on failure)
// IMPORTANT: No String allocation or simplePrintln on this core (heap contention)
void audioCaptureTask(void* parameter) {
    Serial.println("[Core1] Audio pipeline task started");
    audioTaskRunning = true;

    size_t bytesRead = 0;
    uint32_t consecutiveErrors = 0;
    const uint32_t MAX_ERRORS = 10;
    uint32_t packetCount = 0;

    const uint16_t chunkSamples = effectiveAudioChunkSize();
    int16_t* captureBuffer = (int16_t*)malloc(chunkSamples * sizeof(int16_t));
    int16_t* outputBuffer = (int16_t*)malloc(chunkSamples * sizeof(int16_t));

    if (!captureBuffer || !outputBuffer) {
        Serial.println("[Core1] FATAL: Failed to allocate audio buffers!");
        if (captureBuffer) free(captureBuffer);
        if (outputBuffer) free(outputBuffer);
        audioTaskRunning = false;
        vTaskDelete(NULL);
        return;
    }

    // High-pass filter state (local to this core)
    Biquad localHpf = hpf;
    uint32_t localHpfConfigSampleRate = hpfConfigSampleRate;
    uint16_t localHpfConfigCutoff = hpfConfigCutoff;

    // AGC, limiter, and noise-filter state (local)
    float localAgcMult = 1.0f;
    float localLimiterGain = 1.0f;
    float localNoiseFloor = NOISE_FLOOR_INIT;
    float localNoiseGate = max(NOISE_GATE_MIN, localNoiseFloor * NOISE_GATE_RATIO + NOISE_GATE_MARGIN);
    float localNoiseGain = 1.0f;
    float localNoiseEnv = 0.0f;
    uint32_t localNoiseGateHoldSamples = 0;
    float localNoiseReduction = 0.0f;
    const float LIMITER_TARGET_PEAK = 26000.0f;  // ~79% FS to keep headroom and reduce harsh clipping
    const float LIMITER_RELEASE = 0.02f;         // Slow recovery keeps ambience stable instead of pumping
    const float LIMITER_MIN_GAIN = 0.20f;

    uint32_t i2sErrors = 0;
    unsigned long lastStatsLog = millis();
    unsigned long lastLedUpdate = 0;
    unsigned long lastGoodCaptureMs = millis();
    int16_t lastOutputSample = 0;
    const TickType_t readTimeoutTicks = pdMS_TO_TICKS(max(20UL, min(100UL,
        ((unsigned long)chunkSamples * 1000UL) / max((uint32_t)1, currentSampleRate) + 10UL)));

    while (audioTaskRunning) {
        // Check if Core 0 requested us to stop streaming
        if (stopStreamRequested) {
            WiFiClient* client = streamClient;
            if (client) {
                client->stop();
            }
            streamClient = NULL;
            isStreaming = false;
            core1OwnsLED = false;
            streamCleanupDone = true;
            __asm__ __volatile__("memw" ::: "memory");
            // Wait for Core 0 to clear stopStreamRequested
            while (stopStreamRequested && audioTaskRunning) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;
        }

        // Keep I2S capture running even when no RTSP client is active,
        // so /api/audio_status can show live raw-mic diagnostics.
        WiFiClient* client = streamClient;
        bool streamActive = (isStreaming && client != NULL);

        // Periodic stats (every 30s) — Serial.printf only, no heap alloc
        if (millis() - lastStatsLog > 30000) {
            Serial.printf("[Core1] Sent=%u I2Serr=%u Clip=%lu AGC=%.2f RawPeak=%u RawRMS=%u RawMin=%d RawMax=%d Z=%u%%\n",
                         packetCount, i2sErrors, audioClipCount, localAgcMult,
                         i2sLastRawPeakAbs, i2sLastRawRms, i2sLastRawMin, i2sLastRawMax, i2sLastRawZeroPct);
            lastStatsLog = millis();
        }

        // Read from I2S with a cadence-aware timeout so concealment can kick in
        esp_err_t result = i2s_read(I2S_NUM_0, captureBuffer,
                                    chunkSamples * sizeof(int16_t),
                                    &bytesRead, readTimeoutTicks);
        unsigned long processStartUs = micros();

        if (result != ESP_OK || bytesRead == 0) {
            if (result != ESP_OK) {
                consecutiveErrors++;
                i2sErrors++;
                i2sReadErrCount++;
                if (consecutiveErrors >= MAX_ERRORS) {
                    Serial.println("[Core1] Too many I2S errors, pausing");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    consecutiveErrors = 0;
                }
            } else {
                i2sReadZeroCount++;
            }

            if (streamActive) {
                audioFallbackBlockCount++;
                i2sLastGapMs = millis() - lastGoodCaptureMs;
                fillConcealmentBlock(outputBuffer, chunkSamples, lastOutputSample);
                lastOutputSample = outputBuffer[chunkSamples - 1];
                lastPeakAbs16 = (uint16_t)abs((int)outputBuffer[0]);
                audioClippedLastBlock = false;
                sendRTPPacket(*client, outputBuffer, chunkSamples);
                packetCount++;

                float blockMs = (1000.0f * (float)chunkSamples) / max((float)currentSampleRate, 1.0f);
                float workMs = (float)(micros() - processStartUs) / 1000.0f;
                float instLoad = (blockMs > 0.0f) ? (workMs * 100.0f / blockMs) : 0.0f;
                if (instLoad > 100.0f) instLoad = 100.0f;
                audioPipelineLoadPct += (instLoad - audioPipelineLoadPct) * 0.20f;
            }
            continue;
        }

        consecutiveErrors = 0;
        i2sReadOkCount++;
        lastGoodCaptureMs = millis();
        i2sLastGapMs = 0;
        uint16_t samplesRead = bytesRead / sizeof(int16_t);
        i2sLastSamplesRead = samplesRead;

        // Update HPF coefficients if changed
        if (highpassEnabled && (localHpfConfigSampleRate != currentSampleRate ||
                                localHpfConfigCutoff != highpassCutoffHz)) {
            localHpf = hpf;
            localHpfConfigSampleRate = currentSampleRate;
            localHpfConfigCutoff = highpassCutoffHz;
        }

        // Determine effective gain (manual * AGC if enabled)
        float effectiveGain = currentGainFactor;
        if (agcEnabled) {
            effectiveGain *= localAgcMult;
        }
        if (localLimiterGain < 1.0f) {
            localLimiterGain += (1.0f - localLimiterGain) * LIMITER_RELEASE;
            if (localLimiterGain > 1.0f) localLimiterGain = 1.0f;
        }

        // Process audio: HPF, adaptive noise filter, gain, limiter, clipping detection, RMS for AGC
        bool clipped = false;
        float peakAbs = 0.0f;
        float sumSquares = 0.0f;

        // Raw I2S diagnostics before DSP (used to detect dead/flat/stuck mic input)
        int16_t rawMin = INT16_MAX;
        int16_t rawMax = INT16_MIN;
        uint16_t rawPeakAbs = 0;
        uint32_t rawZeroCount = 0;
        float rawSumSquares = 0.0f;

        // First pass: collect raw stats for diagnostics and format detection.
        for (int i = 0; i < samplesRead; i++) {
            int16_t raw = captureBuffer[i];
            if (raw < rawMin) rawMin = raw;
            if (raw > rawMax) rawMax = raw;
            uint16_t rawAbs = (raw < 0) ? (uint16_t)(-raw) : (uint16_t)raw;
            if (rawAbs > rawPeakAbs) rawPeakAbs = rawAbs;
            if (raw == 0) rawZeroCount++;
            rawSumSquares += (float)raw * (float)raw;
        }

        // Some PDM front-ends return unsigned low-amplitude PCM (e.g. around 1024)
        // instead of signed 16-bit centered at 0. Detect and normalize per block.
        bool likelyUnsignedPdm = (rawMin >= 0 && rawMax <= 4095);
        i2sLikelyUnsignedPcm = likelyUnsignedPdm;
        float pdmCenter = 0.0f;
        float pdmScale = 1.0f;
        if (likelyUnsignedPdm) {
            pdmCenter = (rawMax > 2047) ? 2048.0f : 1024.0f;
            pdmScale = (rawMax > 2047) ? 16.0f : 32.0f;
        }

        // Second pass: DSP and output generation.
        for (int i = 0; i < samplesRead; i++) {
            int16_t raw = captureBuffer[i];

            float sample = likelyUnsignedPdm
                ? ((float)raw - pdmCenter) * pdmScale
                : (float)(raw >> i2sShiftBits);

            if (highpassEnabled) {
                sample = localHpf.process(sample);
            }

            float sampleAbs = fabsf(sample);
            if (noiseFilterEnabled) {
                float envSlew = (sampleAbs > localNoiseEnv) ? NOISE_ENV_ATTACK : NOISE_ENV_RELEASE;
                localNoiseEnv += (sampleAbs - localNoiseEnv) * envSlew;

                float floorFollow = (localNoiseEnv <= localNoiseGate) ? NOISE_FLOOR_FAST : NOISE_FLOOR_SLOW;
                localNoiseFloor += (localNoiseEnv - localNoiseFloor) * floorFollow;
                if (localNoiseFloor < 0.0f) localNoiseFloor = 0.0f;
                localNoiseGate = localNoiseFloor * NOISE_GATE_RATIO + NOISE_GATE_MARGIN;
                if (localNoiseGate < NOISE_GATE_MIN) localNoiseGate = NOISE_GATE_MIN;
                if (localNoiseGate > NOISE_GATE_MAX) localNoiseGate = NOISE_GATE_MAX;

                uint32_t holdSamples = ((uint32_t)currentSampleRate * NOISE_GATE_HOLD_MS) / 1000UL;
                if (holdSamples < 1) holdSamples = 1;

                if (localNoiseEnv >= localNoiseGate) {
                    localNoiseGateHoldSamples = holdSamples;
                } else if (localNoiseGateHoldSamples > 0) {
                    localNoiseGateHoldSamples--;
                }

                float desiredNoiseGain = 1.0f;
                float closeThreshold = localNoiseGate * NOISE_GATE_CLOSE_RATIO;
                if (localNoiseGateHoldSamples == 0 && localNoiseGate > 1.0f && localNoiseEnv < closeThreshold) {
                    float normalized = localNoiseEnv / closeThreshold;
                    if (normalized < 0.0f) normalized = 0.0f;
                    if (normalized > 1.0f) normalized = 1.0f;
                    desiredNoiseGain = NOISE_FILTER_MIN_GAIN + (1.0f - NOISE_FILTER_MIN_GAIN) * normalized * normalized;
                }

                float noiseSlew = (desiredNoiseGain < localNoiseGain) ? NOISE_FILTER_ATTACK : NOISE_FILTER_RELEASE;
                localNoiseGain += (desiredNoiseGain - localNoiseGain) * noiseSlew;
                if (localNoiseGain < NOISE_FILTER_MIN_GAIN) localNoiseGain = NOISE_FILTER_MIN_GAIN;
                if (localNoiseGain > 1.0f) localNoiseGain = 1.0f;
                sample *= localNoiseGain;
                localNoiseReduction = 20.0f * log10f(max(localNoiseGain, 0.0001f));
            } else {
                localNoiseGain = 1.0f;
                localNoiseEnv = 0.0f;
                localNoiseGateHoldSamples = 0;
                localNoiseReduction = 0.0f;
            }

            float amplified = sample * effectiveGain * localLimiterGain;
            float aabs = fabsf(amplified);
            if (aabs > LIMITER_TARGET_PEAK && aabs > 1.0f) {
                float neededGain = LIMITER_TARGET_PEAK / aabs;
                localLimiterGain *= neededGain;
                if (localLimiterGain < LIMITER_MIN_GAIN) localLimiterGain = LIMITER_MIN_GAIN;
                amplified = sample * effectiveGain * localLimiterGain;
                aabs = fabsf(amplified);
            }
            if (aabs > peakAbs) peakAbs = aabs;
            if (aabs > 32767.0f) clipped = true;
            sumSquares += amplified * amplified;

            if (amplified > 32767.0f) amplified = 32767.0f;
            if (amplified < -32768.0f) amplified = -32768.0f;
            outputBuffer[i] = (int16_t)amplified;
        }
        if (samplesRead > 0) {
            lastOutputSample = outputBuffer[samplesRead - 1];
        }

        // Publish latest processed frame for browser WebAudio endpoint
        uint16_t publishSamples = samplesRead;
        if (publishSamples > WEB_AUDIO_MAX_SAMPLES) publishSamples = WEB_AUDIO_MAX_SAMPLES;
        portENTER_CRITICAL(&webAudioMux);
        memcpy(webAudioFrame, outputBuffer, publishSamples * sizeof(int16_t));
        webAudioFrameSamples = publishSamples;
        webAudioSampleRate = currentSampleRate;
        webAudioFrameSeq++;
        portEXIT_CRITICAL(&webAudioMux);

        // Update spectrum diagnostics from processed output (throttled)
        static uint8_t fftDecimator = 0;
        fftDecimator++;
        if ((fftDecimator & 0x03) == 0 && samplesRead >= 128) {
            updateFftFromBlock(outputBuffer, samplesRead);
        }

        // Publish raw capture diagnostics for UI/API
        i2sLastRawMin = rawMin;
        i2sLastRawMax = rawMax;
        i2sLastRawPeakAbs = rawPeakAbs;
        if (samplesRead > 0) {
            float rawRms = sqrtf(rawSumSquares / (float)samplesRead);
            if (rawRms > 65535.0f) rawRms = 65535.0f;
            i2sLastRawRms = (uint16_t)rawRms;
            i2sLastRawZeroPct = (uint16_t)((rawZeroCount * 100UL) / samplesRead);
        } else {
            i2sLastRawRms = 0;
            i2sLastRawZeroPct = 100;
        }

        // AGC: adjust multiplier based on RMS
        if (agcEnabled && samplesRead > 0) {
            float rms = sqrtf(sumSquares / (float)samplesRead) / 32767.0f;
            if (rms > 0.001f) {  // Don't adjust on silence
                float ratio = AGC_TARGET_RMS / rms;
                if (ratio < 1.0f) {
                    // Signal too loud — fast attack (reduce gain quickly)
                    localAgcMult += (ratio - 1.0f) * AGC_ATTACK_RATE * localAgcMult;
                } else {
                    // Signal too quiet — slow release (increase gain slowly)
                    localAgcMult += (ratio - 1.0f) * AGC_RELEASE_RATE * localAgcMult;
                }
                if (localAgcMult < AGC_MIN_MULT) localAgcMult = AGC_MIN_MULT;
                if (localAgcMult > AGC_MAX_MULT) localAgcMult = AGC_MAX_MULT;
            }
            agcMultiplier = localAgcMult;  // Publish for WebUI
        }

        if (noiseFilterEnabled) {
            float floorNorm = max(localNoiseFloor, 1.0f) / 32767.0f;
            float gateNorm = max(localNoiseGate, 1.0f) / 32767.0f;
            noiseFloorDbfs = 20.0f * log10f(floorNorm);
            noiseGateDbfs = 20.0f * log10f(gateNorm);
            noiseReductionDb = localNoiseReduction;
        } else {
            noiseFloorDbfs = -90.0f;
            noiseGateDbfs = -90.0f;
            noiseReductionDb = 0.0f;
        }

        // Update metering
        if (peakAbs > 32767.0f) peakAbs = 32767.0f;
        lastPeakAbs16 = (uint16_t)peakAbs;
        audioClippedLastBlock = clipped;
        if (clipped) {
            audioClipCount++;
            static unsigned long lastClipLog = 0;
            if (millis() - lastClipLog > 5000) {
                Serial.printf("[Core1] Clipping! Peak=%u count=%lu\n",
                             lastPeakAbs16, audioClipCount);
                lastClipLog = millis();
            }
        }

        if (lastPeakAbs16 > peakHoldAbs16) {
            peakHoldAbs16 = lastPeakAbs16;
            peakHoldUntilMs = millis() + 3000UL;
        } else if (peakHoldAbs16 > 0 && millis() > peakHoldUntilMs) {
            peakHoldAbs16 = 0;
        }

        // LED update (throttled to ~10 Hz) only while actively streaming.
        if (streamActive && millis() - lastLedUpdate > 100) {
            if (ledMode == 2) {
                // Level mode: color-coded audio level
                float pct = peakAbs / 32767.0f;
                if (clipped) {
                    setStatusLed(CRGB(255, 0, 0));        // Red: clipping
                } else if (pct > 0.7f) {
                    setStatusLed(CRGB(255, 165, 0));      // Orange: hot signal
                } else if (pct > 0.3f) {
                    setStatusLed(CRGB(0, 255, 0));        // Bright green: good level
                } else if (pct > 0.05f) {
                    setStatusLed(CRGB(0, 64, 0));         // Dim green: low signal
                } else {
                    setStatusLed(CRGB(32, 0, 32));        // Dim purple: very quiet
                }
            } else if (ledMode == 1) {
                // Static mode: solid green during streaming
                setStatusLed(CRGB(0, 128, 0));
            } else {
                // Off mode: LED dark during streaming
                setStatusLed(CRGB(0, 0, 0));
            }
            lastLedUpdate = millis();
        }

        if (streamActive) {
            // Send RTP packet
            sendRTPPacket(*client, outputBuffer, samplesRead);
            packetCount++;
        }

        float blockMs = (1000.0f * (float)max((uint16_t)1, samplesRead)) / max((float)currentSampleRate, 1.0f);
        float workMs = (float)(micros() - processStartUs) / 1000.0f;
        float instLoad = (blockMs > 0.0f) ? (workMs * 100.0f / blockMs) : 0.0f;
        if (instLoad > 100.0f) instLoad = 100.0f;
        audioPipelineLoadPct += (instLoad - audioPipelineLoadPct) * 0.15f;

        // Process incoming RTSP commands during streaming (~every 200ms)
        // Keeps all socket I/O on Core 1 during streaming
        static unsigned long lastRtspCheck = 0;
        if (streamActive && (millis() - lastRtspCheck > 200)) {
            lastRtspCheck = millis();
            if (client->available() > 0) {
                char rtspBuf[512];
                int avail = client->available();
                if (avail > (int)sizeof(rtspBuf) - 1) avail = sizeof(rtspBuf) - 1;
                int n = client->read((uint8_t*)rtspBuf, avail);
                if (n > 0) {
                    rtspBuf[n] = '\0';
                    // Parse for RTSP commands
                    if (strstr(rtspBuf, "TEARDOWN") != NULL) {
                        // Extract CSeq
                        const char* cseqStr = strstr(rtspBuf, "CSeq: ");
                        int cseqVal = 1;
                        if (cseqStr) cseqVal = atoi(cseqStr + 6);
                        // Send response (Core 1 owns the socket)
                        char resp[128];
                        int rlen = snprintf(resp, sizeof(resp),
                            "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n", cseqVal);
                        client->write((uint8_t*)resp, rlen);
                        // Close and clean up
                        client->stop();
                        streamClient = NULL;
                        isStreaming = false;
                        core1OwnsLED = false;
                        Serial.println("[Core1] TEARDOWN received, stream stopped");
                    } else if (strstr(rtspBuf, "GET_PARAMETER") != NULL) {
                        const char* cseqStr = strstr(rtspBuf, "CSeq: ");
                        int cseqVal = 1;
                        if (cseqStr) cseqVal = atoi(cseqStr + 6);
                        char resp[128];
                        int rlen = snprintf(resp, sizeof(resp),
                            "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n", cseqVal);
                        client->write((uint8_t*)resp, rlen);
                        lastRTSPActivity = millis();
                    }
                    // Other commands silently discarded
                }
            }
        }

        taskYIELD();
    }

    free(captureBuffer);
    free(outputBuffer);
    core1OwnsLED = false;
    audioTaskRunning = false;
    Serial.println("[Core1] Audio pipeline task stopped");
    xSemaphoreGive(taskExitSemaphore);
    vTaskDelete(NULL);
}

// Start audio pipeline task on Core 1 (or reuse if already running)
void startAudioCaptureTask() {
    if (audioCaptureTaskHandle != NULL) {
        // Task already alive — it will pick up via isStreaming/streamClient
        return;
    }

    BaseType_t result = xTaskCreatePinnedToCore(
        audioCaptureTask,           // Task function
        "AudioPipeline",            // Name
        8192,                       // Stack size
        NULL,                       // Parameters
        10,                         // Priority (elevated)
        &audioCaptureTaskHandle,    // Task handle
        1                           // Core 1 (PRO_CPU)
    );

    if (result != pdPASS) {
        simplePrintln("[Core1] FATAL: Failed to create audio pipeline task!");
    }
}

// Stop audio pipeline task with confirmed exit via semaphore
void stopAudioCaptureTask() {
    if (audioCaptureTaskHandle != NULL) {
        audioTaskRunning = false;
        // Wait for task to confirm exit (up to 2s)
        if (xSemaphoreTake(taskExitSemaphore, pdMS_TO_TICKS(2000)) != pdTRUE) {
            Serial.println("[Core0] WARNING: Audio task did not exit within 2s");
        }
        audioCaptureTaskHandle = NULL;
    }
}

// Request Core 1 to stop streaming and clean up the socket.
// Core 0 must NOT touch the WiFiClient while Core 1 owns it.
// Returns true if Core 1 confirmed cleanup, false on timeout.
bool requestStreamStop(const char* reason) {
    // Early return if not streaming
    if (!isStreaming && streamClient == NULL) return true;

    Serial.printf("[Core0] requestStreamStop: %s\n", reason);

    // Signal Core 1 to stop
    stopStreamRequested = true;
    __asm__ __volatile__("memw" ::: "memory");  // Xtensa memory barrier

    // Poll for Core 1 confirmation (up to 3s)
    unsigned long deadline = millis() + 3000;
    while (!streamCleanupDone && millis() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (streamCleanupDone) {
        // Core 1 confirmed — safe to proceed
        isStreaming = false;
        streamClient = NULL;
        stopStreamRequested = false;
        streamCleanupDone = false;
        Serial.printf("[Core0] Stream stopped cleanly: %s\n", reason);
        return true;
    } else {
        // Timeout — force cleanup
        Serial.printf("[Core0] WARNING: Stream stop timeout, forcing cleanup: %s\n", reason);
        isStreaming = false;
        streamClient = NULL;
        stopStreamRequested = false;
        streamCleanupDone = false;
        core1OwnsLED = false;
        return false;
    }
}

// I2S setup for AtomS3 Lite + Unit Mini PDM (PDM microphone mode)
void setup_i2s_driver() {
    i2s_driver_uninstall(I2S_NUM_0);

    // DMA buffer configuration - smaller for lower latency with 16kHz
    uint16_t dma_buf_len = 60;

    i2s_config_t i2s_config = {
        // PDM mode for Unit Mini PDM microphone
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = currentSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,  // Match original demo exactly
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,   // Match M5 reference PDM example
#if ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4, 1, 0)
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
#else
        .communication_format = I2S_COMM_FORMAT_I2S,
#endif
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = dma_buf_len,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
#if (ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4, 3, 0))
        .mck_io_num = I2S_PIN_NO_CHANGE,
#endif
        .bck_io_num = I2S_PIN_NO_CHANGE,
        .ws_io_num = I2S_CLK_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DATA_IN_PIN
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_clk(I2S_NUM_0, currentSampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

    simplePrintln("I2S ready (PDM mode): " + String(currentSampleRate) + "Hz, gain " +
                  String(currentGainFactor, 1) + ", buffer " + String(currentBufferSize) +
                  " (chunk " + String(effectiveAudioChunkSize()) + ")" +
                  ", shiftBits " + String(i2sShiftBits));
}

static bool writeAll(WiFiClient &client, const uint8_t* data, size_t len, unsigned long timeoutMs) {
    size_t off = 0;
    unsigned long startTime = millis();

    while (off < len) {
        // Check timeout to prevent blocking Core 1
        if (millis() - startTime > timeoutMs) {
            // Drop frame if WiFi is too slow (normal with poor signal)
            return false;
        }

        int w = client.write(data + off, len - off);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}

static uint32_t consecutiveWriteFailures = 0;
static const uint32_t MAX_WRITE_FAILURES = 100;  // Allow ~5s of failures before disconnect

void sendRTPPacket(WiFiClient &client, int16_t* audioData, int numSamples) {
    if (!client.connected()) {
        // Client disconnected — Core 1 owns the socket, close it
        client.stop();
        streamClient = NULL;
        isStreaming = false;
        core1OwnsLED = false;
        return;
    }

    const int maxSamplesPerPacket = (int)effectiveAudioChunkSize();
    int offsetSamples = 0;
    while (offsetSamples < numSamples) {
        const int packetSamples = min(maxSamplesPerPacket, numSamples - offsetSamples);
        const uint16_t payloadSize = (uint16_t)(packetSamples * (int)sizeof(int16_t));
        const uint16_t packetSize = (uint16_t)(12 + payloadSize);
        const unsigned long packetWindowMs = max(40UL, min(140UL, (unsigned long)(((uint32_t)packetSamples * 1000UL) / max((uint32_t)1, currentSampleRate)) * 2UL));

        // RTSP interleaved header: '$' 0x24, channel 0, length
        uint8_t inter[4];
        inter[0] = 0x24;
        inter[1] = 0x00;
        inter[2] = (uint8_t)((packetSize >> 8) & 0xFF);
        inter[3] = (uint8_t)(packetSize & 0xFF);

        // RTP header (12 bytes)
        uint8_t header[12];
        header[0] = 0x80;      // V=2, P=0, X=0, CC=0
        header[1] = 96;        // M=0, PT=96 (dynamic)
        header[2] = (uint8_t)((rtpSequence >> 8) & 0xFF);
        header[3] = (uint8_t)(rtpSequence & 0xFF);
        header[4] = (uint8_t)((rtpTimestamp >> 24) & 0xFF);
        header[5] = (uint8_t)((rtpTimestamp >> 16) & 0xFF);
        header[6] = (uint8_t)((rtpTimestamp >> 8) & 0xFF);
        header[7] = (uint8_t)(rtpTimestamp & 0xFF);
        header[8]  = (uint8_t)((rtpSSRC >> 24) & 0xFF);
        header[9]  = (uint8_t)((rtpSSRC >> 16) & 0xFF);
        header[10] = (uint8_t)((rtpSSRC >> 8) & 0xFF);
        header[11] = (uint8_t)(rtpSSRC & 0xFF);

        int16_t* packetAudio = audioData + offsetSamples;
        // Host->network: per-sample byte-swap (16bit PCM L16 big-endian)
        for (int i = 0; i < packetSamples; ++i) {
            uint16_t s = (uint16_t)packetAudio[i];
            s = (uint16_t)((s << 8) | (s >> 8));
            packetAudio[i] = (int16_t)s;
        }

        bool success = writeAll(client, inter, sizeof(inter), packetWindowMs) &&
                       writeAll(client, header, sizeof(header), packetWindowMs) &&
                       writeAll(client, (uint8_t*)packetAudio, payloadSize, packetWindowMs);

        if (success) {
            rtpSequence++;
            rtpTimestamp += (uint32_t)packetSamples;
            audioPacketsSent++;
            consecutiveWriteFailures = 0;  // Reset on success
            offsetSamples += packetSamples;
        } else {
            audioPacketsDropped++;
            consecutiveWriteFailures++;

            if (consecutiveWriteFailures >= MAX_WRITE_FAILURES) {
                // Sustained failure — Core 1 owns the socket, close it
                Serial.printf("[Core1] %u consecutive write failures, disconnecting\n", consecutiveWriteFailures);
                consecutiveWriteFailures = 0;
                client.stop();
                streamClient = NULL;
                isStreaming = false;
                core1OwnsLED = false;
            }
            return;
        }
    }
}

// ================== CORE 0: NO AUDIO STREAMING ==================
// Audio streaming is now handled entirely on Core 1
// Core 0 only manages client connections and RTSP protocol
// (streamAudio function removed - handled by Core 1 audioCaptureTask)

// RTSP handling
void handleRTSPCommand(WiFiClient &client, String request) {
    String cseq = "1";
    int cseqPos = request.indexOf("CSeq: ");
    if (cseqPos >= 0) {
        cseq = request.substring(cseqPos + 6, request.indexOf("\r", cseqPos));
        cseq.trim();
    }

    lastRTSPActivity = millis();

    if (request.startsWith("OPTIONS")) {
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n");

    } else if (request.startsWith("DESCRIBE")) {
        String ip = WiFi.localIP().toString();
        String sdp = "v=0\r\n";
        sdp += "o=- 0 0 IN IP4 " + ip + "\r\n";
        sdp += "s=ESP32 RTSP Mic (" + String(currentSampleRate) + "Hz, 16-bit PCM)\r\n";
        // better compatibility: include actual IP
        sdp += "c=IN IP4 " + ip + "\r\n";
        sdp += "t=0 0\r\n";
        sdp += "m=audio 0 RTP/AVP 96\r\n";
        sdp += "a=rtpmap:96 L16/" + String(currentSampleRate) + "/1\r\n";
        sdp += "a=control:track1\r\n";

        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Content-Type: application/sdp\r\n");
        client.print("Content-Base: rtsp://" + ip + ":8554/audio/\r\n");
        client.print("Content-Length: " + String(sdp.length()) + "\r\n\r\n");
        client.print(sdp);

    } else if (request.startsWith("SETUP")) {
        if (rtspSessionId.length() == 0) {
            rtspSessionId = String(random(100000000, 999999999));
        }
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        // Large timeout reduces ffmpeg keepalive frequency (keepalive sent at timeout/2)
        client.print("Session: " + rtspSessionId + ";timeout=86400\r\n");
        client.print("Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=\"PLAY\";ssrc=" +
                     String(rtpSSRC, HEX) + "\r\n\r\n");

    } else if (request.startsWith("PLAY")) {
        // Send PLAY response FIRST (still on Core 0, Core 1 not started yet)
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + "\r\n");
        client.print("Range: npt=0.000-\r\n\r\n");

        rtpSequence = 0;
        rtpTimestamp = 0;
        audioPacketsSent = 0;
        audioPacketsDropped = 0;
        lastStatsReset = millis();
        lastRtspPlayMs = millis();
        rtspPlayCount++;

        // Initialize stream stop flags before handing off
        stopStreamRequested = false;
        streamCleanupDone = false;
        core1OwnsLED = true;
        __asm__ __volatile__("memw" ::: "memory");

        // Hand off client to Core 1 via pointer
        streamClient = &rtspClient;
        isStreaming = true;

        // Start audio capture task
        startAudioCaptureTask();

        // Core 1 now owns LED during streaming
        simplePrintln("STREAMING STARTED");

    } else if (request.startsWith("TEARDOWN")) {
        // Stop streaming FIRST — Core 1 owns the socket during streaming
        if (isStreaming) {
            requestStreamStop("TEARDOWN");
        }

        // Now Core 0 owns the socket again, safe to send response
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + "\r\n\r\n");

        if (!core1OwnsLED) {
            if (ledMode > 0) setStatusLed(CRGB(0, 0, 128));
            else setStatusLed(CRGB(0, 0, 0));
        }
        simplePrintln("STREAMING STOPPED");
    } else if (request.startsWith("GET_PARAMETER")) {
        // Many RTSP clients send GET_PARAMETER as keep-alive.
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n\r\n");
    }
}

// RTSP processing (runs on Core 0, only called when !isStreaming)
void processRTSP(WiFiClient &client) {
    if (!client.connected()) return;

    if (client.available()) {
        int available = client.available();
        int spaceLeft = sizeof(rtspParseBuffer) - rtspParseBufferPos - 1;

        if (available > spaceLeft) {
            available = spaceLeft;
        }

        if (available <= 0) {
            static unsigned long lastOverflowWarning = 0;
            if (millis() - lastOverflowWarning > 5000) {
                simplePrintln("RTSP buffer full - resetting");
                lastOverflowWarning = millis();
            }
            rtspParseBufferPos = 0;
            return;
        }

        client.read(rtspParseBuffer + rtspParseBufferPos, available);
        rtspParseBufferPos += available;
        rtspParseBuffer[rtspParseBufferPos] = '\0';

        char* endOfHeader = strstr((char*)rtspParseBuffer, "\r\n\r\n");
        if (endOfHeader != nullptr) {
            *endOfHeader = '\0';
            String request = String((char*)rtspParseBuffer);

            handleRTSPCommand(client, request);

            int headerLen = (endOfHeader - (char*)rtspParseBuffer) + 4;
            int remaining = rtspParseBufferPos - headerLen;
            if (remaining > 0) {
                memmove(rtspParseBuffer, rtspParseBuffer + headerLen, remaining);
            }
            rtspParseBufferPos = remaining;
        }
    }
}


// Web UI is a separate module (WebUI.*)

void setup() {
    FastLED.addLeds<WS2812, WS2812_LED_PIN, GRB>(statusLed, 1);
    FastLED.setBrightness(32);
    setStatusLed(CRGB(0, 0, 0));

    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== ESP32 RTSP Mic Starting ===");
    Serial.println("Board: M5Stack AtomS3 Lite");

    // Create task exit semaphore for confirmed Core 1 task shutdown
    taskExitSemaphore = xSemaphoreCreateBinary();

    // Set LED to indicate startup
    setStatusLed(CRGB(128, 128, 0));  // Yellow for startup

    // (4) seed for random(): combination of time and unique MAC
    randomSeed((uint32_t)micros() ^ (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF));
    for (uint16_t i = 0; i < TELEMETRY_HISTORY_LEN; ++i) {
        telemetryTempDeciC[i] = INT16_MIN;
    }

    bootTime = millis(); // Store boot time
    rtpSSRC = (uint32_t)random(1, 0x7FFFFFFF);
    Serial.println("Random seed initialized");

    // Enable external antenna (for XIAO ESP32-C6).
    // NOTE: Commented out for M5Stack STAMP S3 - no external antenna control needed
    // pinMode(3, OUTPUT);
    // digitalWrite(3, LOW);
    // Serial.println("RF switch control enabled (GPIO3 LOW)");
    // pinMode(14, OUTPUT);
    // digitalWrite(14, HIGH);
    // Serial.println("External antenna selected (GPIO14 HIGH)");

    // Load settings from flash
    Serial.println("Loading settings...");
    loadAudioSettings();
    Serial.println("Settings loaded");

    // Note: Audio buffers now allocated by Core 1 task (not in main)
    Serial.println("Audio buffers will be allocated by Core 1 pipeline task");

    // WiFi optimization for stable streaming
    Serial.println("Initializing WiFi...");
    WiFi.setSleep(false);

    WiFi.setHostname(DEFAULT_NETWORK_HOSTNAME);

    WiFiManager wm;
    wm.setConnectTimeout(60);
    wm.setConfigPortalTimeout(180);
    if (!wm.autoConnect("ESP32-RTSP-Mic-AP")) {
        simplePrintln("WiFi failed, restarting...");
        ESP.restart();
    }

    simplePrintln("WiFi connected: " + WiFi.localIP().toString());

    // NTP time sync (EST = UTC-5, no DST)
    configTime(-5 * 3600, 0, "pool.ntp.org");
    Serial.print("Waiting for NTP time sync...");
    time_t now = 0;
    for (int i = 0; i < 20 && now < 100000; i++) {
        delay(250);
        time(&now);
    }
    if (now > 100000) {
        struct tm ti;
        localtime_r(&now, &ti);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
        Serial.printf(" OK: %s EST\n", buf);
    } else {
        Serial.println(" failed (will use uptime)");
    }

    // Apply configured WiFi TX power after connect (logs once on change)
    applyWifiTxPower(true);

    const char* wifiHostname = WiFi.getHostname();
    if (wifiHostname && wifiHostname[0] != '\0') {
        networkHostname = String(wifiHostname);
    }

    // mDNS: allows rtsp://<hostname>.local:8554/audio
    if (MDNS.begin(networkHostname.c_str())) {
        MDNS.addService("rtsp", "tcp", 8554);
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("arduino", "tcp", OTA_PORT);
        simplePrintln("mDNS: " + networkHostname + ".local");
    }

    setupArduinoOta();

    Serial.println("Setting up I2S driver...");
    setup_i2s_driver();
    Serial.println("I2S driver ready");

    // Start Core 1 audio pipeline now so raw I2S diagnostics are available even before PLAY.
    startAudioCaptureTask();
    Serial.println("Dual-core audio ready (Core 1 pipeline running for always-on diagnostics)");

    Serial.println("Updating highpass coefficients...");
    updateHighpassCoeffs();
    Serial.println("Highpass coefficients updated");

    if (!overheatLatched) {
        rtspServer.begin();
        rtspServer.setNoDelay(true);
        rtspServerEnabled = true;
    } else {
        rtspServerEnabled = false;
        rtspServer.stop();
    }
    // Web UI
    webui_begin();

    lastStatsReset = millis();
    lastRTSPActivity = millis();
    lastMemoryCheck = millis();
    lastPerformanceCheck = millis();
    lastWiFiCheck = millis();
    minFreeHeap = ESP.getFreeHeap();
    float initialTemp = temperatureRead();
    if (isTemperatureValid(initialTemp)) {
        maxTemperature = initialTemp;
        lastTemperatureC = initialTemp;
        lastTemperatureValid = true;
        overheatSensorFault = false;
    } else {
        maxTemperature = 0.0f;
        lastTemperatureC = 0.0f;
        lastTemperatureValid = false;
        overheatSensorFault = true;
        overheatLastReason = "Thermal protection disabled: temperature sensor unavailable.";
        overheatLastTimestamp = "";
        overheatTripTemp = 0.0f;
        overheatTriggeredAt = 0;
        persistOverheatNote();
        simplePrintln("WARNING: Temperature sensor unavailable at startup. Thermal protection paused.");
    }

    setCpuFrequencyMhz(cpuFrequencyMhz);
    simplePrintln("CPU frequency set to " + String(cpuFrequencyMhz) + " MHz for optimal thermal/performance balance");

    if (!overheatLatched) {
        simplePrintln("RTSP server ready on port 8554");
        simplePrintln("RTSP URL: rtsp://" + WiFi.localIP().toString() + ":8554/audio");
        simplePrintln("RTSP URL: rtsp://" + networkHostname + ".local:8554/audio");
        // Set LED to blue when ready (green reserved for level indicator)
        if (ledMode > 0) setStatusLed(CRGB(0, 0, 128));
        else setStatusLed(CRGB(0, 0, 0));
    } else {
        simplePrintln("RTSP server paused due to thermal latch. Clear via Web UI before resuming streaming.");
        // Set LED to red when latched
        setStatusLed(CRGB(128, 0, 0));
    }
    simplePrintln("Web UI: http://" + WiFi.localIP().toString() + "/");
}

void loop() {
    webui_handleClient();
    ArduinoOTA.handle();

    if (millis() - lastTempCheck > 5000) { // 5 s
        checkTemperature();
        lastTempCheck = millis();
    }

    if (millis() - lastTelemetrySampleMs > 3000) {
        recordTelemetrySample(audioPipelineLoadPct, lastTemperatureC, lastTemperatureValid);
        lastTelemetrySampleMs = millis();
    }

    // Heap monitoring (every 10 minutes — useful for detecting leaks in long deployments)
    if (millis() - lastMemoryCheck > 600000) { // 10 min
        uint32_t currentHeap = ESP.getFreeHeap();
        if (currentHeap < minFreeHeap) minFreeHeap = currentHeap;
        Serial.printf("[Heap] Current: %u KB, Min: %u KB\n", currentHeap / 1024, minFreeHeap / 1024);
        lastMemoryCheck = millis();
    }

    if (millis() - lastPerformanceCheck > (performanceCheckInterval * 60000UL)) {
        checkPerformance();
        lastPerformanceCheck = millis();
    }

    if (millis() - lastWiFiCheck > 30000) { // 30 s
        checkWiFiHealth(); // without TX power log spam
        lastWiFiCheck = millis();
    }

    checkScheduledReset();

    // RTSP idle timeout — disconnect clients that connect but never stream (60s)
    if (rtspClient && rtspClient.connected() && !isStreaming) {
        if (millis() - lastRTSPActivity > 60000) {
            simplePrintln("RTSP idle timeout — disconnecting");
            rtspClient.stop();
            rtspParseBufferPos = 0;
        }
    }

    // RTSP client management (Core 0) — clear phase separation
    static bool wasStreaming = false;
    if (rtspServerEnabled) {
        // Phase: detect disconnect (Core 1 cleared isStreaming after self-disconnect)
        if (wasStreaming && !isStreaming) {
            // Core 1 already closed the socket — just update LED and log
            if (!core1OwnsLED) {
                if (ledMode > 0) setStatusLed(CRGB(0, 0, 128));
                else setStatusLed(CRGB(0, 0, 0));
            }
            unsigned long sessionSec = (millis() - lastRtspPlayMs) / 1000;
            simplePrintln("RTSP client disconnected (session: " + String(sessionSec) + "s, dropped: " +
                         String(audioPacketsDropped) + ", RSSI: " + String(WiFi.RSSI()) + " dBm)");
        }
        wasStreaming = isStreaming;

        // Phase: accept new client (only when not streaming)
        if (!isStreaming) {
            if (!rtspClient || !rtspClient.connected()) {
                WiFiClient newClient = rtspServer.available();
                if (newClient) {
                    rtspClient = newClient;
                    rtspClient.setNoDelay(true);
                    rtspParseBufferPos = 0;
                    lastRTSPActivity = millis();
                    lastRtspClientConnectMs = millis();
                    rtspConnectCount++;
                    simplePrintln("New RTSP client connected");
                }
            }

            // Phase: RTSP negotiation (only when not streaming)
            if (rtspClient && rtspClient.connected()) {
                processRTSP(rtspClient);
            }
        }
    } else {
        // RTSP server disabled (overheat lockout)
        if (isStreaming) {
            requestStreamStop("server disabled");
            stopAudioCaptureTask();
        }
        if (!core1OwnsLED) {
            if (overheatLatched) {
                setStatusLed(CRGB(128, 0, 0));
            } else {
                if (ledMode > 0) setStatusLed(CRGB(0, 0, 128));
                else setStatusLed(CRGB(0, 0, 0));
            }
        }
    }
    // Handle deferred reboot/reset safely here
    if (scheduledRebootAt != 0 && millis() >= scheduledRebootAt) {
        if (scheduledFactoryReset) {
            resetToDefaultSettings();
        }
        delay(50);
        ESP.restart();
    }
}
