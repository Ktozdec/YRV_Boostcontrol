#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Adafruit_ADS1X15.h>
#include <Wire.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Adafruit_MCP4725.h>
#include "esp_gap_ble_api.h"
#include "driver/pcnt.h"
#include "esp_task_wdt.h"
#include "esp_system.h"

#define WATCHDOG_TIMEOUT_SEC 5
#define BLE_MAX_MESSAGE_LEN 180
#define BLE_NOTIFY_INTERVAL_MS 125
#define SETTINGS_RESEND_INTERVAL_MS 60
#define KLINE_BAUD 10400
#define KLINE_FRAME_GAP_MS 25
#define KLINE_MAX_FRAME_LEN 48

constexpr uint8_t ADS1115_I2C_ADDR = 0x48;
constexpr float ADS1115_MULTIPLIER_6V144 = 0.0001875f;
constexpr uint16_t ADS1115_CFG_OS_SINGLE = 0x8000;
constexpr uint16_t ADS1115_CFG_PGA_6V144 = 0x0000;
constexpr uint16_t ADS1115_CFG_MODE_SINGLE = 0x0100;
constexpr uint16_t ADS1115_CFG_DR_860 = 0x00E0;
constexpr uint16_t ADS1115_CFG_COMP_DISABLE = 0x0003;
constexpr uint16_t ADS1115_CFG_BASE =
    ADS1115_CFG_OS_SINGLE |
    ADS1115_CFG_PGA_6V144 |
    ADS1115_CFG_MODE_SINGLE |
    ADS1115_CFG_DR_860 |
    ADS1115_CFG_COMP_DISABLE;
constexpr float SOFT_LIMP_BOOST_BAR = 1.05f;
constexpr float HARD_LIMP_BOOST_BAR = 1.15f;

// =============================== ADAPTIVE TUNING ===============================
// One place for the knobs that shape boost behaviour and self-learning.
// (Protection thresholds are SOFT_LIMP_BOOST_BAR / HARD_LIMP_BOOST_BAR above.)

// -- Control loop timing & robustness --
constexpr uint32_t CONTROL_PERIOD_MS = 50;     // 20 Hz. 10 ms (100 Hz) starved the software tach ISR → RPM read half. Keep at 50.
constexpr int   ADC_FAIL_LIMIT       = 4;      // consecutive ADC failures before forcing limp (~50 ms)
constexpr uint32_t MAP_SAVE_INTERVAL_MS = 60000; // NVS map flush no more than once per minute (when changed)

// -- Diagnostics --
// Set to false to STOP writing the MCP4725 DAC: an A/B test for whether the DAC writes (0x60)
// disturb the I2C bus and break the ADS1115 reads. (ECU loses the MAP passthrough while off —
// for a stationary bench test only.)
constexpr bool DAC_OUTPUT_ENABLED = true;

// -- PID base gains (also power-on defaults; a saved value in NVS overrides them) --
constexpr float DEFAULT_KP = 28.0f;
constexpr float DEFAULT_KI = 16.0f;
constexpr float DEFAULT_KD = 10.0f;
constexpr float INTEGRAL_LIMIT      = 30.0f;   // anti-windup clamp on the integral term
constexpr float TARGET_FILTER_TAU_S = 0.35f;   // smoothing time constant for the boost target

// -- Self-learning (adaptive feed-forward map) --
constexpr float DEFAULT_LEARN_RATE  = 0.10f;   // lA: fraction of the steady integral bias baked into the map per update
constexpr float LEARN_RATE_MIN      = 0.02f;
constexpr float LEARN_RATE_MAX      = 0.30f;
constexpr float MAP_LEARN_MAX_STEP  = 0.8f;    // max % duty baked into the map per update
constexpr float INTEGRAL_TRANSFER   = 0.85f;   // fraction of the offloaded bias removed from the integral
constexpr float MAP_MAX_RPM_DELTA   = 18.0f;   // sanity slope limit between adjacent RPM cells (% duty)
constexpr float MAP_DUTY_MIN        = 0.0f;
constexpr float MAP_DUTY_MAX        = 85.0f;

// -- Learning window: when it is safe to adapt the map --
constexpr float LEARN_MAX_TPS_DELTA   = 4.0f;
constexpr float LEARN_MAX_RPM_DELTA   = 250.0f;
constexpr float LEARN_MAX_BOOST_DELTA = 0.08f;
constexpr float LEARN_MAX_ERR         = 0.30f;
constexpr float LEARN_MIN_TPS         = 12.0f;
constexpr float LEARN_MIN_RPM         = 1600.0f;
constexpr float LEARN_MIN_SPEED       = 6.0f;
constexpr uint32_t LEARN_HOLD_MS      = 300;

// -- Transient feel: setpoint feed-forward + RPM gain scheduling --
constexpr float SETPOINT_FF_GAIN = 3.0f;   // % duty per (bar/s) of rising target — anticipates spool
constexpr float SETPOINT_FF_CAP  = 8.0f;   // clamp on setpoint feed-forward (% duty)
constexpr float GAIN_SPOOL    = 1.30f;     // kP multiplier at/below GAIN_RPM_LOW (aggressive spool)
constexpr float GAIN_TOP      = 0.85f;     // kP multiplier at/above GAIN_RPM_HIGH (gentle, no overshoot)
constexpr float GAIN_RPM_LOW  = 2500.0f;
constexpr float GAIN_RPM_MID  = 4000.0f;   // multiplier = 1.0 here
constexpr float GAIN_RPM_HIGH = 6000.0f;
// ===============================================================================

const float ATMOS_MIN_VOLTS    = 2.40f;
const float ATMOS_MAX_VOLTS    = 2.70f;
const float DEFAULT_OFFSET     = 2.57f;
const float DAC_REFERENCE_VOLTAGE = 5.02f;

const float FILTER_NEW = 0.70f;
const float FILTER_OLD = 0.30f;
float filtered_map_volts = 0.0f;

const char *ssid = "YRV_Boost_Pro";
const char *password = "";

Adafruit_ADS1115 ads;
Adafruit_MCP4725 dac;
WebServer server(80);
Preferences prefs;

SemaphoreHandle_t dataMutex;
SemaphoreHandle_t mapMutex;
SemaphoreHandle_t configMutex;
SemaphoreHandle_t bleMutex;
SemaphoreHandle_t klineMutex;
TaskHandle_t storageTaskHandle = nullptr;

const int rpmPin = 18;
const int vssPin = 19;
const int sdaPin = 23;
const int sclPin = 22;
const int solPin = 25;
const int klineRxPin = 16;
const int klineTxPin = 17;

#define PCNT_UNIT PCNT_UNIT_0

const int pwmFreq = 30;
const int pwmRes = 8;
volatile int testDuty = 0;
volatile float latchedGearBoostTrim = 0.20f;

struct PID_Config {
    float kP = DEFAULT_KP;
    float kI = DEFAULT_KI;
    float kD = DEFAULT_KD;
    float learnCoeff = DEFAULT_LEARN_RATE;   // integral-offload rate (see ADAPTIVE TUNING above)
    float integral = 0.0f;
    float lastError = 0.0f;
    float lastMeas = 0.0f;      // last boost reading, for derivative-on-measurement (no setpoint kick)
    float filteredDerivative = 0.0f;
} pid;

struct RuntimeConfig {
    float offsetPIM = 2.56f;
    float scalePIM = 0.55f;
    float pulsesPerRev = 2.0f;
    float offsetVTA = 0.42f;
    float targetBoost = 0.80f;
    float limitBoostBar = 0.95f;
    float vssPulsesPerRev = 5.18f;
    int tireW = 195;
    int tireA = 55;
    int tireR = 15;
    float wheelSizeM = 1.87f;
} cfg;

const int NUM_RPM_BINS = 13;
const int NUM_TPS_BINS = 6;

float rpmBins[NUM_RPM_BINS] = {1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000, 5500, 6000, 6500, 7000, 7500};
float tpsBins[NUM_TPS_BINS] = {10, 25, 40, 60, 80, 100};

float dutyMap2D[NUM_TPS_BINS][NUM_RPM_BINS];
float confidence[NUM_TPS_BINS][NUM_RPM_BINS];
uint16_t cellSamples[NUM_TPS_BINS][NUM_RPM_BINS];
bool mapCellDirty = false;
bool confidenceDirty = false;

float currentBaseDuty = 40.0f;
float currentOutDuty = 0.0f;

unsigned long lastMapSaveMs = 0;
volatile bool mapNeedsSaving = false;
volatile bool settingsNeedsSaving = false;
volatile bool sendSettingsRequested = false;
volatile bool odometerNeedsSaving = false;
volatile bool otaModeEnabled = false;
volatile bool forceMapSaveRequested = false;
volatile bool forceSettingsSaveRequested = false;
volatile bool forceOdometerSaveRequested = false;
volatile bool klineBridgeBusy = false;

enum RebootRequest : uint8_t {
    REBOOT_NONE = 0,
    REBOOT_TO_NORMAL = 1,
    REBOOT_TO_OTA = 2
};

volatile RebootRequest rebootRequest = REBOOT_NONE;

double totalDistanceKm = 0.0;
double stationaryEngineHours = 0.0;
volatile uint32_t pendingVssPulses = 0;
portMUX_TYPE vssMux = portMUX_INITIALIZER_UNLOCKED;

struct SensorData {
    float boost;
    float tps;
    float rpm;
    float speed;
    float maxBoost;
    float minBoost;
    float maxRPM;
    float maxSpeed;
    float rawPIM;
    float rawVTA;
};

SensorData sensors;

struct KLineState {
    uint32_t byteCount = 0;
    uint32_t frameCount = 0;
    uint32_t overflowCount = 0;
    uint32_t lastByteAtMs = 0;
    uint8_t lastByte = 0;
    uint8_t lastFrameLen = 0;
    uint8_t lastFrame[KLINE_MAX_FRAME_LEN] = {0};
};

KLineState klineState;

enum Mode { NORMAL, SOFT_LIMP, HARD_LIMP };
volatile Mode systemMode = NORMAL;

portMUX_TYPE rpmMux = portMUX_INITIALIZER_UNLOCKED;
volatile unsigned long rpmPeriodUs = 0;
volatile unsigned long lastRpmMicros = 0;
volatile uint32_t g_rpmEdges = 0;   // diagnostic: total accepted tach edges

BLEServer *pServer = nullptr;
BLECharacteristic *pTxCharacteristic = nullptr;
volatile bool deviceConnected = false;
volatile uint16_t bleConnId = 0;   // current connection id, for per-link MTU lookup
volatile int g_adcFailStreak = 0;  // consecutive ADS1115 read failures (0 = bus healthy)
volatile bool g_ch0ok = false;     // last raw read status, channel 0 (MAP), pre-debounce
volatile bool g_ch1ok = false;     // last raw read status, channel 1 (TPS), pre-debounce
volatile float g_ch0v = 0.0f;      // last raw volts, channel 0 (MAP), pre-debounce
volatile float g_ch1v = 0.0f;      // last raw volts, channel 1 (TPS), pre-debounce

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

struct LearningState {
    float lastTps = 0.0f;
    float lastRpm = 0.0f;
    float lastBoost = 0.0f;
    uint32_t stableSinceMs = 0;
} learningState;

float filteredDynamicTarget = 0.80f;
bool filteredDynamicTargetInitialized = false;
float prevControlTarget = 0.80f;   // previous filtered target, for setpoint feed-forward

// Target-boost shape vs RPM (multiplier on cfg.targetBoost). Flat through the mid-range,
// a gentle taper up top to protect the turbo and smooth the limiter approach. Adjust to taste.
float targetShapeCurve[NUM_RPM_BINS] = {
    1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f,
    1.00f, 1.00f, 1.00f, 0.99f, 0.98f, 0.97f
};

inline float constrainFloat(float x, float a, float b) {
    return x < a ? a : (x > b ? b : x);
}

bool isFiniteFloat(float value) {
    return !isnan(value) && !isinf(value);
}

float computeGearBoostTrim(float rpm, float speed, float previousTrim) {
    if (speed <= 10.0f) return 0.20f;

    const float gearRatio = rpm / max(speed, 1.0f);

    if (gearRatio > 105.0f) return 0.20f;
    return 0.0f;
}

bool parseIntStrict(const String &s, int minV, int maxV, int &out) {
    char *end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || v < minV || v > maxV) return false;
    out = static_cast<int>(v);
    return true;
}

bool parseFloatStrict(const String &s, float minV, float maxV, float &out) {
    char *end = nullptr;
    float v = strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !isFiniteFloat(v) || v < minV || v > maxV) return false;
    out = v;
    return true;
}

bool takeMutex(SemaphoreHandle_t mutex, TickType_t timeout = pdMS_TO_TICKS(100)) {
    return mutex != nullptr && xSemaphoreTake(mutex, timeout) == pdTRUE;
}

RuntimeConfig snapshotConfig() {
    RuntimeConfig localCfg = cfg;
    if (takeMutex(configMutex, pdMS_TO_TICKS(20))) {
        localCfg = cfg;
        xSemaphoreGive(configMutex);
    }
    return localCfg;
}

void notifyStorageTask() {
    if (storageTaskHandle != nullptr) {
        xTaskNotifyGive(storageTaskHandle);
    }
}

void calcWheelSizeLocked() {
    float diameterMm = (cfg.tireR * 25.4f) + 2.0f * (cfg.tireW * (cfg.tireA / 100.0f));
    cfg.wheelSizeM = (diameterMm * PI) / 1000.0f;
}

void sanitizeMapProfileLocked() {
    for (int t = 0; t < NUM_TPS_BINS; t++) {
        for (int r = 0; r < NUM_RPM_BINS; r++) {
            dutyMap2D[t][r] = constrainFloat(dutyMap2D[t][r], MAP_DUTY_MIN, MAP_DUTY_MAX);
            confidence[t][r] = constrainFloat(confidence[t][r], 0.05f, 1.0f);
            if (r > 0) {
                // Sanity slope limit between adjacent RPM cells — guards against corrupt
                // jumps without fighting legitimately learned steps.
                float delta = dutyMap2D[t][r] - dutyMap2D[t][r - 1];
                if (delta > MAP_MAX_RPM_DELTA) dutyMap2D[t][r] = dutyMap2D[t][r - 1] + MAP_MAX_RPM_DELTA;
                if (delta < -MAP_MAX_RPM_DELTA) dutyMap2D[t][r] = dutyMap2D[t][r - 1] - MAP_MAX_RPM_DELTA;
            }
        }
    }
}

void initDefaultMapLocked() {
    // Rows = TPS {10,25,40,60,80,100}, Cols = RPM {1500..7500 step 500}.
    // Higher base duty at light load / low RPM (hold wastegate to spool), gentle taper up top.
    const float defaults[NUM_TPS_BINS][NUM_RPM_BINS] = {
        {62, 62, 62, 62, 62, 60, 58, 57, 56, 52, 52, 52, 50},
        {58, 58, 56, 55, 53, 52, 50, 48, 47, 45, 43, 42, 42},
        {55, 55, 55, 52, 50, 50, 48, 45, 45, 42, 40, 40, 39},
        {50, 50, 49, 47, 45, 44, 42, 42, 40, 40, 38, 37, 36},
        {46, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35},
        {43, 43, 42, 40, 40, 40, 38, 38, 38, 36, 35, 35, 34}
    };

    memcpy(dutyMap2D, defaults, sizeof(dutyMap2D));
    for (int t = 0; t < NUM_TPS_BINS; t++) {
        for (int r = 0; r < NUM_RPM_BINS; r++) {
            confidence[t][r] = 0.5f;
            cellSamples[t][r] = 0;
        }
    }
}

bool isMapPlausibleLocked() {
    int populatedCells = 0;
    float maxValue = 0.0f;

    for (int t = 0; t < NUM_TPS_BINS; t++) {
        for (int r = 0; r < NUM_RPM_BINS; r++) {
            float cell = dutyMap2D[t][r];
            if (!isFiniteFloat(cell)) return false;
            if (cell > 1.0f) populatedCells++;
            if (cell > maxValue) maxValue = cell;
        }
    }

    return populatedCells >= 8 && maxValue >= 15.0f;
}

void sendBleText(const char *text) {
    if (!deviceConnected || pTxCharacteristic == nullptr || text == nullptr) return;
    if (!takeMutex(bleMutex, pdMS_TO_TICKS(20))) return;

    const size_t len = strlen(text);

    // A single notify() only delivers up to (MTU-3) bytes — the rest, including the framing '\n',
    // is silently dropped. The long settings ("S") packet is bigger than the telemetry ("T") packet,
    // so on a small negotiated MTU telemetry arrives but settings get truncated and never parse.
    // Split into MTU-sized chunks; the app reassembles them by the '\n' delimiter.
    uint16_t mtu = (pServer != nullptr) ? pServer->getPeerMTU(bleConnId) : 0;
    size_t chunk = (mtu > 23) ? static_cast<size_t>(mtu - 3) : 20;

    size_t offset = 0;
    while (offset < len) {
        size_t n = (len - offset < chunk) ? (len - offset) : chunk;
        pTxCharacteristic->setValue(reinterpret_cast<const uint8_t *>(text + offset), n);
        pTxCharacteristic->notify();
        offset += n;
        if (offset < len) vTaskDelay(pdMS_TO_TICKS(4));   // let the stack flush between chunks
    }
    xSemaphoreGive(bleMutex);
}

void sendBleAck(const char *cmd) {
    char buffer[80];
    snprintf(buffer, sizeof(buffer), "{\"ack\":\"%s\",\"ok\":true}\n", cmd);
    sendBleText(buffer);
}

void sendBleError(const char *cmd, const char *reason) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "{\"ack\":\"%s\",\"ok\":false,\"err\":\"%s\"}\n", cmd, reason);
    sendBleText(buffer);
}

void klineFormatFrameHex(const KLineState &state, char *out, size_t outSize) {
    if (out == nullptr || outSize == 0) return;
    out[0] = '\0';

    size_t used = 0;
    for (uint8_t i = 0; i < state.lastFrameLen && used + 3 < outSize; i++) {
        int written = snprintf(out + used, outSize - used, "%02X", state.lastFrame[i]);
        if (written <= 0) break;
        used += static_cast<size_t>(written);
        if (i + 1 < state.lastFrameLen && used + 2 < outSize) {
            out[used++] = ' ';
            out[used] = '\0';
        }
    }
}

void klineCommitFrame(const uint8_t *frame, uint8_t len, bool overflow) {
    if (frame == nullptr || len == 0) return;

    if (takeMutex(klineMutex, pdMS_TO_TICKS(10))) {
        klineState.frameCount++;
        klineState.lastFrameLen = (len < KLINE_MAX_FRAME_LEN) ? len : KLINE_MAX_FRAME_LEN;
        memcpy(klineState.lastFrame, frame, klineState.lastFrameLen);
        if (overflow) klineState.overflowCount++;
        xSemaphoreGive(klineMutex);
    }

    char hex[KLINE_MAX_FRAME_LEN * 3] = {0};
    KLineState snapshot;
    if (takeMutex(klineMutex, pdMS_TO_TICKS(10))) {
        snapshot = klineState;
        xSemaphoreGive(klineMutex);
        klineFormatFrameHex(snapshot, hex, sizeof(hex));
        Serial.printf("KLINE frame len=%u data=%s%s\n", snapshot.lastFrameLen, hex, overflow ? " overflow" : "");
    }
}

KLineState snapshotKLine() {
    KLineState local = {};
    if (takeMutex(klineMutex, pdMS_TO_TICKS(10))) {
        local = klineState;
        xSemaphoreGive(klineMutex);
    }
    return local;
}

bool parseHexCommand(const String &hex, uint8_t *out, uint8_t &outLen, uint8_t maxLen) {
    outLen = 0;
    int highNibble = -1;

    for (uint16_t i = 0; i < hex.length(); i++) {
        char c = hex.charAt(i);
        if (c == ' ' || c == ':' || c == '-' || c == ',') continue;

        int value = -1;
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        else return false;

        if (highNibble < 0) {
            highNibble = value;
        } else {
            if (outLen >= maxLen) return false;
            out[outLen++] = static_cast<uint8_t>((highNibble << 4) | value);
            highNibble = -1;
        }
    }

    return highNibble < 0 && outLen > 0;
}

void sendKLineRequest(const String &hex) {
    uint8_t tx[KLINE_MAX_FRAME_LEN] = {0};
    uint8_t txLen = 0;
    if (!parseHexCommand(hex, tx, txLen, KLINE_MAX_FRAME_LEN)) {
        sendBleError("KREQ", "bad_hex");
        return;
    }

    if (klineBridgeBusy) {
        sendBleError("KREQ", "busy");
        return;
    }

    klineBridgeBusy = true;
    while (Serial2.available() > 0) Serial2.read();
    Serial2.write(tx, txLen);
    Serial2.flush();

    uint8_t rx[KLINE_MAX_FRAME_LEN] = {0};
    uint8_t rxLen = 0;
    bool overflow = false;
    uint32_t startedAtMs = millis();
    uint32_t lastByteAtMs = 0;

    while (millis() - startedAtMs < 350) {
        while (Serial2.available() > 0) {
            uint8_t b = static_cast<uint8_t>(Serial2.read());
            if (rxLen < KLINE_MAX_FRAME_LEN) {
                rx[rxLen++] = b;
            } else {
                overflow = true;
            }
            lastByteAtMs = millis();
        }

        if (rxLen > 0 && millis() - lastByteAtMs >= KLINE_FRAME_GAP_MS) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    klineBridgeBusy = false;

    KLineState response = {};
    response.lastFrameLen = rxLen;
    memcpy(response.lastFrame, rx, rxLen);
    char responseHex[KLINE_MAX_FRAME_LEN * 3] = {0};
    klineFormatFrameHex(response, responseHex, sizeof(responseHex));

    char buffer[320];
    snprintf(buffer, sizeof(buffer),
        "{\"KRES\":1,\"ok\":%s,\"txl\":%u,\"rxl\":%u,\"ov\":%u,\"rx\":\"%s\"}\n",
        rxLen > 0 ? "true" : "false",
        txLen,
        rxLen,
        overflow ? 1 : 0,
        responseHex
    );
    sendBleText(buffer);
}

void requestReboot(RebootRequest mode) {
    rebootRequest = mode;
}

void handlePendingReboot() {
    if (rebootRequest == REBOOT_NONE) return;

    RebootRequest request = rebootRequest;
    rebootRequest = REBOOT_NONE;
    otaModeEnabled = (request == REBOOT_TO_OTA);
    prefs.putBool("ota_mode", otaModeEnabled);
    delay(250);
    ESP.restart();
}

void loadMap() {
    if (!takeMutex(mapMutex, pdMS_TO_TICKS(500))) return;

    const size_t mapSize = sizeof(dutyMap2D);
    const size_t confidenceSize = sizeof(confidence);
    const size_t samplesSize = sizeof(cellSamples);

    if (prefs.getBytesLength("map2D") == mapSize) {
        prefs.getBytes("map2D", dutyMap2D, mapSize);
    } else {
        initDefaultMapLocked();
        prefs.putBytes("map2D", dutyMap2D, mapSize);
    }

    if (prefs.getBytesLength("conf2D") == confidenceSize) {
        prefs.getBytes("conf2D", confidence, confidenceSize);
    } else {
        for (int t = 0; t < NUM_TPS_BINS; t++) {
            for (int r = 0; r < NUM_RPM_BINS; r++) {
                confidence[t][r] = 0.5f;
            }
        }
    }

    if (prefs.getBytesLength("samples2D") == samplesSize) {
        prefs.getBytes("samples2D", cellSamples, samplesSize);
    } else {
        memset(cellSamples, 0, samplesSize);
    }

    if (!isMapPlausibleLocked()) {
        initDefaultMapLocked();
        prefs.putBytes("map2D", dutyMap2D, mapSize);
        prefs.putBytes("conf2D", confidence, confidenceSize);
        prefs.putBytes("samples2D", cellSamples, samplesSize);
    }

    sanitizeMapProfileLocked();
    mapCellDirty = false;
    confidenceDirty = false;
    xSemaphoreGive(mapMutex);
}

float getMappedBaseDuty2D(float currentRpm, float currentTps) {
    float rRpm = constrainFloat(currentRpm, rpmBins[0], rpmBins[NUM_RPM_BINS - 1]);
    float rTps = constrainFloat(currentTps, tpsBins[0], tpsBins[NUM_TPS_BINS - 1]);

    int r = 0;
    int t = 0;
    while (r < NUM_RPM_BINS - 2 && rRpm >= rpmBins[r + 1]) r++;
    while (t < NUM_TPS_BINS - 2 && rTps >= tpsBins[t + 1]) t++;

    float rDenom = rpmBins[r + 1] - rpmBins[r];
    float tDenom = tpsBins[t + 1] - tpsBins[t];
    if (fabs(rDenom) < 0.001f) rDenom = 0.001f;
    if (fabs(tDenom) < 0.001f) tDenom = 0.001f;

    float rRatio = (rRpm - rpmBins[r]) / rDenom;
    float tRatio = (rTps - tpsBins[t]) / tDenom;

    float y1 = 0.0f;
    float y2 = 0.0f;
    if (takeMutex(mapMutex, pdMS_TO_TICKS(30))) {
        y1 = dutyMap2D[t][r] + rRatio * (dutyMap2D[t][r + 1] - dutyMap2D[t][r]);
        y2 = dutyMap2D[t + 1][r] + rRatio * (dutyMap2D[t + 1][r + 1] - dutyMap2D[t + 1][r]);
        xSemaphoreGive(mapMutex);
    }
    return constrainFloat(y1 + tRatio * (y2 - y1), 0.0f, 85.0f);
}

// 1D linear interpolation of the target-shape multiplier over the RPM axis.
float getTargetShape(float currentRpm) {
    float rRpm = constrainFloat(currentRpm, rpmBins[0], rpmBins[NUM_RPM_BINS - 1]);
    int r = 0;
    while (r < NUM_RPM_BINS - 2 && rRpm >= rpmBins[r + 1]) r++;
    float denom = rpmBins[r + 1] - rpmBins[r];
    if (fabs(denom) < 0.001f) denom = 0.001f;
    float ratio = (rRpm - rpmBins[r]) / denom;
    return constrainFloat(targetShapeCurve[r] + ratio * (targetShapeCurve[r + 1] - targetShapeCurve[r]), 0.5f, 1.2f);
}

// Gain scheduling: more aggressive in the spool zone, gentler up top to avoid overshoot.
float spoolGainFactor(float currentRpm) {
    if (currentRpm <= GAIN_RPM_LOW) return GAIN_SPOOL;
    if (currentRpm < GAIN_RPM_MID)  return GAIN_SPOOL + (currentRpm - GAIN_RPM_LOW) * (1.00f - GAIN_SPOOL) / (GAIN_RPM_MID - GAIN_RPM_LOW);
    if (currentRpm <= GAIN_RPM_HIGH) return 1.00f + (currentRpm - GAIN_RPM_MID) * (GAIN_TOP - 1.00f) / (GAIN_RPM_HIGH - GAIN_RPM_MID);
    return GAIN_TOP;
}

bool learningWindowStable(const SensorData &d, float err) {
    const uint32_t now = millis();
    const float tpsDelta = fabs(d.tps - learningState.lastTps);
    const float rpmDelta = fabs(d.rpm - learningState.lastRpm);
    const float boostDelta = fabs(d.boost - learningState.lastBoost);

    // Relaxed window: because we now learn the slow integral *bias* (not the noisy
    // instantaneous error), we can safely capture far more of real driving, including
    // the spool region and lighter loads.
    bool locallyStable =
        tpsDelta < LEARN_MAX_TPS_DELTA &&
        rpmDelta < LEARN_MAX_RPM_DELTA &&
        boostDelta < LEARN_MAX_BOOST_DELTA &&
        fabs(err) < LEARN_MAX_ERR &&
        d.tps > LEARN_MIN_TPS &&
        d.rpm > LEARN_MIN_RPM &&
        d.speed > LEARN_MIN_SPEED &&
        systemMode == NORMAL;

    if (!locallyStable) {
        learningState.stableSinceMs = now;
    }

    learningState.lastTps = d.tps;
    learningState.lastRpm = d.rpm;
    learningState.lastBoost = d.boost;

    return locallyStable && (now - learningState.stableSinceMs >= LEARN_HOLD_MS);
}

void smoothCellTowardsNeighborsLocked(int tt, int rr) {
    float sum = dutyMap2D[tt][rr];
    float weight = 1.0f;

    if (rr > 0) { sum += dutyMap2D[tt][rr - 1] * 0.35f; weight += 0.35f; }
    if (rr < NUM_RPM_BINS - 1) { sum += dutyMap2D[tt][rr + 1] * 0.35f; weight += 0.35f; }
    if (tt > 0) { sum += dutyMap2D[tt - 1][rr] * 0.20f; weight += 0.20f; }
    if (tt < NUM_TPS_BINS - 1) { sum += dutyMap2D[tt + 1][rr] * 0.20f; weight += 0.20f; }

    dutyMap2D[tt][rr] = constrainFloat((dutyMap2D[tt][rr] * 0.8f) + ((sum / weight) * 0.2f), 0.0f, 85.0f);
}

// Adaptive feed-forward: bakes the offloaded steady integral bias (dutyDelta, in % duty)
// into the surrounding map cells via an LMS step, weighted by bilinear position.
// `confidence` is now a true accuracy metric (1 = cell predicts well, low = poor),
// reported to the app — it no longer throttles the learning rate to zero.
void learnDutyMap3D(float currentRpm, float currentTps, float dutyDelta, float rawError) {
    float rRpm = constrainFloat(currentRpm, rpmBins[0], rpmBins[NUM_RPM_BINS - 1]);
    float rTps = constrainFloat(currentTps, tpsBins[0], tpsBins[NUM_TPS_BINS - 1]);

    int r = 0;
    int t = 0;
    while (r < NUM_RPM_BINS - 2 && rRpm >= rpmBins[r + 1]) r++;
    while (t < NUM_TPS_BINS - 2 && rTps >= tpsBins[t + 1]) t++;

    float rDenom = rpmBins[r + 1] - rpmBins[r];
    float tDenom = tpsBins[t + 1] - tpsBins[t];
    if (fabs(rDenom) < 0.001f) rDenom = 0.001f;
    if (fabs(tDenom) < 0.001f) tDenom = 0.001f;

    float rRatio = (rRpm - rpmBins[r]) / rDenom;
    float tRatio = (rTps - tpsBins[t]) / tDenom;
    float weights[4] = {
        (1.0f - rRatio) * (1.0f - tRatio),
        rRatio * (1.0f - tRatio),
        (1.0f - rRatio) * tRatio,
        rRatio * tRatio
    };

    // Safety guard against learning on a genuinely wild reading that slipped past the window.
    float boundedDelta = constrainFloat(dutyDelta, -1.0f, 1.0f);
    bool applyMapStep = fabs(boundedDelta) > 0.0001f && fabs(rawError) < 0.35f;

    // Accuracy in [0..1]: 1 when the cell already nails the target, →0 as |error| grows.
    float accuracy = constrainFloat(1.0f - fabs(rawError) / 0.30f, 0.0f, 1.0f);

    if (!takeMutex(mapMutex, pdMS_TO_TICKS(40))) return;

    for (int i = 0; i < 4; i++) {
        int tt = t + (i / 2);
        int rr = r + (i % 2);
        float w = weights[i];

        confidence[tt][rr] = constrainFloat(confidence[tt][rr] * 0.95f + accuracy * 0.05f, 0.05f, 1.0f);
        confidenceDirty = true;

        if (applyMapStep && w > 0.001f) {
            dutyMap2D[tt][rr] = constrainFloat(dutyMap2D[tt][rr] + boundedDelta * w, MAP_DUTY_MIN, MAP_DUTY_MAX);
            smoothCellTowardsNeighborsLocked(tt, rr);
            if (cellSamples[tt][rr] < 60000) cellSamples[tt][rr]++;
            mapCellDirty = true;
        }
    }

    if (mapCellDirty) {
        sanitizeMapProfileLocked();
        mapNeedsSaving = true;
    }
    xSemaphoreGive(mapMutex);
}

bool updateSettingValue(const String &key, const String &rawValue, String &error) {
    if (!takeMutex(configMutex, pdMS_TO_TICKS(80))) {
        error = "cfg_busy";
        return false;
    }

    bool ok = true;
    float fValue = 0.0f;
    int iValue = 0;

    if (key == "pR") {
        ok = parseFloatStrict(rawValue, 0.5f, 8.0f, fValue);
        if (ok) cfg.pulsesPerRev = fValue;
    } else if (key == "oP") {
        ok = parseFloatStrict(rawValue, 0.1f, 4.5f, fValue);
        if (ok) cfg.offsetPIM = fValue;
    } else if (key == "sP") {
        ok = parseFloatStrict(rawValue, 0.1f, 2.0f, fValue);
        if (ok) cfg.scalePIM = fValue;
    } else if (key == "oV") {
        ok = parseFloatStrict(rawValue, 0.1f, 3.5f, fValue);
        if (ok) cfg.offsetVTA = fValue;
    } else if (key == "tB") {
        ok = parseFloatStrict(rawValue, 0.3f, 1.5f, fValue);
        if (ok) cfg.targetBoost = fValue;
    } else if (key == "kP") {
        ok = parseFloatStrict(rawValue, 0.0f, 200.0f, fValue);
        if (ok) pid.kP = fValue;
    } else if (key == "kI") {
        ok = parseFloatStrict(rawValue, 0.0f, 200.0f, fValue);
        if (ok) pid.kI = fValue;
    } else if (key == "kD") {
        ok = parseFloatStrict(rawValue, 0.0f, 50.0f, fValue);
        if (ok) pid.kD = fValue;
    } else if (key == "lA") {
        ok = parseFloatStrict(rawValue, LEARN_RATE_MIN, LEARN_RATE_MAX, fValue);
        if (ok) pid.learnCoeff = fValue;
    } else if (key == "vP") {
        ok = parseFloatStrict(rawValue, 1.0f, 40.0f, fValue);
        if (ok) cfg.vssPulsesPerRev = fValue;
    } else if (key == "oD") {
        ok = parseFloatStrict(rawValue, 0.0f, 999999.0f, fValue);
        if (ok) {
            totalDistanceKm = static_cast<double>(fValue);
            odometerNeedsSaving = true;
        }
    } else if (key == "eH") {
        ok = parseFloatStrict(rawValue, 0.0f, 50000.0f, fValue);
        if (ok) {
            stationaryEngineHours = static_cast<double>(fValue);
            odometerNeedsSaving = true;
        }
    } else if (key == "tW") {
        ok = parseIntStrict(rawValue, 100, 300, iValue);
        if (ok) {
            cfg.tireW = iValue;
            calcWheelSizeLocked();
        }
    } else if (key == "tA") {
        ok = parseIntStrict(rawValue, 20, 100, iValue);
        if (ok) {
            cfg.tireA = iValue;
            calcWheelSizeLocked();
        }
    } else if (key == "tR") {
        ok = parseIntStrict(rawValue, 10, 24, iValue);
        if (ok) {
            cfg.tireR = iValue;
            calcWheelSizeLocked();
        }
    } else if (key == "lB") {
        ok = parseFloatStrict(rawValue, 0.3f, 2.0f, fValue);
        if (ok) cfg.limitBoostBar = fValue;
    } else {
        ok = false;
        error = "unknown_key";
    }

    if (ok && error.length() == 0) settingsNeedsSaving = true;
    if (!ok && error.length() == 0) error = "bad_value";

    xSemaphoreGive(configMutex);
    return ok;
}

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *server, esp_ble_gatts_cb_param_t *param) override {
        (void)server;
        deviceConnected = true;
        bleConnId = param->connect.conn_id;
        sendSettingsRequested = true;
        // Request fast connection interval immediately after pairing:
        // min=12*1.25ms=15ms, max=24*1.25ms=30ms, latency=0, timeout=400*10ms=4s
        esp_ble_conn_update_params_t connParams = {};
        memcpy(connParams.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        connParams.min_int = 12;
        connParams.max_int = 24;
        connParams.latency = 0;
        connParams.timeout = 400;
        esp_ble_gap_update_conn_params(&connParams);
    }

    void onDisconnect(BLEServer *server) override {
        deviceConnected = false;
        sendSettingsRequested = false;
        server->startAdvertising();
    }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) override {
        String raw = pCharacteristic->getValue();
        if (raw.length() == 0) return;
        if (raw.length() > BLE_MAX_MESSAGE_LEN) {
            sendBleError("BLE", "msg_too_long");
            return;
        }

        String msg(raw);
        msg.trim();
        if (msg.length() == 0) return;

        if (msg == "GET:SETTINGS") {
            sendSettingsRequested = true;
            sendBleAck("GET:SETTINGS");
            return;
        }

        if (msg.startsWith("DUTY:")) {
            int duty = 0;
            if (parseIntStrict(msg.substring(5), 0, 100, duty)) {
                testDuty = duty;
                sendBleAck("DUTY");
            } else {
                sendBleError("DUTY", "bad_duty");
            }
            return;
        }

        if (msg == "RESET") {
            if (takeMutex(dataMutex, pdMS_TO_TICKS(40))) {
                sensors.maxBoost = 0.0f;
                sensors.minBoost = 1.0f;
                sensors.maxRPM = 0.0f;
                sensors.maxSpeed = 0.0f;
                xSemaphoreGive(dataMutex);
            }
            sendBleAck("RESET");
            return;
        }

        if (msg.startsWith("KREQ:")) {
            sendKLineRequest(msg.substring(5));
            return;
        }

        if (msg == "SAVE") {
            settingsNeedsSaving = true;
            odometerNeedsSaving = true;
            forceSettingsSaveRequested = true;
            forceOdometerSaveRequested = true;
            notifyStorageTask();
            sendBleAck("SAVE");
            return;
        }

        if (msg == "OTA:ON") {
            requestReboot(REBOOT_TO_OTA);
            sendBleAck("OTA");
            return;
        }

        if (msg.startsWith("SET:")) {
            String data = msg.substring(4);
            int sepIndex = data.indexOf(':');
            if (sepIndex <= 0) {
                sendBleError("SET", "bad_format");
                return;
            }

            String key = data.substring(0, sepIndex);
            String val = data.substring(sepIndex + 1);

            String error;
            if (updateSettingValue(key, val, error)) {
                sendBleAck(key.c_str());
            } else {
                sendBleError(key.c_str(), error.c_str());
            }
            return;
        }

        sendBleError("BLE", "unknown_cmd");
    }
};

void IRAM_ATTR handleRPM() {
    unsigned long now = micros();
    portENTER_CRITICAL_ISR(&rpmMux);
    // Restored exactly to the version that read RPM 1:1 before the 05-21 changes.
    unsigned long dt = now - lastRpmMicros;
    if (dt > 3500) {
        rpmPeriodUs = dt;
        lastRpmMicros = now;
        g_rpmEdges++;
    }
    portEXIT_CRITICAL_ISR(&rpmMux);
}

void initPCNT() {
    pcnt_config_t pcnt_config = {};
    pcnt_config.pulse_gpio_num = vssPin;
    pcnt_config.ctrl_gpio_num = PCNT_PIN_NOT_USED;
    pcnt_config.lctrl_mode = PCNT_MODE_KEEP;
    pcnt_config.hctrl_mode = PCNT_MODE_KEEP;
    pcnt_config.pos_mode = PCNT_COUNT_INC;
    pcnt_config.neg_mode = PCNT_COUNT_DIS;
    pcnt_config.counter_h_lim = 20000;
    pcnt_config.counter_l_lim = -1;
    pcnt_config.unit = PCNT_UNIT;
    pcnt_config.channel = PCNT_CHANNEL_0;

    pcnt_unit_config(&pcnt_config);
    pinMode(vssPin, INPUT_PULLUP);
    pcnt_set_filter_value(PCNT_UNIT, 1023);
    pcnt_filter_enable(PCNT_UNIT);
    pcnt_counter_pause(PCNT_UNIT);
    pcnt_counter_clear(PCNT_UNIT);
    pcnt_counter_resume(PCNT_UNIT);
}

int16_t safeReadADS1115(uint8_t channel, uint32_t timeoutMs = 10) {
    // Keep ADC full-scale and software multiplier in sync:
    // PGA = +/- 6.144V  -> 0.1875 mV/LSB
    // Up to 3 attempts: a single NACK/glitch from a noisy bus (or right after a DAC write)
    // shouldn't lose the reading. Each failed transaction ends with a STOP, freeing the bus.
    for (int attempt = 0; attempt < 3; attempt++) {
        uint16_t config = ADS1115_CFG_BASE;
        config |= ((4 + channel) << 12);

        Wire.beginTransmission(ADS1115_I2C_ADDR);
        Wire.write(1);
        Wire.write(static_cast<uint8_t>(config >> 8));
        Wire.write(static_cast<uint8_t>(config & 0xFF));
        if (Wire.endTransmission() != 0) { delayMicroseconds(250); continue; }

        uint32_t start = millis();
        bool ready = false;
        while (millis() - start < timeoutMs) {
            Wire.beginTransmission(ADS1115_I2C_ADDR);
            Wire.write(1);
            if (Wire.endTransmission() != 0) break;
            Wire.requestFrom(ADS1115_I2C_ADDR, static_cast<uint8_t>(2));
            if (Wire.available() == 2) {
                uint16_t status = (Wire.read() << 8) | Wire.read();
                if ((status & 0x8000) != 0) { ready = true; break; }
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!ready) { delayMicroseconds(250); continue; }

        Wire.beginTransmission(ADS1115_I2C_ADDR);
        Wire.write(0);
        if (Wire.endTransmission() != 0) { delayMicroseconds(250); continue; }
        Wire.requestFrom(ADS1115_I2C_ADDR, static_cast<uint8_t>(2));
        if (Wire.available() == 2) {
            return static_cast<int16_t>((Wire.read() << 8) | Wire.read());
        }
        delayMicroseconds(250);
    }
    return INT16_MIN;
}

void updateSafety(const SensorData &d) {
    if (d.rawPIM < 0.1f || d.rawPIM > 4.9f || d.rawVTA < 0.1f || d.rawVTA > 4.9f) {
        systemMode = HARD_LIMP;
    } else if (isnan(d.boost) || d.boost > HARD_LIMP_BOOST_BAR || (d.rpm < 300.0f && d.tps > 5.0f)) {
        systemMode = HARD_LIMP;
    } else if (d.boost > SOFT_LIMP_BOOST_BAR) {
        systemMode = SOFT_LIMP;
    } else {
        systemMode = NORMAL;
    }
}

void TaskKLineReader(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_add(nullptr);

    uint8_t frame[KLINE_MAX_FRAME_LEN] = {0};
    uint8_t frameLen = 0;
    bool overflow = false;
    uint32_t lastByteAtMs = 0;

    for (;;) {
        if (klineBridgeBusy) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        while (Serial2.available() > 0) {
            uint8_t b = static_cast<uint8_t>(Serial2.read());
            uint32_t now = millis();

            if (frameLen < KLINE_MAX_FRAME_LEN) {
                frame[frameLen++] = b;
            } else {
                overflow = true;
            }

            if (takeMutex(klineMutex, pdMS_TO_TICKS(5))) {
                klineState.byteCount++;
                klineState.lastByte = b;
                klineState.lastByteAtMs = now;
                xSemaphoreGive(klineMutex);
            }

            lastByteAtMs = now;
        }

        if (frameLen > 0 && millis() - lastByteAtMs >= KLINE_FRAME_GAP_MS) {
            klineCommitFrame(frame, frameLen, overflow);
            frameLen = 0;
            overflow = false;
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void TaskSensors(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_add(nullptr);
    unsigned long lastSpdCalcMs = millis();
    float rpmHist[3] = {0, 0, 0};
    float smoothedSpeed = 0.0f;

    for (;;) {
        RuntimeConfig localCfg = snapshotConfig();

        unsigned long nowMs = millis();
        int16_t r0 = safeReadADS1115(0, 10);
        int16_t r1 = safeReadADS1115(1, 10);

        // Per-channel raw status (pre-debounce) for diagnostics. The debounce below zeroes BOTH
        // rawPIM and rawVTA whenever EITHER channel fails, so this is the only place the true
        // per-channel state is visible (e.g. MAP OK while TPS fails).
        g_ch0ok = (r0 != INT16_MIN);
        g_ch1ok = (r1 != INT16_MIN);
        g_ch0v = g_ch0ok ? (r0 * ADS1115_MULTIPLIER_6V144) : 0.0f;
        g_ch1v = g_ch1ok ? (r1 * ADS1115_MULTIPLIER_6V144) : 0.0f;

        // ADC debounce (F4): a single I2C glitch must not drop us into limp. Ride through
        // brief failures on the last good reading; only force a fault (0 V → limp) after
        // ADC_FAIL_LIMIT consecutive misses.
        static float lastGoodPIM = 2.57f;   // ~atmosphere
        static float lastGoodVTA = 0.42f;
        static int adcFailCount = 0;

        float rawPIM;
        float rawVTA;
        if (r0 == INT16_MIN || r1 == INT16_MIN) {
            adcFailCount++;
            if (adcFailCount >= ADC_FAIL_LIMIT) {
                rawPIM = 0.0f;   // genuine sensor/bus loss → updateSafety will go HARD_LIMP
                rawVTA = 0.0f;
            } else {
                rawPIM = lastGoodPIM;
                rawVTA = lastGoodVTA;
            }
        } else {
            adcFailCount = 0;
            rawPIM = r0 * ADS1115_MULTIPLIER_6V144;
            rawVTA = r1 * ADS1115_MULTIPLIER_6V144;
            lastGoodPIM = rawPIM;
            lastGoodVTA = rawVTA;
        }
        g_adcFailStreak = adcFailCount;   // surfaced in the Diag line

        filtered_map_volts = FILTER_OLD * filtered_map_volts + FILTER_NEW * rawPIM;
        float real_boost_bar = (filtered_map_volts - localCfg.offsetPIM) * localCfg.scalePIM;

        float ecu_boost_bar = min(real_boost_bar, localCfg.limitBoostBar);
        float out_volts = (ecu_boost_bar / localCfg.scalePIM) + localCfg.offsetPIM;
        out_volts = constrainFloat(out_volts, 0.0f, DAC_REFERENCE_VOLTAGE);
        uint16_t dac_value = (uint16_t)((out_volts / DAC_REFERENCE_VOLTAGE) * 4095.0f);
        if (DAC_OUTPUT_ENABLED) {
            dac.setVoltage(dac_value, false);   // disable via DAC_OUTPUT_ENABLED to A/B test the bus
        }

        float tpsSpan = max(0.3f, 3.71f - localCfg.offsetVTA);
        float tpsRaw = constrainFloat((rawVTA - localCfg.offsetVTA) / tpsSpan * 100.0f, 0.0f, 100.0f);

        unsigned long period;
        unsigned long lastTime;
        portENTER_CRITICAL(&rpmMux);
        period = rpmPeriodUs;
        lastTime = lastRpmMicros;
        portEXIT_CRITICAL(&rpmMux);

        float instRPM = 0.0f;
        if (micros() - lastTime <= 150000 && period > 0) {
            instRPM = (1000000.0f / period) * 60.0f / localCfg.pulsesPerRev;
        }
        instRPM = constrainFloat(instRPM, 0.0f, 12000.0f);

        rpmHist[2] = rpmHist[1];
        rpmHist[1] = rpmHist[0];
        rpmHist[0] = instRPM;
        float medRPM = max(min(rpmHist[0], rpmHist[1]), min(max(rpmHist[0], rpmHist[1]), rpmHist[2]));

        if (nowMs - lastSpdCalcMs >= 200) {
            float dt = (nowMs - lastSpdCalcMs) / 1000.0f;
            if (dt < 0.001f) dt = 0.2f;

            int16_t vssPulses = 0;
            pcnt_get_counter_value(PCNT_UNIT, &vssPulses);
            pcnt_counter_clear(PCNT_UNIT);
            if (vssPulses > 0) {
                portENTER_CRITICAL(&vssMux);
                pendingVssPulses += vssPulses;
                portEXIT_CRITICAL(&vssMux);
            }

            float frequencyHz = (vssPulses > 0) ? (static_cast<float>(vssPulses) / dt) : 0.0f;
            float instSpeed = (frequencyHz / localCfg.vssPulsesPerRev) * localCfg.wheelSizeM * 3.6f;
            smoothedSpeed = smoothedSpeed * 0.7f + instSpeed * 0.3f;
            if (smoothedSpeed < 2.0f && frequencyHz == 0.0f) smoothedSpeed = 0.0f;

            float nextGearTrim = computeGearBoostTrim(medRPM, smoothedSpeed, latchedGearBoostTrim);
            latchedGearBoostTrim = nextGearTrim;
            lastSpdCalcMs = nowMs;
        }

        if (takeMutex(dataMutex, pdMS_TO_TICKS(40))) {
            sensors.rawPIM = rawPIM;
            sensors.rawVTA = rawVTA;
            sensors.boost = constrainFloat(real_boost_bar, -1.0f, 2.0f);
            sensors.tps = sensors.tps * 0.60f + tpsRaw * 0.40f;
            sensors.rpm = sensors.rpm * 0.40f + medRPM * 0.60f;
            sensors.speed = smoothedSpeed;

            if (sensors.rpm > sensors.maxRPM) sensors.maxRPM = sensors.rpm;
            if (sensors.boost > sensors.maxBoost) sensors.maxBoost = sensors.boost;
            if (sensors.boost < sensors.minBoost) sensors.minBoost = sensors.boost;
            if (sensors.speed > sensors.maxSpeed) sensors.maxSpeed = sensors.speed;
            xSemaphoreGive(dataMutex);
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void TaskControl(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_add(nullptr);
    uint32_t lastPidMicros = micros();
    uint32_t lastMeasMicros = micros();
    bool wasEngineRunning = false;
    SensorData d = {};

    for (;;) {
        RuntimeConfig localCfg = snapshotConfig();
        if (takeMutex(dataMutex, pdMS_TO_TICKS(40))) {
            d = sensors;
            xSemaphoreGive(dataMutex);
        }

        updateSafety(d);
        currentBaseDuty = getMappedBaseDuty2D(d.rpm, d.tps);

        // F1: precise loop timing from micros() (unsigned subtraction handles the ~71 min wrap).
        uint32_t nowUs = micros();
        float dt = (nowUs - lastPidMicros) / 1000000.0f;
        if (dt <= 0.0002f || dt > 0.5f) dt = CONTROL_PERIOD_MS / 1000.0f;
        lastPidMicros = nowUs;

        float targetTrim = latchedGearBoostTrim;
        float shapedTarget = localCfg.targetBoost * getTargetShape(d.rpm);
        float desiredDynamicTarget = constrainFloat(shapedTarget - targetTrim, 0.30f, localCfg.targetBoost);
        if (!filteredDynamicTargetInitialized) {
            filteredDynamicTarget = desiredDynamicTarget;
            filteredDynamicTargetInitialized = true;
        } else {
            float alpha = dt / (TARGET_FILTER_TAU_S + dt);
            filteredDynamicTarget += (desiredDynamicTarget - filteredDynamicTarget) * alpha;
        }

        float dynamicTarget = filteredDynamicTarget;
        float targetRate = (dynamicTarget - prevControlTarget) / dt;   // bar/s, smooth (target is low-passed)
        prevControlTarget = dynamicTarget;

        if (systemMode == NORMAL && d.tps > 10.0f && d.rpm >= 1300.0f) {
            float err = dynamicTarget - d.boost;

            // Gain scheduling: aggressive on spool, gentle up top.
            float gainFactor = spoolGainFactor(d.rpm);
            float kpEff = pid.kP * gainFactor;
            float kiEff = pid.kI * (1.0f + (gainFactor - 1.0f) * 0.5f);
            float kdEff = pid.kD;

            // F1: derivative-on-measurement evaluated over the REAL time since the boost reading
            // actually changed (the control loop runs faster than the sensor, so most cycles see
            // an unchanged value — recomputing every cycle would inject 0/spike noise).
            if (d.boost != pid.lastMeas) {
                float measDt = (nowUs - lastMeasMicros) / 1000000.0f;
                if (measDt < 0.0005f) measDt = dt;
                float measRate = (d.boost - pid.lastMeas) / measDt;
                pid.filteredDerivative = pid.filteredDerivative * 0.65f + (-measRate) * 0.35f;
                pid.lastMeas = d.boost;
                lastMeasMicros = nowUs;
            } else {
                pid.filteredDerivative *= 0.9f;   // no fresh data → relax the derivative toward 0
            }
            pid.lastError = err;

            // Setpoint feed-forward: anticipate a rising target to spool faster (help only, never fight).
            float setpointFF = constrainFloat(targetRate * SETPOINT_FF_GAIN, 0.0f, SETPOINT_FF_CAP);

            // F3: conditional-integration anti-windup. Form the candidate integral, build the
            // output, and only commit the integral if we are not driving further into saturation.
            float integralCandidate = constrainFloat(pid.integral + err * kiEff * dt, -INTEGRAL_LIMIT, INTEGRAL_LIMIT);
            float dutyUnclamped = currentBaseDuty + (err * kpEff) + integralCandidate + (pid.filteredDerivative * kdEff) + setpointFF;
            bool pushingIntoHigh = dutyUnclamped > MAP_DUTY_MAX && err > 0.0f;
            bool pushingIntoLow  = dutyUnclamped < MAP_DUTY_MIN && err < 0.0f;
            if (!pushingIntoHigh && !pushingIntoLow) {
                pid.integral = integralCandidate;     // accept; otherwise freeze (no abrupt *0.9 kicks)
            }

            currentOutDuty = constrainFloat(dutyUnclamped, MAP_DUTY_MIN, MAP_DUTY_MAX);

            bool pidNotSaturated = currentOutDuty > 6.0f && currentOutDuty < 84.0f;
            if (pidNotSaturated && learningWindowStable(d, err)) {
                // Adaptive feed-forward (P1): move the steady integral bias into the map and
                // bleed the integral by the same amount — the map permanently absorbs the offset,
                // converging in seconds and leaving the integral free for transients.
                // loopScale keeps learning speed independent of CONTROL_PERIOD_MS: at 50 ms the
                // loop fires 5x less often than the original 10 ms design, so each step is 5x larger.
                float loopScale = CONTROL_PERIOD_MS / 10.0f;
                float maxStep = MAP_LEARN_MAX_STEP * loopScale;
                float offload = constrainFloat(pid.integral * pid.learnCoeff * loopScale, -maxStep, maxStep);
                learnDutyMap3D(d.rpm, d.tps, offload, err);
                pid.integral -= offload * INTEGRAL_TRANSFER;
            }
        } else {
            learningState.stableSinceMs = millis();
            pid.integral *= 0.90f;
            pid.lastError = 0.0f;
            pid.lastMeas = d.boost;
            lastMeasMicros = nowUs;
            pid.filteredDerivative = 0.0f;
            currentOutDuty = (systemMode == SOFT_LIMP) ? 20.0f : 0.0f;
        }

        // F2: write the solenoid PWM immediately here — no separate TaskPWM, no 0–20 ms dead time.
        // Manual solenoid test (DUTY:) still overrides while the car is stationary.
        int requestedTestDuty = constrain(static_cast<int>(testDuty), 0, 100);
        int pwmByte = (d.speed < 2.0f && requestedTestDuty > 0)
            ? map(requestedTestDuty, 0, 100, 0, 255)
            : map(static_cast<int>(currentOutDuty), 0, 100, 0, 255);
        ledcWrite(solPin, pwmByte);

        // F6: when the engine stops, flush the learned map promptly so up to a minute of
        // adaptation isn't lost (the routine NVS cadence is once per minute, see TaskOdometerAndStorage).
        bool engineRunning = d.rpm > 300.0f;
        if (wasEngineRunning && !engineRunning && mapNeedsSaving) {
            forceMapSaveRequested = true;
            notifyStorageTask();
        }
        wasEngineRunning = engineRunning;

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

// TaskPWM removed (F2): the solenoid is now written directly at the end of TaskControl,
// eliminating the 0–20 ms actuation dead time.

void TaskTelemetry(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_add(nullptr);
    char bleBuffer[512];
    SensorData d = {};
    uint32_t lastPrint = millis();
    uint32_t lastNotifyMs = 0;
    uint32_t lastKLineNotifyMs = 0;

    for (;;) {
        if (takeMutex(dataMutex, pdMS_TO_TICKS(40))) {
            d = sensors;
            xSemaphoreGive(dataMutex);
        }

        if (millis() - lastPrint > 5000) {
            KLineState kline = snapshotKLine();
            Serial.printf("Diag Heap:%u MaxBlock:%u TelStack:%u Mode:%d MTU:%u Conn:%d\n",
                ESP.getFreeHeap(),
                ESP.getMaxAllocHeap(),
                uxTaskGetStackHighWaterMark(nullptr),
                static_cast<int>(systemMode),
                (deviceConnected && pServer != nullptr) ? pServer->getPeerMTU(bleConnId) : 0,
                deviceConnected ? 1 : 0);
            // Sensor snapshot. ADCfail>0 means the ESP can't talk to the ADS1115 (I2C fault) —
            // that is NOT "0 V on the input". 0.000 V with ADCfail>0 = bus/comms loss.
            Serial.printf("Sensors PIM:%.3fV VTA:%.3fV boost:%.2f rpm:%.0f tps:%.0f%% speed:%.0f ADCfail:%d\n",
                d.rawPIM, d.rawVTA, d.boost, d.rpm, d.tps, d.speed, g_adcFailStreak);
            // True per-channel reads (pre-debounce) + DAC state. This tells us whether MAP (ch0)
            // still reads in runtime while TPS (ch1) fails, or both die — and isolates the DAC.
            Serial.printf("ADC ch0(MAP):%s %.3fV  ch1(TPS):%s %.3fV  DAC:%s\n",
                g_ch0ok ? "OK" : "FAIL", g_ch0v,
                g_ch1ok ? "OK" : "FAIL", g_ch1v,
                DAC_OUTPUT_ENABLED ? "on" : "off");
            // Raw tach: periodUs = time between accepted edges; edges = running count.
            // rpm = 60e6 / periodUs / pulsesPerRev. Two prints 5 s apart → edges/5 = real edge Hz.
            Serial.printf("RPMraw periodUs:%lu edges:%lu  loopMs:%u\n",
                static_cast<unsigned long>(rpmPeriodUs),
                static_cast<unsigned long>(g_rpmEdges),
                static_cast<unsigned>(CONTROL_PERIOD_MS));
            Serial.printf("KLINE bytes:%lu frames:%lu lastLen:%u overflows:%lu\n",
                static_cast<unsigned long>(kline.byteCount),
                static_cast<unsigned long>(kline.frameCount),
                kline.lastFrameLen,
                static_cast<unsigned long>(kline.overflowCount));
            lastPrint = millis();
        }

        if (deviceConnected && millis() - lastNotifyMs >= BLE_NOTIFY_INTERVAL_MS) {
            if (sendSettingsRequested) {
                RuntimeConfig localCfg = snapshotConfig();

                snprintf(bleBuffer, sizeof(bleBuffer),
                    "{\"S\":1,\"pR\":%.1f,\"oP\":%.2f,\"sP\":%.2f,\"oV\":%.2f,\"tB\":%.2f,\"lB\":%.2f,\"kP\":%.1f,\"kI\":%.1f,\"kD\":%.1f,\"tW\":%d,\"tA\":%d,\"tR\":%d,\"eH\":%.2f,\"vP\":%.2f,\"lA\":%.3f}\n",
                    localCfg.pulsesPerRev, localCfg.offsetPIM, localCfg.scalePIM, localCfg.offsetVTA,
                    localCfg.targetBoost, localCfg.limitBoostBar, pid.kP, pid.kI, pid.kD,
                    localCfg.tireW, localCfg.tireA, localCfg.tireR,
                    stationaryEngineHours, localCfg.vssPulsesPerRev, pid.learnCoeff
                );
                sendBleText(bleBuffer);
                vTaskDelay(pdMS_TO_TICKS(SETTINGS_RESEND_INTERVAL_MS));

                sendSettingsRequested = false;
                vTaskDelay(pdMS_TO_TICKS(SETTINGS_RESEND_INTERVAL_MS));
            }

            snprintf(bleBuffer, sizeof(bleBuffer),
                "{\"T\":1,\"b\":%.2f,\"miB\":%.2f,\"maB\":%.2f,\"r\":%.0f,\"maR\":%.0f,\"s\":%.0f,\"maS\":%.0f,\"v\":%.0f,\"oD\":%.2f,\"bD\":%.1f,\"cD\":%.1f,\"mode\":%d}\n",
                d.boost, d.minBoost, d.maxBoost, d.rpm, d.maxRPM,
                d.speed, d.maxSpeed, d.tps, totalDistanceKm, currentBaseDuty, currentOutDuty, static_cast<int>(systemMode)
            );
            sendBleText(bleBuffer);
            lastNotifyMs = millis();

            if (millis() - lastKLineNotifyMs >= 1000) {
                KLineState kline = snapshotKLine();
                char klineHex[KLINE_MAX_FRAME_LEN * 3] = {0};
                klineFormatFrameHex(kline, klineHex, sizeof(klineHex));
                snprintf(bleBuffer, sizeof(bleBuffer),
                    "{\"K\":1,\"kb\":%lu,\"kf\":%lu,\"kl\":%u,\"ko\":%lu,\"kh\":\"%s\"}\n",
                    static_cast<unsigned long>(kline.byteCount),
                    static_cast<unsigned long>(kline.frameCount),
                    kline.lastFrameLen,
                    static_cast<unsigned long>(kline.overflowCount),
                    klineHex
                );
                sendBleText(bleBuffer);
                lastKLineNotifyMs = millis();
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void TaskOdometerAndStorage(void *pvParameters) {
    (void)pvParameters;
    storageTaskHandle = xTaskGetCurrentTaskHandle();
    esp_task_wdt_add(nullptr);
    double lastSavedKm = totalDistanceKm;
    double lastSavedHours = stationaryEngineHours;
    SensorData d = {};
    uint32_t lastHoursUpdateMs = millis();

    for (;;) {
        uint32_t pulsesToCalc = 0;
        portENTER_CRITICAL(&vssMux);
        pulsesToCalc = pendingVssPulses;
        pendingVssPulses = 0;
        portEXIT_CRITICAL(&vssMux);

        RuntimeConfig localCfg = snapshotConfig();

        if (pulsesToCalc > 0) {
            double distanceStepKm = (static_cast<double>(pulsesToCalc) / static_cast<double>(localCfg.vssPulsesPerRev)) *
                static_cast<double>(localCfg.wheelSizeM) / 1000.0;
            totalDistanceKm += distanceStepKm;
        }

        if (takeMutex(dataMutex, pdMS_TO_TICKS(40))) {
            d = sensors;
            xSemaphoreGive(dataMutex);
        }

        uint32_t nowMs = millis();
        double dtHours = (nowMs - lastHoursUpdateMs) / 3600000.0;
        lastHoursUpdateMs = nowMs;
        if (d.rpm > 500.0f && d.speed < 1.0f && dtHours > 0.0) {
            stationaryEngineHours += dtHours;
        }

        if ((totalDistanceKm - lastSavedKm >= 1.0) || (stationaryEngineHours - lastSavedHours >= 0.1)) {
            odometerNeedsSaving = true;
        }

        if (settingsNeedsSaving) {
            if (takeMutex(configMutex, pdMS_TO_TICKS(80))) {
                prefs.putFloat("oP", cfg.offsetPIM);
                prefs.putFloat("sP", cfg.scalePIM);
                prefs.putFloat("pR", cfg.pulsesPerRev);
                prefs.putFloat("oV", cfg.offsetVTA);
                prefs.putFloat("tB", cfg.targetBoost);
                prefs.putFloat("kP", pid.kP);
                prefs.putFloat("kI", pid.kI);
                prefs.putFloat("kD", pid.kD);
                prefs.putFloat("lA", pid.learnCoeff);
                prefs.putFloat("vP", cfg.vssPulsesPerRev);
                prefs.putInt("tW", cfg.tireW);
                prefs.putInt("tA", cfg.tireA);
                prefs.putInt("tR", cfg.tireR);
                prefs.putFloat("lB", cfg.limitBoostBar);
                xSemaphoreGive(configMutex);
                settingsNeedsSaving = false;
                forceSettingsSaveRequested = false;
            }
        }

        if (odometerNeedsSaving && (forceOdometerSaveRequested || (totalDistanceKm - lastSavedKm >= 1.0) || (stationaryEngineHours - lastSavedHours >= 0.1))) {
            prefs.putDouble("oD", totalDistanceKm);
            prefs.putDouble("eH", stationaryEngineHours);
            odometerNeedsSaving = false;
            forceOdometerSaveRequested = false;
            lastSavedKm = totalDistanceKm;
            lastSavedHours = stationaryEngineHours;
        }

        if (mapNeedsSaving && (forceMapSaveRequested || (millis() - lastMapSaveMs >= MAP_SAVE_INTERVAL_MS))) {
            float mapSnapshot[NUM_TPS_BINS][NUM_RPM_BINS];
            float confidenceSnapshot[NUM_TPS_BINS][NUM_RPM_BINS];
            uint16_t samplesSnapshot[NUM_TPS_BINS][NUM_RPM_BINS];
            bool writeMap = false;
            bool writeConfidence = false;

            if (takeMutex(mapMutex, pdMS_TO_TICKS(100))) {
                if (mapCellDirty) {
                    memcpy(mapSnapshot, dutyMap2D, sizeof(mapSnapshot));
                    mapCellDirty = false;
                    writeMap = true;
                }
                if (confidenceDirty) {
                    memcpy(confidenceSnapshot, confidence, sizeof(confidenceSnapshot));
                    memcpy(samplesSnapshot, cellSamples, sizeof(samplesSnapshot));
                    confidenceDirty = false;
                    writeConfidence = true;
                }

                for (int t = 0; t < NUM_TPS_BINS; t++) {
                    for (int r = 0; r < NUM_RPM_BINS; r++) {
                        confidence[t][r] = constrainFloat(confidence[t][r] * 0.9995f, 0.05f, 1.0f);
                    }
                }
                mapNeedsSaving = false;
                xSemaphoreGive(mapMutex);
            }

            if (writeMap) prefs.putBytes("map2D", mapSnapshot, sizeof(mapSnapshot));
            if (writeConfidence) {
                prefs.putBytes("conf2D", confidenceSnapshot, sizeof(confidenceSnapshot));
                prefs.putBytes("samples2D", samplesSnapshot, sizeof(samplesSnapshot));
            }
            forceMapSaveRequested = false;
            lastMapSaveMs = millis();
        }

        esp_task_wdt_reset();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

const char *ota_html =
    "<html style='background:#111;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;'>"
    "<h2>YRV Boost Controller OTA</h2>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='update' style='margin-bottom:20px;'><br>"
    "<input type='submit' value='Загрузить прошивку' style='padding:10px 20px;font-weight:bold;background:#fa0;border:none;border-radius:5px;cursor:pointer;'>"
    "</form><br><hr style='border:0;border-top:1px solid #333;max-width:300px;margin:20px auto;'>"
    "<form method='GET' action='/cancel'>"
    "<input type='submit' value='Отмена' style='padding:10px 20px;font-weight:bold;background:#444;color:#fff;border:none;border-radius:5px;cursor:pointer;'>"
    "</form></html>";

void setupOTAWebServer() {
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", ota_html);
    });

    server.on("/cancel", HTTP_GET, []() {
        server.send(200, "text/html", "<html style='background:#111;color:#fff;text-align:center;padding-top:50px;'><h2>Перезагрузка...</h2></html>");
        requestReboot(REBOOT_TO_NORMAL);
    });

    server.on("/update", HTTP_POST, []() {
        bool shouldReboot = !Update.hasError();
        server.send(200, "text/plain", shouldReboot ? "УСПЕШНО! Перезагрузка..." : "ОШИБКА ПРОШИВКИ");
        if (shouldReboot) requestReboot(REBOOT_TO_NORMAL);
    }, []() {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) Update.printError(Serial);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) Serial.println("Update Success");
            else Update.printError(Serial);
        }
    });
    server.begin();
}

void setup() {
    Serial.begin(115200);
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("System Booted. Reset Reason: %d\n", static_cast<int>(reason));

    dataMutex = xSemaphoreCreateMutex();
    mapMutex = xSemaphoreCreateMutex();
    configMutex = xSemaphoreCreateMutex();
    bleMutex = xSemaphoreCreateMutex();
    klineMutex = xSemaphoreCreateMutex();
    if (dataMutex == nullptr || mapMutex == nullptr || configMutex == nullptr || bleMutex == nullptr || klineMutex == nullptr) {
        Serial.println("Crit Error: Mutex creation failed!");
    }

    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_SEC * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&twdt_config);

    Serial.println("Step: NVS init...");
    if (!prefs.begin("yrv_v4", false)) {
        Serial.println("Crit Error: NVS Init failed!");
    }

    otaModeEnabled = prefs.getBool("ota_mode", false);

    if (takeMutex(configMutex, pdMS_TO_TICKS(100))) {
        cfg.offsetPIM = constrainFloat(prefs.getFloat("oP", 2.56f), 0.1f, 4.5f);
        cfg.scalePIM = constrainFloat(prefs.getFloat("sP", 0.55f), 0.1f, 2.0f);
        cfg.pulsesPerRev = constrainFloat(prefs.getFloat("pR", 2.0f), 0.5f, 8.0f);
        cfg.offsetVTA = constrainFloat(prefs.getFloat("oV", 0.42f), 0.1f, 3.5f);
        cfg.targetBoost = constrainFloat(prefs.getFloat("tB", 0.80f), 0.3f, 1.5f);
        cfg.vssPulsesPerRev = constrainFloat(prefs.getFloat("vP", 5.18f), 1.0f, 40.0f);
        cfg.tireW = constrain(prefs.getInt("tW", 195), 100, 300);
        cfg.tireA = constrain(prefs.getInt("tA", 55), 20, 100);
        cfg.tireR = constrain(prefs.getInt("tR", 15), 10, 24);
        cfg.limitBoostBar = constrainFloat(prefs.getFloat("lB", 0.95f), 0.3f, 2.0f);
        calcWheelSizeLocked();
        pid.kP = constrainFloat(prefs.getFloat("kP", DEFAULT_KP), 0.0f, 200.0f);
        pid.kI = constrainFloat(prefs.getFloat("kI", DEFAULT_KI), 0.0f, 200.0f);
        pid.kD = constrainFloat(prefs.getFloat("kD", DEFAULT_KD), 0.0f, 50.0f);
        pid.learnCoeff = constrainFloat(prefs.getFloat("lA", DEFAULT_LEARN_RATE), LEARN_RATE_MIN, LEARN_RATE_MAX);

        // One-time migration to the v6 tune. Forces the adaptive PID gains AND the requested
        // sensor calibrations (oP/sP/oV) once, overriding any stale values saved in NVS. After
        // this runs, the app can still fine-tune them and the changes persist.
        // (oP is also re-set by the atmosphere auto-zero at every boot — see below.)
        if (prefs.getInt("tuneVer", 0) < 6) {
            pid.kP = DEFAULT_KP;
            pid.kI = DEFAULT_KI;
            pid.kD = DEFAULT_KD;
            pid.learnCoeff = DEFAULT_LEARN_RATE;
            cfg.offsetPIM = 2.56f;
            cfg.scalePIM = 0.55f;
            cfg.offsetVTA = 0.42f;
            prefs.putFloat("kP", pid.kP);
            prefs.putFloat("kI", pid.kI);
            prefs.putFloat("kD", pid.kD);
            prefs.putFloat("lA", pid.learnCoeff);
            prefs.putFloat("oP", cfg.offsetPIM);
            prefs.putFloat("sP", cfg.scalePIM);
            prefs.putFloat("oV", cfg.offsetVTA);
            prefs.putInt("tuneVer", 6);
        }
        xSemaphoreGive(configMutex);
    }

    totalDistanceKm = prefs.getDouble("oD", 0.0);
    stationaryEngineHours = prefs.getDouble("eH", 0.0);
    Serial.println("Step: load map...");
    loadMap();

    sensors = {0, 0, 0, 0, 0, 1.0f, 0, 0, 0, 0};
    learningState.stableSinceMs = millis();

    if (otaModeEnabled) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(ssid, password);
        setupOTAWebServer();
        return;
    }

    Serial.println("Step: I2C begin...");
    Wire.begin(sdaPin, sclPin);
    Wire.setClock(100000);   // 100 kHz: reliable through the logic-level converter (400 kHz corrupted ADS1115 reads)
    Wire.setTimeOut(20);

    Serial.println("Step: ADS1115 begin...");
    if (!ads.begin()) {
        Serial.println("Error: ADS1115 not found!");
    }
    ads.setGain(GAIN_TWOTHIRDS);
    ads.setDataRate(RATE_ADS1115_860SPS);

    Serial.println("Step: MCP4725 begin...");
    dac.begin(0x60);   // MCP4725 actual address (I2C scan found 0x60, not 0x62)

    // Auto-zero the MAP at atmosphere. Uses the timeout-bounded safeReadADS1115() instead of the
    // Adafruit blocking read, so a flaky bus can never hang setup() in an endless conversion-wait.
    Serial.println("Step: MAP auto-zero...");
    {
        float sumVolts = 0.0f;
        int validSamples = 0;
        for (int i = 0; i < 10; i++) {
            int16_t raw = safeReadADS1115(0, 10);
            if (raw != INT16_MIN) {
                sumVolts += raw * ADS1115_MULTIPLIER_6V144;
                validSamples++;
            }
            delay(5);
        }
        float avgVolts = (validSamples > 0) ? (sumVolts / validSamples) : 0.0f;
        if (validSamples > 0 && avgVolts >= ATMOS_MIN_VOLTS && avgVolts <= ATMOS_MAX_VOLTS) {
            cfg.offsetPIM = avgVolts;
            Serial.printf("Auto-Zero: offset=%.3fV (calibrated, %d samples)\n", cfg.offsetPIM, validSamples);
        } else {
            // Keep the NVS-loaded calibration instead of forcing a default — safe after a
            // mid-drive reboot (not at atmosphere) or if the ADC didn't answer this boot.
            Serial.printf("Auto-Zero: skipped (avg=%.3fV, valid=%d), keeping oP=%.3fV\n", avgVolts, validSamples, cfg.offsetPIM);
        }
        filtered_map_volts = cfg.offsetPIM;
    }

    pinMode(rpmPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(rpmPin), handleRPM, FALLING);
    initPCNT();

    pinMode(klineTxPin, OUTPUT);
    digitalWrite(klineTxPin, HIGH);
    Serial2.begin(KLINE_BAUD, SERIAL_8N1, klineRxPin, klineTxPin);
    Serial.printf("K-Line listen-only UART started: RX=%d TX=%d baud=%d\n", klineRxPin, klineTxPin, KLINE_BAUD);

    ledcAttach(solPin, pwmFreq, pwmRes);
    ledcWrite(solPin, 0);

    Serial.println("Step: BLE init...");
    BLEDevice::init("YRV_Boost_BLE");
    BLEDevice::setMTU(247);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_UUID);

    pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
    pTxCharacteristic->addDescriptor(new BLE2902());

    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE);
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    pService->start();

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    // Preferred connection interval hint in GAP data: 15ms–30ms
    pAdvertising->setMinPreferred(0x0C);  // 12 * 1.25ms = 15ms
    pAdvertising->setMaxPreferred(0x18);  // 24 * 1.25ms = 30ms
    // Advertising interval: 20ms–40ms for fast phone discovery
    pAdvertising->setMinInterval(32);     // 32 * 0.625ms = 20ms
    pAdvertising->setMaxInterval(64);     // 64 * 0.625ms = 40ms
    pAdvertising->start();

    Serial.println("Step: starting tasks...");
    xTaskCreatePinnedToCore(TaskSensors, "SENS", 4096, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(TaskKLineReader, "KLINE", 3072, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(TaskControl, "CTRL", 4096, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(TaskTelemetry, "TELEM", 4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(TaskOdometerAndStorage, "ODO_STOR", 4096, nullptr, 1, nullptr, 0);
    Serial.println("Setup complete. Running.");
}

void loop() {
    if (otaModeEnabled) {
        server.handleClient();
        handlePendingReboot();
        delay(2);
    } else {
        handlePendingReboot();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
