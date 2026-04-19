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
#include "driver/pcnt.h"
#include "esp_task_wdt.h"
#include "esp_system.h"

#define WATCHDOG_TIMEOUT_SEC 5
#define BLE_MAX_MESSAGE_LEN 96
#define BLE_NOTIFY_INTERVAL_MS 125
#define SETTINGS_RESEND_INTERVAL_MS 60

const char *ssid = "YRV_Boost_Pro";
const char *password = "";

Adafruit_ADS1115 ads;
WebServer server(80);
Preferences prefs;

SemaphoreHandle_t dataMutex;
SemaphoreHandle_t mapMutex;
SemaphoreHandle_t configMutex;
SemaphoreHandle_t bleMutex;

const int rpmPin = 18;
const int vssPin = 19;
const int sdaPin = 23;
const int sclPin = 22;
const int solPin = 25;

#define PCNT_UNIT PCNT_UNIT_0

const int pwmFreq = 30;
const int pwmRes = 8;
volatile int activePwmDuty = 0;
volatile int testDuty = 0;

struct PID_Config {
    float kP = 25.0f;
    float kI = 15.0f;
    float kD = 15.0f;
    float learnCoeff = 0.03f;
    float integral = 0.0f;
    float lastError = 0.0f;
    float filteredDerivative = 0.0f;
} pid;

struct RuntimeConfig {
    float offsetPIM = 2.57f;
    float scalePIM = 0.64f;
    float pulsesPerRev = 2.0f;
    float offsetVTA = 0.35f;
    float targetBoost = 0.80f;
    float vssPulsesPerRev = 5.18f;
    int tireW = 195;
    int tireA = 55;
    int tireR = 15;
    float wheelSizeM = 1.87f;
} cfg;

const int NUM_RPM_BINS = 11;
const int NUM_TPS_BINS = 4;

float rpmBins[NUM_RPM_BINS] = {2000, 2500, 3000, 3500, 4000, 4500, 5000, 5500, 6000, 6500, 7000};
float tpsBins[NUM_TPS_BINS] = {20, 40, 70, 90};

float dutyMap2D[NUM_TPS_BINS][NUM_RPM_BINS];
float confidence[NUM_TPS_BINS][NUM_RPM_BINS];
uint16_t cellSamples[NUM_TPS_BINS][NUM_RPM_BINS];
bool mapCellDirty = false;
bool confidenceDirty = false;

volatile int activeBleTab = 3;
float currentBaseDuty = 40.0f;
float currentOutDuty = 0.0f;

unsigned long lastMapSaveMs = 0;
volatile bool mapNeedsSaving = false;
volatile bool settingsNeedsSaving = false;
volatile bool sendSettingsRequested = false;
volatile bool odometerNeedsSaving = false;
volatile bool otaModeEnabled = false;

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

enum Mode { NORMAL, SOFT_LIMP, HARD_LIMP };
volatile Mode systemMode = NORMAL;

portMUX_TYPE rpmMux = portMUX_INITIALIZER_UNLOCKED;
volatile unsigned long rpmPeriodUs = 0;
volatile unsigned long lastRpmMicros = 0;

BLEServer *pServer = nullptr;
BLECharacteristic *pTxCharacteristic = nullptr;
volatile bool deviceConnected = false;

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

struct LearningState {
    float lastTps = 0.0f;
    float lastRpm = 0.0f;
    float lastBoost = 0.0f;
    uint32_t stableSinceMs = 0;
} learningState;

inline float constrainFloat(float x, float a, float b) {
    return x < a ? a : (x > b ? b : x);
}

bool isFiniteFloat(float value) {
    return !isnan(value) && !isinf(value);
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

void calcWheelSizeLocked() {
    float diameterMm = (cfg.tireR * 25.4f) + 2.0f * (cfg.tireW * (cfg.tireA / 100.0f));
    cfg.wheelSizeM = (diameterMm * PI) / 1000.0f;
}

void sanitizeMapProfileLocked() {
    for (int t = 0; t < NUM_TPS_BINS; t++) {
        for (int r = 0; r < NUM_RPM_BINS; r++) {
            dutyMap2D[t][r] = constrainFloat(dutyMap2D[t][r], 0.0f, 85.0f);
            confidence[t][r] = constrainFloat(confidence[t][r], 0.05f, 1.0f);
            if (r > 0) {
                float delta = dutyMap2D[t][r] - dutyMap2D[t][r - 1];
                if (delta > 12.0f) dutyMap2D[t][r] = dutyMap2D[t][r - 1] + 12.0f;
                if (delta < -12.0f) dutyMap2D[t][r] = dutyMap2D[t][r - 1] - 12.0f;
            }
        }
    }
}

void initDefaultMapLocked() {
    const float defaults[NUM_TPS_BINS][NUM_RPM_BINS] = {
        {60, 60, 60, 60, 58, 55, 55, 55, 50, 50, 50},
        {55, 55, 52, 50, 50, 48, 45, 45, 42, 40, 40},
        {48, 48, 45, 44, 42, 42, 40, 40, 38, 38, 35},
        {42, 42, 40, 40, 40, 38, 38, 38, 35, 35, 35}
    };

    memcpy(dutyMap2D, defaults, sizeof(dutyMap2D));
    for (int t = 0; t < NUM_TPS_BINS; t++) {
        for (int r = 0; r < NUM_RPM_BINS; r++) {
            confidence[t][r] = 0.5f;
            cellSamples[t][r] = 0;
        }
    }
}

void sendBleText(const char *text) {
    if (!deviceConnected || pTxCharacteristic == nullptr || text == nullptr) return;
    if (!takeMutex(bleMutex, pdMS_TO_TICKS(20))) return;
    pTxCharacteristic->setValue(reinterpret_cast<const uint8_t *>(text), strlen(text));
    pTxCharacteristic->notify();
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

bool learningWindowStable(const SensorData &d, float err) {
    const uint32_t now = millis();
    const float tpsDelta = fabs(d.tps - learningState.lastTps);
    const float rpmDelta = fabs(d.rpm - learningState.lastRpm);
    const float boostDelta = fabs(d.boost - learningState.lastBoost);

    bool locallyStable =
        tpsDelta < 2.0f &&
        rpmDelta < 180.0f &&
        boostDelta < 0.05f &&
        fabs(err) < 0.18f &&
        d.tps > 18.0f &&
        d.rpm > 2200.0f &&
        d.speed > 8.0f &&
        systemMode == NORMAL;

    if (!locallyStable) {
        learningState.stableSinceMs = now;
    }

    learningState.lastTps = d.tps;
    learningState.lastRpm = d.rpm;
    learningState.lastBoost = d.boost;

    return locallyStable && (now - learningState.stableSinceMs >= 400);
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

void learnDutyMap3D(float currentRpm, float currentTps, float targetDutyCorrection, float rawError) {
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

    float boundedCorrection = constrainFloat(targetDutyCorrection, -0.30f, 0.30f);

    if (!takeMutex(mapMutex, pdMS_TO_TICKS(40))) return;

    for (int i = 0; i < 4; i++) {
        int tt = t + (i / 2);
        int rr = r + (i % 2);

        if (fabs(rawError) > 0.25f) {
            confidence[tt][rr] = constrainFloat(confidence[tt][rr] * 0.997f, 0.05f, 1.0f);
            confidenceDirty = true;
            continue;
        }

        float maturity = constrainFloat(cellSamples[tt][rr] / 150.0f, 0.0f, 1.0f);
        float gain = (1.0f - confidence[tt][rr]) * (0.55f + 0.45f * (1.0f - maturity));
        dutyMap2D[tt][rr] += boundedCorrection * weights[i] * gain;
        dutyMap2D[tt][rr] = constrainFloat(dutyMap2D[tt][rr], 0.0f, 85.0f);
        smoothCellTowardsNeighborsLocked(tt, rr);

        if (cellSamples[tt][rr] < 60000) cellSamples[tt][rr]++;
        confidence[tt][rr] = constrainFloat(confidence[tt][rr] + 0.0015f, 0.05f, 1.0f);
        mapCellDirty = true;
        confidenceDirty = true;
    }

    sanitizeMapProfileLocked();
    mapNeedsSaving = true;
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
        ok = parseFloatStrict(rawValue, 0.005f, 0.15f, fValue);
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
    } else if (key.startsWith("M_")) {
        int firstUs = key.indexOf('_');
        int secondUs = key.indexOf('_', firstUs + 1);
        int t = -1;
        int r = -1;
        if (firstUs == -1 || secondUs == -1 ||
            !parseIntStrict(key.substring(firstUs + 1, secondUs), 0, NUM_TPS_BINS - 1, t) ||
            !parseIntStrict(key.substring(secondUs + 1), 0, NUM_RPM_BINS - 1, r) ||
            !parseFloatStrict(rawValue, 0.0f, 85.0f, fValue)) {
            ok = false;
        } else if (takeMutex(mapMutex, pdMS_TO_TICKS(80))) {
            dutyMap2D[t][r] = fValue;
            sanitizeMapProfileLocked();
            mapCellDirty = true;
            confidenceDirty = true;
            mapNeedsSaving = true;
            xSemaphoreGive(mapMutex);
        } else {
            ok = false;
            error = "map_busy";
        }
    } else {
        ok = false;
        error = "unknown_key";
    }

    if (ok && error.length() == 0 && !key.startsWith("M_")) settingsNeedsSaving = true;
    if (!ok && error.length() == 0) error = "bad_value";

    xSemaphoreGive(configMutex);
    return ok;
}

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *server) override {
        (void)server;
        deviceConnected = true;
        sendSettingsRequested = true;
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

        if (msg.startsWith("TAB:")) {
            int tab = 0;
            if (parseIntStrict(msg.substring(4), 0, NUM_TPS_BINS - 1, tab)) {
                activeBleTab = tab;
                sendSettingsRequested = true;
                sendBleAck("TAB");
            } else {
                sendBleError("TAB", "bad_tab");
            }
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

        if (msg == "SAVE") {
            settingsNeedsSaving = true;
            mapNeedsSaving = true;
            odometerNeedsSaving = true;
            sendBleAck("SAVE");
            return;
        }

        if (msg == "SAVE_MAP") {
            mapNeedsSaving = true;
            sendBleAck("SAVE_MAP");
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

            if (key == "TAB") {
                int tab = 0;
                if (parseIntStrict(val, 0, NUM_TPS_BINS - 1, tab)) {
                    activeBleTab = tab;
                    sendSettingsRequested = true;
                    sendBleAck("SET:TAB");
                } else {
                    sendBleError("SET:TAB", "bad_tab");
                }
                return;
            }

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
    unsigned long dt = now - lastRpmMicros;
    if (dt > 3500) {
        rpmPeriodUs = dt;
        lastRpmMicros = now;
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
    uint16_t config = 0x8583;
    config &= ~0x7000;
    config |= ((4 + channel) << 12);

    Wire.beginTransmission(0x48);
    Wire.write(1);
    Wire.write(static_cast<uint8_t>(config >> 8));
    Wire.write(static_cast<uint8_t>(config & 0xFF));
    if (Wire.endTransmission() != 0) return INT16_MIN;

    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        Wire.beginTransmission(0x48);
        Wire.write(1);
        if (Wire.endTransmission() != 0) break;
        Wire.requestFrom(static_cast<uint8_t>(0x48), static_cast<uint8_t>(2));
        if (Wire.available() == 2) {
            uint16_t status = (Wire.read() << 8) | Wire.read();
            if ((status & 0x8000) != 0) break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    Wire.beginTransmission(0x48);
    Wire.write(0);
    if (Wire.endTransmission() != 0) return INT16_MIN;
    Wire.requestFrom(static_cast<uint8_t>(0x48), static_cast<uint8_t>(2));
    if (Wire.available() == 2) {
        return static_cast<int16_t>((Wire.read() << 8) | Wire.read());
    }
    return INT16_MIN;
}

void updateSafety(const SensorData &d) {
    if (d.rawPIM < 0.1f || d.rawPIM > 4.9f || d.rawVTA < 0.1f || d.rawVTA > 4.9f) {
        systemMode = HARD_LIMP;
    } else if (isnan(d.boost) || d.boost > 1.5f || (d.rpm < 300.0f && d.tps > 5.0f)) {
        systemMode = HARD_LIMP;
    } else if (d.boost > 1.2f) {
        systemMode = SOFT_LIMP;
    } else {
        systemMode = NORMAL;
    }
}

void TaskSensors(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_add(nullptr);
    unsigned long lastSpdCalcMs = millis();
    float rpmHist[3] = {0, 0, 0};
    float smoothedSpeed = 0.0f;
    const float ADS1115_MULTIPLIER = 0.0001875f;

    for (;;) {
        RuntimeConfig localCfg;
        if (takeMutex(configMutex, pdMS_TO_TICKS(20))) {
            localCfg = cfg;
            xSemaphoreGive(configMutex);
        }

        unsigned long nowMs = millis();
        int16_t r0 = safeReadADS1115(0, 10);
        int16_t r1 = safeReadADS1115(1, 10);

        float rawPIM = (r0 == INT16_MIN) ? 0.0f : (r0 * ADS1115_MULTIPLIER);
        float rawVTA = (r1 == INT16_MIN) ? 0.0f : (r1 * ADS1115_MULTIPLIER);

        float boostRaw = constrainFloat((rawPIM - localCfg.offsetPIM) * localCfg.scalePIM, -1.0f, 2.0f);
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
            lastSpdCalcMs = nowMs;
        }

        if (takeMutex(dataMutex, pdMS_TO_TICKS(40))) {
            sensors.rawPIM = rawPIM;
            sensors.rawVTA = rawVTA;
            sensors.boost = sensors.boost * 0.50f + boostRaw * 0.50f;
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
    uint32_t lastPidTime = millis();
    SensorData d = {};

    for (;;) {
        RuntimeConfig localCfg;
        if (takeMutex(configMutex, pdMS_TO_TICKS(20))) {
            localCfg = cfg;
            xSemaphoreGive(configMutex);
        }
        if (takeMutex(dataMutex, pdMS_TO_TICKS(40))) {
            d = sensors;
            xSemaphoreGive(dataMutex);
        }

        updateSafety(d);
        currentBaseDuty = getMappedBaseDuty2D(d.rpm, d.tps);
        float dynamicTarget = localCfg.targetBoost;

        if (d.speed > 10.0f) {
            float gearRatio = d.rpm / max(d.speed, 1.0f);
            if (gearRatio > 100.0f) dynamicTarget -= 0.20f;
            else if (gearRatio > 60.0f) dynamicTarget -= 0.10f;
        } else {
            dynamicTarget -= 0.20f;
        }

        uint32_t now = millis();
        float dt = (now - lastPidTime) / 1000.0f;
        if (dt <= 0.001f) dt = 0.05f;
        lastPidTime = now;

        if (systemMode == NORMAL && d.tps > 10.0f && d.rpm >= 1300.0f) {
            float err = dynamicTarget - d.boost;
            float derivative = (err - pid.lastError) / dt;
            pid.filteredDerivative = pid.filteredDerivative * 0.65f + derivative * 0.35f;
            pid.lastError = err;

            float integralCandidate = pid.integral + err * pid.kI * dt;
            pid.integral = constrainFloat(integralCandidate, -30.0f, 30.0f);

            float duty = currentBaseDuty + (err * pid.kP) + pid.integral + (pid.filteredDerivative * pid.kD);
            if (d.boost > dynamicTarget + 0.15f) {
                duty -= 10.0f;
                pid.integral *= 0.45f;
            }
            if (duty > 85.0f || duty < 0.0f) pid.integral *= 0.92f;

            currentOutDuty = constrainFloat(duty, 0.0f, 85.0f);

            bool pidNotSaturated = currentOutDuty > 8.0f && currentOutDuty < 82.0f;
            if (pidNotSaturated && learningWindowStable(d, err)) {
                float feedForwardCorrection = err * pid.learnCoeff * 0.45f;
                learnDutyMap3D(d.rpm, d.tps, feedForwardCorrection, err);
            }
        } else {
            learningState.stableSinceMs = millis();
            pid.integral *= 0.90f;
            pid.lastError = 0.0f;
            currentOutDuty = (systemMode == SOFT_LIMP) ? 20.0f : 0.0f;
        }

        portENTER_CRITICAL(&rpmMux);
        activePwmDuty = map(static_cast<int>(currentOutDuty), 0, 100, 0, 255);
        portEXIT_CRITICAL(&rpmMux);

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void TaskPWM(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_add(nullptr);
    int lastPwmValue = -1;
    SensorData d = {};

    for (;;) {
        if (takeMutex(dataMutex, pdMS_TO_TICKS(40))) {
            d = sensors;
            xSemaphoreGive(dataMutex);
        }

        int currentActivePwm = 0;
        portENTER_CRITICAL(&rpmMux);
        currentActivePwm = activePwmDuty;
        portEXIT_CRITICAL(&rpmMux);

        int requestedTestDuty = constrain(static_cast<int>(testDuty), 0, 100);
        int pwmValue = (d.speed < 2.0f && requestedTestDuty > 0)
            ? map(requestedTestDuty, 0, 100, 0, 255)
            : currentActivePwm;

        if (pwmValue != lastPwmValue) {
            ledcWrite(solPin, pwmValue);
            lastPwmValue = pwmValue;
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void TaskTelemetry(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_add(nullptr);
    char bleBuffer[512];
    SensorData d = {};
    uint32_t lastPrint = millis();
    uint32_t lastNotifyMs = 0;

    for (;;) {
        if (takeMutex(dataMutex, pdMS_TO_TICKS(40))) {
            d = sensors;
            xSemaphoreGive(dataMutex);
        }

        if (millis() - lastPrint > 5000) {
            Serial.printf("Diag Heap:%u MaxBlock:%u TelStack:%u Mode:%d\n",
                ESP.getFreeHeap(),
                ESP.getMaxAllocHeap(),
                uxTaskGetStackHighWaterMark(nullptr),
                static_cast<int>(systemMode));
            lastPrint = millis();
        }

        if (deviceConnected && millis() - lastNotifyMs >= BLE_NOTIFY_INTERVAL_MS) {
            if (sendSettingsRequested) {
                RuntimeConfig localCfg;
                if (takeMutex(configMutex, pdMS_TO_TICKS(40))) {
                    localCfg = cfg;
                    xSemaphoreGive(configMutex);
                }

                snprintf(bleBuffer, sizeof(bleBuffer),
                    "{\"S\":1,\"pR\":%.1f,\"oP\":%.2f,\"sP\":%.2f,\"oV\":%.2f,\"tB\":%.2f,\"kP\":%.1f,\"kI\":%.1f,\"kD\":%.1f,\"tW\":%d,\"tA\":%d,\"tR\":%d,\"eH\":%.2f,\"vP\":%.2f,\"lA\":%.3f}\n",
                    localCfg.pulsesPerRev, localCfg.offsetPIM, localCfg.scalePIM, localCfg.offsetVTA,
                    localCfg.targetBoost, pid.kP, pid.kI, pid.kD,
                    localCfg.tireW, localCfg.tireA, localCfg.tireR,
                    stationaryEngineHours, localCfg.vssPulsesPerRev, pid.learnCoeff
                );
                sendBleText(bleBuffer);
                vTaskDelay(pdMS_TO_TICKS(SETTINGS_RESEND_INTERVAL_MS));

                if (takeMutex(mapMutex, pdMS_TO_TICKS(40))) {
                    int tab = constrain(activeBleTab, 0, NUM_TPS_BINS - 1);
                    snprintf(bleBuffer, sizeof(bleBuffer),
                        "{\"M\":1,\"tab\":%d,\"w0\":%.1f,\"w1\":%.1f,\"w2\":%.1f,\"w3\":%.1f,\"w4\":%.1f,\"w5\":%.1f,\"w6\":%.1f,\"w7\":%.1f,\"w8\":%.1f,\"w9\":%.1f,\"w10\":%.1f}\n",
                        tab,
                        dutyMap2D[tab][0], dutyMap2D[tab][1], dutyMap2D[tab][2], dutyMap2D[tab][3], dutyMap2D[tab][4],
                        dutyMap2D[tab][5], dutyMap2D[tab][6], dutyMap2D[tab][7], dutyMap2D[tab][8], dutyMap2D[tab][9], dutyMap2D[tab][10]
                    );
                    xSemaphoreGive(mapMutex);
                    sendBleText(bleBuffer);
                }
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
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void TaskOdometerAndStorage(void *pvParameters) {
    (void)pvParameters;
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

        RuntimeConfig localCfg;
        if (takeMutex(configMutex, pdMS_TO_TICKS(20))) {
            localCfg = cfg;
            xSemaphoreGive(configMutex);
        }

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
                xSemaphoreGive(configMutex);
                settingsNeedsSaving = false;
            }
        }

        if (odometerNeedsSaving) {
            prefs.putDouble("oD", totalDistanceKm);
            prefs.putDouble("eH", stationaryEngineHours);
            odometerNeedsSaving = false;
            lastSavedKm = totalDistanceKm;
            lastSavedHours = stationaryEngineHours;
        }

        if (mapNeedsSaving && (millis() - lastMapSaveMs >= 10000)) {
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
            lastMapSaveMs = millis();
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
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
    if (dataMutex == nullptr || mapMutex == nullptr || configMutex == nullptr || bleMutex == nullptr) {
        Serial.println("Crit Error: Mutex creation failed!");
    }

    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_SEC * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&twdt_config);

    if (!prefs.begin("yrv_v5", false)) {
        Serial.println("Crit Error: NVS Init failed!");
    }

    otaModeEnabled = prefs.getBool("ota_mode", false);

    if (takeMutex(configMutex, pdMS_TO_TICKS(100))) {
        cfg.offsetPIM = constrainFloat(prefs.getFloat("oP", 2.57f), 0.1f, 4.5f);
        cfg.scalePIM = constrainFloat(prefs.getFloat("sP", 0.64f), 0.1f, 2.0f);
        cfg.pulsesPerRev = constrainFloat(prefs.getFloat("pR", 2.0f), 0.5f, 8.0f);
        cfg.offsetVTA = constrainFloat(prefs.getFloat("oV", 0.42f), 0.1f, 3.5f);
        cfg.targetBoost = constrainFloat(prefs.getFloat("tB", 0.80f), 0.3f, 1.5f);
        cfg.vssPulsesPerRev = constrainFloat(prefs.getFloat("vP", 5.18f), 1.0f, 40.0f);
        cfg.tireW = constrain(prefs.getInt("tW", 195), 100, 300);
        cfg.tireA = constrain(prefs.getInt("tA", 55), 20, 100);
        cfg.tireR = constrain(prefs.getInt("tR", 15), 10, 24);
        calcWheelSizeLocked();
        pid.kP = constrainFloat(prefs.getFloat("kP", 25.0f), 0.0f, 200.0f);
        pid.kI = constrainFloat(prefs.getFloat("kI", 15.0f), 0.0f, 200.0f);
        pid.kD = constrainFloat(prefs.getFloat("kD", 15.0f), 0.0f, 50.0f);
        pid.learnCoeff = constrainFloat(prefs.getFloat("lA", 0.03f), 0.005f, 0.15f);
        xSemaphoreGive(configMutex);
    }

    totalDistanceKm = prefs.getDouble("oD", 0.0);
    stationaryEngineHours = prefs.getDouble("eH", 0.0);
    loadMap();

    sensors = {0, 0, 0, 0, 0, 1.0f, 0, 0, 0, 0};
    learningState.stableSinceMs = millis();

    if (otaModeEnabled) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(ssid, password);
        setupOTAWebServer();
        return;
    }

    Wire.begin(sdaPin, sclPin);
    Wire.setClock(400000);
    Wire.setTimeOut(20);

    if (!ads.begin()) {
        Serial.println("Error: ADS1115 not found!");
    }
    ads.setGain(GAIN_TWOTHIRDS);
    ads.setDataRate(RATE_ADS1115_860SPS);

    pinMode(rpmPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(rpmPin), handleRPM, FALLING);
    initPCNT();

    ledcAttach(solPin, pwmFreq, pwmRes);
    ledcWrite(solPin, 0);

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
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    pAdvertising->setMinInterval(160);
    pAdvertising->setMaxInterval(320);
    pAdvertising->start();

    xTaskCreatePinnedToCore(TaskSensors, "SENS", 4096, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(TaskControl, "CTRL", 4096, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(TaskPWM, "PWM", 3072, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(TaskTelemetry, "TELEM", 4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(TaskOdometerAndStorage, "ODO_STOR", 4096, nullptr, 1, nullptr, 0);
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
