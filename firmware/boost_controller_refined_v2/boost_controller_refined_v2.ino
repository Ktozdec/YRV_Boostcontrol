#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Adafruit_ADS1X15.h>
#include <Wire.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <Adafruit_MCP4725.h>
#include "driver/pcnt.h"
#include "esp_task_wdt.h"
#include "esp_system.h"

#define WATCHDOG_TIMEOUT_SEC 5
#define BLE_MAX_MESSAGE_LEN 256   // must fit the batch "SETALL:k=v;..." write (~130 chars) with headroom
#define BLE_NOTIFY_INTERVAL_MS 125
// "S" settings packet is sent as a short burst — one copy per telemetry cycle (~125 ms apart) —
// instead of a single notify. A lost/truncated "S" right after connect then self-heals on the next
// cycle, so the app no longer needs the user to re-enter the Settings screen to re-request it.
#define SETTINGS_SEND_BURST 3
// Verbose 5-second serial diagnostics block in TaskTelemetry. Set to 0 for a release build.
#define DEBUG_DIAG 1
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
// Soft/Hard limp are NOT independently tunable: they are ALWAYS derived from the boost target —
// soft = target + 0.15 bar, hard = target + 0.20 bar — so the protection band tracks the target
// automatically and stays a fixed margin above it (see recomputeLimpThresholdsLocked()).
constexpr float SOFT_LIMP_TARGET_OFFSET = 0.15f;
constexpr float HARD_LIMP_TARGET_OFFSET = 0.20f;

// =============================== ADAPTIVE TUNING ===============================
// One place for the knobs that shape boost behaviour and self-learning.
// (Protection thresholds are derived from the target: soft = target + 0.15, hard = target + 0.20 — see above.)

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
constexpr float DEFAULT_KP = 20.0f;   // was 28 — kill plateau hunting (not down to 18: target is close, don't go sluggish)
constexpr float DEFAULT_KI = 14.0f;   // was 16 — keep integral authority (needed to hold the target)
constexpr float DEFAULT_KD = 8.0f;    // was 10 — measurement is noisy, don't amplify it
constexpr float INTEGRAL_LIMIT      = 25.0f;   // anti-windup clamp on the integral term (% duty)
constexpr float AW_TRACK_TAU_S      = 0.30f;   // back-calculation (tracking) anti-windup time constant.
                                               // When the commanded duty saturates at MAP_DUTY_MIN/MAX, the
                                               // integrator is "un-wound" toward the achievable value over
                                               // ~this time, so it stops growing while saturated but stays
                                               // fully active near the setpoint (no deadband trap → no
                                               // permanent under/overboost, and the autotune keeps learning).
constexpr float TARGET_FILTER_TAU_S = 0.35f;   // smoothing time constant for the boost target

// -- Actuator slew limiting --
// The solenoid + wastegate + turbo cannot follow an instant duty step; an unrestricted jump excites
// the boost oscillation. Cap how fast the commanded duty may move (per second, so it is independent of
// loop period). Generous enough that it does not choke spool (≈full 0-100% sweep in ~0.5 s), but it
// removes the abrupt steps that set up hunting. HARD_LIMP bypasses this — protection stays instant.
constexpr float DUTY_SLEW_PER_S = 200.0f;

// -- Open-loop spool assist (transient) --
// On a hard tip-in the PID + slew ramp duty up too gently and only back off AFTER boost has already
// overshot. This commands a high OPEN-LOOP duty while boost is far below target (tight, dense spool),
// then crossfades back down to the PID value BEFORE the target is reached, so turbo momentum doesn't
// carry past it. SPOOL_DUTY_MAX is a TRANSIENT ceiling only (engaged for the brief spool); the
// steady-state duty still tops out at MAP_DUTY_MAX (85%) — the solenoid sticks/overheats if HELD
// above ~85%, but a short spool spike to 95% is safe. The crossfade thresholds (spoolBlendHigh/Low)
// and the downward slew (dutyFallSlewPerS) are runtime-tunable over BLE (see RuntimeConfig).
constexpr float SPOOL_DUTY_MAX        = 95.0f;   // transient-only spool duty ceiling (NOT sustained — do not raise MAP_DUTY_MAX)
constexpr float DUTY_RISE_SLEW_PER_S  = 800.0f;  // fast rise so spool isn't choked; the fall rate is cfg.dutyFallSlewPerS
// Handover defaults, set from the 2026-07-29 road log (the first with the actuation path healthy).
// Across all 8 assist releases the pattern was identical: the assist let go at a deficit of ~0.15
// bar and turbo momentum then carried boost a further +0.22..+0.43 (median +0.33) — i.e. the
// handover was happening roughly 0.2 bar too late, and every hard pull finished past the target,
// three of them into soft/hard limp. Start fading earlier and finish while there is still headroom
// so the carry lands ON target instead of beyond it. Deliberately not the full +0.2 correction:
// a gentler assist spools the turbo less, so the carry shrinks too and over-correcting undershoots.
constexpr float SPOOL_BLEND_HIGH_DEF  = 0.30f;   // deficit (bar) at which the assist is still at full duty
constexpr float SPOOL_BLEND_LOW_DEF   = 0.10f;   // deficit (bar) at which it is fully handed to the PID
// The old 150 %/s was provably the binding constraint on release: 21 of 32 measured fall rates sat
// pinned at the limit while boost ran away 0.62 -> 1.01 bar in 0.4 s. The wastegate was being held
// shut by the rate limiter, not by the controller's intent.
constexpr float DUTY_FALL_SLEW_DEF    = 400.0f;  // power-on default: downward duty slew (%/s)
constexpr float SPOOL_FREEZE_BLEND    = 0.05f;   // freeze the integrator while blend exceeds this (no windup on PID handover)
// Thermal budget for the open-loop assist: 95% duty is safe only as a brief spike, never a hold.
// A genuine spool reaches the target well inside this window; if the assist is STILL running after
// it (target unreachable at this load, cold map, mechanical fault), latch it off so the PID alone
// (capped at MAP_DUTY_MAX = 85%) carries the pull. Re-arms with the spool latch on pedal lift.
constexpr uint32_t SPOOL_MAX_ASSIST_MS = 2500;
// Pedal gate. The assist keys off the boost DEFICIT alone, which made it fire wherever boost sat
// below target — including steady part-throttle cruise, where the throttle plate (not the wastegate)
// is the restriction and the solenoid has no authority at all. Measured on the 2026-07-29 log:
// 44% of an 18-minute drive ran at >=94% duty, 97% of it below 50% pedal, with one unbroken 60 s
// stretch at 95% duty / 20% pedal / 125 km/h / -0.16 bar — i.e. the solenoid was cooked in vacuum
// for nothing. Meanwhile real WOT (70-100% pedal) accounted for only 10% of the assist's runtime.
// Gate it on the driver actually asking for power. Runtime-tunable over BLE (key "sT").
constexpr float SPOOL_MIN_TPS_DEF = 50.0f;

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

// ============================ V2: SELF-TUNING PID ==============================
// The feed-forward map already absorbs the steady-state bias (learnDutyMap3D), so the PID
// only shapes the TRANSIENT. This tuner watches each tip-in "episode", scores it (overshoot,
// rise time, hunting, overboost) and nudges kPa/kIa/kDa by one small bounded step per episode —
// the same converge-over-many-pulls philosophy as the map. The auto gains live in their OWN
// NVS keys (kPa/kIa/kDa), separate from the manual kP/kI/kD that seed them and that take over
// whenever the tuner is disarmed.
constexpr bool  AUTOTUNE_ENABLED_DEF = true;   // master switch (runtime key "aT")
// -- Hard envelope: the tuner can never leave this box (safety) --
constexpr float KP_MIN = 8.0f,  KP_MAX = 40.0f;
constexpr float KI_MIN = 4.0f,  KI_MAX = 28.0f;
constexpr float KD_MIN = 2.0f,  KD_MAX = 20.0f;
// -- Per-episode multiplicative step (so the step scales with the current gain) --
constexpr float AT_STEP_KP = 0.04f;   // +/- 4 %
constexpr float AT_STEP_KI = 0.03f;
constexpr float AT_STEP_KD = 0.05f;
constexpr float AT_EWMA    = 0.30f;   // episode-to-episode metric smoothing
// -- Symptom thresholds --
constexpr float AT_OVERSHOOT_HI = 0.06f;   // bar past target = "too hot"
constexpr float AT_OVERSHOOT_LO = 0.02f;   // essentially no overshoot
constexpr float AT_RISE_HI_MS   = 900.0f;  // time-to-band above this = "sluggish"
constexpr float AT_OSC_MAX      = 3.0f;     // error sign-changes after target = hunting
constexpr float AT_OFFSET_HI    = 0.04f;    // residual |err| at episode end = needs more kI
constexpr float AT_SETTLE_BAND  = 0.03f;
constexpr uint32_t AT_SETTLE_HOLD_MS    = 400;
constexpr uint32_t AT_EPISODE_TIMEOUT_MS = 6000;
// -- Panic de-tune on overboost (asymmetric: back off fast, push up slow) --
constexpr float AT_PANIC_KP = 0.12f;   // -12 % immediately
constexpr float AT_PANIC_KI = 0.15f;
// -- Persistence --
constexpr uint32_t AT_SAVE_INTERVAL_MS         = 60000;
constexpr uint16_t AT_MIN_EPISODES_BEFORE_SAVE = 3;
// ===============================================================================

// ======================= MAP SENSOR / ECU PASSTHROUGH CALIBRATION =======================
// Two INDEPENDENT characteristics. They used to be one pair, which was only correct while the
// input sensor WAS the stock sensor. With the 2-bar sensor fitted they must be separate:
//
//   INPUT  (oP/sP) — the 2-bar sensor we measure and control from.
//     Datasheet: V = 0.0964 + 0.01518 * P[kPa]   ->   P = (V - 0.0964) * 65.88
//     scale = 65.88 kPa/V / 100 = 0.6588 bar/V. (The spec sheet's "4.65 V at 400 kPa" is a typo:
//     4.65 V is 300 kPa, which matches both the slope and the stated 20-300 kPa range.)
//     offset = the voltage AS READ BY THE ADS at atmosphere. Calibrated on the car 2026-07-29:
//     1.590 V gives exactly 0.00 bar with the engine off. A multimeter against SENSOR ground reads
//     ~17 mV higher, which is within a day's barometric drift — the grounds are effectively in
//     agreement. Always trim oP from the ADS value (the "MAP atmosphere" line setup() prints, or
//     the Diag "ADC ch0" line), never from a multimeter: the control loop runs on the ADS reading.
//
//   OUTPUT (oE/sE) — the STOCK sensor characteristic the ECU still expects. Calibrated on the car
//     2026-07-29 against the ECU's own reported MAP: oE set so the ECU reads 99 kPa at atmosphere;
//     sE derived from the idle-vacuum point (ECU 38 kPa at -0.635 bar). Two independent datasets
//     both landed on ~0.53, not the stock-pair guess of 0.55.
//     Do not "simplify" these back into one pair — the engine will not start (the ECU sees roughly
//     half the real air charge and fuels far too lean).
//
//   Both pairs are per-car and per-install. These constants are only the power-on/NVS-erase
//   defaults; the app can trim all four at runtime.
constexpr float DEFAULT_OFFSET_PIM = 1.590f;   // ADS volts at atmosphere (2-bar sensor)
constexpr float DEFAULT_SCALE_PIM  = 0.6588f;  // bar per volt (2-bar sensor)
constexpr float DEFAULT_OFFSET_ECU = 2.621f;   // stock-sensor volts at atmosphere (what the ECU expects)
constexpr float DEFAULT_SCALE_ECU  = 0.530f;   // stock-sensor bar per volt
// Hard clamp on the synthesized ECU signal: never hand the ECU a voltage the stock sensor could not
// physically produce, so a bad calibration cannot trip a MAP-range DTC. 0.60 V ~ full vacuum,
// 4.60 V ~ 1.12 bar gauge — above the FCD limit, so it never interferes with normal capping.
constexpr float ECU_OUT_MIN_VOLTS = 0.60f;
constexpr float ECU_OUT_MAX_VOLTS = 4.60f;
const float DAC_REFERENCE_VOLTAGE = 4.99f;   // measured VDD on the 5V rail (MCP4725 output is ratiometric to VDD)

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
volatile uint32_t testDutySetAtMs = 0;   // when the bench duty was last commanded (for auto-expiry)
// Bench DUTY: override auto-expires — a slider forgotten at 80% must not silently re-engage
// (and cook the solenoid) at the next stop. Also cleared on motion and on BLE disconnect.
constexpr uint32_t TEST_DUTY_TIMEOUT_MS = 120000;

struct PID_Config {
    float kP = DEFAULT_KP;
    float kI = DEFAULT_KI;
    float kD = DEFAULT_KD;
    // Auto-tuned gains (own NVS keys kPa/kIa/kDa), never written by anything but the tuner.
    // The loop runs on these while the tuner is armed; with it disarmed the manual kP/kI/kD above
    // take over directly. Editing a manual gain also re-seeds its auto twin ("tune from here").
    float kPa = DEFAULT_KP;
    float kIa = DEFAULT_KI;
    float kDa = DEFAULT_KD;
    float learnCoeff = DEFAULT_LEARN_RATE;   // integral-offload rate (see ADAPTIVE TUNING above)
    float integral = 0.0f;
    float lastError = 0.0f;
    float lastMeas = 0.0f;      // last boost reading, for derivative-on-measurement (no setpoint kick)
    float filteredDerivative = 0.0f;
} pid;

// V2: self-tuning state (persisted) and the live transient-capture episode (runtime only).
struct AutoTuneState {
    bool   enabled = AUTOTUNE_ENABLED_DEF;
    float  lkgKp = DEFAULT_KP, lkgKi = DEFAULT_KI, lkgKd = DEFAULT_KD;   // last-known-good for divergence rollback
    float  emaOvershoot = 0.0f, emaRise = 0.0f, emaOsc = 0.0f;          // smoothed metrics
    float  prevOvershoot = 0.0f;
    uint8_t divergeStreak = 0;
    uint16_t episodes = 0;
    uint32_t lastSaveMs = 0;
    bool   dirty = false;
} at;

struct Episode {
    bool   active = false;
    uint32_t startMs = 0;
    bool   reachedTarget = false;
    float  peakBoost = 0.0f;     // for overshoot = peak - target
    uint32_t riseMs = 0;         // time from start to first entering the target band
    int8_t lastErrSign = 0;
    uint16_t signChanges = 0;    // hunting counter (error sign flips after reaching target)
    float  lateErr = 0.0f;       // EWMA of |err| once near target (residual offset for kI)
    bool   sawLimp = false;
    uint32_t settleStartMs = 0;
} ep;

struct RuntimeConfig {
    float offsetPIM = DEFAULT_OFFSET_PIM;   // INPUT: 2-bar sensor, ADS volts at atmosphere
    float scalePIM = DEFAULT_SCALE_PIM;     // INPUT: 2-bar sensor, bar/V
    float offsetECU = DEFAULT_OFFSET_ECU;   // OUTPUT: stock-sensor volts at atmosphere (ECU passthrough)
    float scaleECU = DEFAULT_SCALE_ECU;     // OUTPUT: stock-sensor bar/V
    float spoolMinTps = SPOOL_MIN_TPS_DEF;  // pedal gate for the open-loop spool assist (%)
    // true  = target scales with pedal (tpsTargetCurve) — pedal acts as a torque dial.
    // false = flat AVC-R behaviour: chase the full target whenever boost is achievable at all,
    //         regardless of how far the pedal is down. Runtime key "pT".
    bool pedalTargetScaling = true;
    float pulsesPerRev = 2.0f;
    float offsetVTA = 0.42f;
    float targetBoost = 0.80f;
    float limitBoostBar = 0.95f;
    float softLimpBar = 0.95f;   // DERIVED at runtime = targetBoost + 0.15 (recomputeLimpThresholdsLocked); proportional duty bleed above this
    float hardLimpBar = 1.00f;   // DERIVED at runtime = targetBoost + 0.20; duty drops to 0% above this
    float spoolBlendHigh = SPOOL_BLEND_HIGH_DEF;   // boost deficit (bar) for full open-loop spool assist
    float spoolBlendLow  = SPOOL_BLEND_LOW_DEF;    // deficit (bar) where the spool assist fades to pure PID
    float dutyFallSlewPerS = DUTY_FALL_SLEW_DEF;   // downward duty slew rate (%/s); rise is DUTY_RISE_SLEW_PER_S
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
volatile int settingsSendPending = 0;   // >0 = remaining "S" burst packets, one sent per telemetry cycle
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

NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pTxCharacteristic = nullptr;
volatile bool deviceConnected = false;
volatile uint16_t bleConnId = 0;   // current connection id, for per-link MTU lookup
volatile int g_adcFailStreak = 0;  // consecutive ADS1115 read failures (0 = bus healthy)
volatile bool g_ch0ok = false;     // last raw read status, channel 0 (MAP), pre-debounce
volatile bool g_ch1ok = false;     // last raw read status, channel 1 (TPS), pre-debounce
volatile float g_ch0v = 0.0f;      // last raw volts, channel 0 (MAP), pre-debounce
volatile float g_ch1v = 0.0f;      // last raw volts, channel 1 (TPS), pre-debounce
volatile float g_ecuOutVolts = 0.0f;   // last voltage synthesized for the ECU (MCP4725 target)
// The MOMENTARY target the PID is actually chasing (user target x RPM shape x TPS scale, low-passed).
// Telemetered because it is the one number that makes a boost log interpretable: without it an
// overshoot cannot be told apart from a target that legitimately moved under the pedal.
volatile float g_dynamicTarget = 0.0f;

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

// Target-boost multiplier vs TPS (same bins as tpsBins {10,25,40,60,80,100}): partial throttle
// requests partial boost, so the pedal is a torque dial instead of a two-position switch. Full
// target only near WOT. This also removes the "unreachable target" trap: at ~30% throttle the
// engine physically cannot build the full target, which used to leave the open-loop spool assist
// pinned at 95% duty indefinitely (solenoid overheat) and the integrator/map chasing a fiction.
// NOTE: part-throttle map cells learned against the old full target will read high for a few
// drives; the PID pulls them down and the learner re-converges them to the scaled targets.
float tpsTargetCurve[NUM_TPS_BINS] = {0.35f, 0.55f, 0.75f, 0.92f, 1.00f, 1.00f};

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

// Soft/Hard limp thresholds always sit a fixed margin above the boost target (soft = target + 0.15,
// hard = target + 0.20). Call this after any change to cfg.targetBoost. Caller must hold configMutex.
void recomputeLimpThresholdsLocked() {
    cfg.softLimpBar = cfg.targetBoost + SOFT_LIMP_TARGET_OFFSET;
    cfg.hardLimpBar = cfg.targetBoost + HARD_LIMP_TARGET_OFFSET;
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
    // Higher duty = more boost on this solenoid (it bleeds the wastegate signal, holding the gate
    // shut). So the base feed-forward RISES with load (TPS) and RPM to hold the gate against rising
    // exhaust backpressure, with a gentle taper at the very top to protect the turbo. Light throttle
    // stays near zero so the car does not build boost off-throttle; the PID + self-learning trim the rest.
    const float defaults[NUM_TPS_BINS][NUM_RPM_BINS] = {
        {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0},   // 10% TPS — cruise, wastegate on spring
        { 15, 15, 18, 20, 22, 25, 25, 25, 25, 25, 24, 22, 20},   // 25% TPS
        { 25, 28, 32, 35, 38, 40, 42, 42, 42, 42, 40, 38, 36},   // 40% TPS
        { 35, 38, 40, 43, 46, 48, 50, 53, 54, 54, 52, 50, 48},   // 60% TPS  (mid-RPM hump cooled ~10%)
        { 42, 46, 48, 52, 55, 57, 59, 63, 64, 64, 62, 60, 58},   // 80% TPS  (mid-RPM hump cooled ~10%)
        { 48, 52, 55, 58, 62, 65, 67, 71, 72, 72, 70, 68, 66}    // 100% TPS — WOT (mid-RPM hump cooled ~10%)
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

// 1D linear interpolation of the TPS target multiplier (see tpsTargetCurve above).
float getTpsTargetScale(float currentTps) {
    float rTps = constrainFloat(currentTps, tpsBins[0], tpsBins[NUM_TPS_BINS - 1]);
    int t = 0;
    while (t < NUM_TPS_BINS - 2 && rTps >= tpsBins[t + 1]) t++;
    float denom = tpsBins[t + 1] - tpsBins[t];
    if (fabs(denom) < 0.001f) denom = 0.001f;
    float ratio = (rTps - tpsBins[t]) / denom;
    return constrainFloat(tpsTargetCurve[t] + ratio * (tpsTargetCurve[t + 1] - tpsTargetCurve[t]), 0.2f, 1.0f);
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

// V2: scores one finished tip-in episode and applies ONE bounded gain step. Only ever called from
// TaskControl (single task → no locking needed on `ep`/`at`/`pid`). At most one dominant symptom is
// corrected per episode so kP/kI/kD don't fight each other in gain-space.
void autoTunePID(float target, float hardLimpBar) {
    float overshoot = ep.peakBoost - target;
    if (overshoot < 0.0f) overshoot = 0.0f;
    float rise = static_cast<float>(ep.riseMs);
    float osc = static_cast<float>(ep.signChanges);
    bool overboost = ep.sawLimp || ep.peakBoost >= hardLimpBar;

    // Smooth the metrics so a single noisy pull can't yank the gains.
    at.emaOvershoot = (1.0f - AT_EWMA) * at.emaOvershoot + AT_EWMA * overshoot;
    at.emaRise      = (1.0f - AT_EWMA) * at.emaRise + AT_EWMA * rise;
    at.emaOsc       = (1.0f - AT_EWMA) * at.emaOsc + AT_EWMA * osc;

    // Divergence watchdog: overshoot growing episode-over-episode → roll back to last-known-good.
    if (overshoot > at.prevOvershoot + 0.005f && overshoot > AT_OVERSHOOT_HI) {
        if (at.divergeStreak < 255) at.divergeStreak++;
    } else {
        at.divergeStreak = 0;
    }
    at.prevOvershoot = overshoot;

    if (at.divergeStreak >= 3) {
        pid.kPa = at.lkgKp; pid.kIa = at.lkgKi; pid.kDa = at.lkgKd;
        at.emaOvershoot = at.emaRise = at.emaOsc = 0.0f;
        at.divergeStreak = 0;
    } else if (overboost) {
        pid.kPa *= (1.0f - AT_PANIC_KP);
        pid.kIa *= (1.0f - AT_PANIC_KI);
    } else if (at.emaOsc > AT_OSC_MAX) {
        pid.kPa *= (1.0f - AT_STEP_KP);
        pid.kIa *= (1.0f - AT_STEP_KI);
    } else if (at.emaOvershoot > AT_OVERSHOOT_HI) {
        pid.kPa *= (1.0f - AT_STEP_KP);
        pid.kDa *= (1.0f + AT_STEP_KD);
    } else if (at.emaRise > AT_RISE_HI_MS && at.emaOvershoot < AT_OVERSHOOT_LO) {
        pid.kPa *= (1.0f + AT_STEP_KP);
    } else if (ep.lateErr > AT_OFFSET_HI) {
        pid.kIa *= (1.0f + AT_STEP_KI);
    } else {
        // Clean episode → snapshot the current gains as the rollback target.
        at.lkgKp = pid.kPa; at.lkgKi = pid.kIa; at.lkgKd = pid.kDa;
    }

    pid.kPa = constrainFloat(pid.kPa, KP_MIN, KP_MAX);
    pid.kIa = constrainFloat(pid.kIa, KI_MIN, KI_MAX);
    pid.kDa = constrainFloat(pid.kDa, KD_MIN, KD_MAX);

    at.episodes++;
    at.dirty = true;

    uint32_t nowMs = millis();
    if (at.episodes >= AT_MIN_EPISODES_BEFORE_SAVE && nowMs - at.lastSaveMs >= AT_SAVE_INTERVAL_MS) {
        settingsNeedsSaving = true;   // flushed by TaskOdometerAndStorage (throttled)
        at.lastSaveMs = nowMs;
    }

    Serial.printf("AT ep#%u ovs:%.3f rise:%.0f osc:%.0f -> kP:%.1f kI:%.1f kD:%.1f%s\n",
        at.episodes, overshoot, rise, osc, pid.kPa, pid.kIa, pid.kDa, overboost ? " PANIC" : "");
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
    } else if (key == "oE") {
        // ECU passthrough characteristic — stock-sensor volts at atmosphere.
        ok = parseFloatStrict(rawValue, 0.1f, 4.5f, fValue);
        if (ok) cfg.offsetECU = fValue;
    } else if (key == "sE") {
        // ECU passthrough characteristic — stock-sensor bar/V (never 0: used as a divisor).
        ok = parseFloatStrict(rawValue, 0.1f, 2.0f, fValue);
        if (ok) cfg.scaleECU = fValue;
    } else if (key == "sT") {
        ok = parseFloatStrict(rawValue, 20.0f, 100.0f, fValue);
        if (ok) cfg.spoolMinTps = fValue;
    } else if (key == "pT") {
        // 1 = pedal-scaled target, 0 = flat full target (AVC-R style).
        ok = parseIntStrict(rawValue, 0, 1, iValue);
        if (ok) cfg.pedalTargetScaling = (iValue != 0);
    } else if (key == "oV") {
        ok = parseFloatStrict(rawValue, 0.1f, 3.5f, fValue);
        if (ok) cfg.offsetVTA = fValue;
    } else if (key == "tB") {
        ok = parseFloatStrict(rawValue, 0.3f, 1.5f, fValue);
        if (ok) {
            cfg.targetBoost = fValue;
            recomputeLimpThresholdsLocked();   // keep soft/hard limp at target + 0.15 / + 0.20
        }
    } else if (key == "kP") {
        ok = parseFloatStrict(rawValue, 0.0f, 200.0f, fValue);
        // V2: a manual edit re-seeds the auto gain ("tune from here") so the slider stays meaningful;
        // the self-tuner then refines kPa from this point. Re-seed ONLY on a real change — the app
        // re-sends the unchanged slider value with every settings save, and that must not keep
        // wiping the self-tuned kPa back to the seed.
        if (ok) {
            bool changed = fabsf(fValue - pid.kP) > 0.005f;
            pid.kP = fValue;
            if (changed) { pid.kPa = constrainFloat(fValue, KP_MIN, KP_MAX); at.lkgKp = pid.kPa; }
        }
    } else if (key == "kI") {
        ok = parseFloatStrict(rawValue, 0.0f, 200.0f, fValue);
        if (ok) {
            bool changed = fabsf(fValue - pid.kI) > 0.005f;
            pid.kI = fValue;
            if (changed) { pid.kIa = constrainFloat(fValue, KI_MIN, KI_MAX); at.lkgKi = pid.kIa; }
        }
    } else if (key == "kD") {
        ok = parseFloatStrict(rawValue, 0.0f, 50.0f, fValue);
        if (ok) {
            bool changed = fabsf(fValue - pid.kD) > 0.005f;
            pid.kD = fValue;
            if (changed) { pid.kDa = constrainFloat(fValue, KD_MIN, KD_MAX); at.lkgKd = pid.kDa; }
        }
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
    } else if (key == "sL" || key == "hL") {
        // Soft/Hard limp can no longer be set directly — they are derived from the target
        // (target + 0.15 / + 0.20). Accept the legacy key without error (the app still sends it),
        // but ignore the value and re-derive so the band always tracks the target.
        recomputeLimpThresholdsLocked();
        ok = true;
    } else if (key == "bH") {
        ok = parseFloatStrict(rawValue, 0.05f, 1.0f, fValue);
        if (ok) cfg.spoolBlendHigh = fValue;
    } else if (key == "bL") {
        ok = parseFloatStrict(rawValue, 0.0f, 0.5f, fValue);
        if (ok) cfg.spoolBlendLow = fValue;
    } else if (key == "fS") {
        ok = parseFloatStrict(rawValue, 50.0f, 1000.0f, fValue);
        if (ok) cfg.dutyFallSlewPerS = fValue;
    } else if (key == "aT") {
        // V2: enable/disable the self-tuning PID at runtime.
        ok = parseIntStrict(rawValue, 0, 1, iValue);
        if (ok) at.enabled = (iValue != 0);
    } else {
        ok = false;
        error = "unknown_key";
    }

    if (ok && error.length() == 0) settingsNeedsSaving = true;
    if (!ok && error.length() == 0) error = "bad_value";

    xSemaphoreGive(configMutex);
    return ok;
}

class MyServerCallbacks : public NimBLEServerCallbacks {
    // NimBLE-Arduino 2.x signatures. On 1.4.x they are:
    //   onConnect(NimBLEServer*, ble_gap_conn_desc*) / onDisconnect(NimBLEServer*, ble_gap_conn_desc*)
    // and the handle is desc->conn_handle instead of connInfo.getConnHandle().
    void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
        deviceConnected = true;
        bleConnId = connInfo.getConnHandle();
        settingsSendPending = SETTINGS_SEND_BURST;
        // Request fast connection interval right after connecting:
        // min=12*1.25ms=15ms, max=24*1.25ms=30ms, latency=0, timeout=400*10ms=4s
        server->updateConnParams(connInfo.getConnHandle(), 12, 24, 0, 400);
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
        (void)server;
        (void)connInfo;
        (void)reason;
        deviceConnected = false;
        settingsSendPending = 0;
        testDuty = 0;   // never leave a bench test duty running with nobody connected to stop it
        NimBLEDevice::startAdvertising();
    }
};

// Command handling runs OFF the BLE callback. onWrite() only copies the bytes into a queue and
// returns immediately, so the BLE host task never blocks on configMutex or flash. TaskBleCommands
// drains the queue and does the real work (parse, validate, apply, ACK).
struct BleCommand {
    char data[BLE_MAX_MESSAGE_LEN + 1];
};
QueueHandle_t bleCmdQueue = nullptr;

void enqueueBleCommand(const String &raw) {
    if (bleCmdQueue == nullptr || raw.length() == 0) return;
    // Too long to fit the fixed buffer — drop it (the app never sends anything near this big).
    if (raw.length() > BLE_MAX_MESSAGE_LEN) return;
    BleCommand cmd;
    size_t n = raw.length();
    memcpy(cmd.data, raw.c_str(), n);
    cmd.data[n] = '\0';
    // Non-blocking: if the queue is somehow full we drop rather than stall the BLE host task.
    xQueueSend(bleCmdQueue, &cmd, 0);
}

void processBleCommand(const String &msg) {
    if (msg == "GET:SETTINGS") {
        settingsSendPending = SETTINGS_SEND_BURST;
        sendBleAck("GET:SETTINGS");
        return;
    }

    if (msg.startsWith("DUTY:")) {
        int duty = 0;
        if (parseIntStrict(msg.substring(5), 0, 100, duty)) {
            testDuty = duty;
            testDutySetAtMs = millis();
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
        settingsSendPending = SETTINGS_SEND_BURST;   // re-push confirmed settings so the app re-syncs after save
        sendBleAck("SAVE");
        return;
    }

    if (msg == "OTA:ON") {
        requestReboot(REBOOT_TO_OTA);
        sendBleAck("OTA");
        return;
    }

    // Batch settings write: "SETALL:k1=v1;k2=v2;...". One BLE round-trip instead of ~16 separate
    // SET commands; each pair goes through the same validated path as a single SET.
    if (msg.startsWith("SETALL:")) {
        String body = msg.substring(7);
        int failed = 0;
        int start = 0;
        while (start < (int)body.length()) {
            int semi = body.indexOf(';', start);
            String pair = (semi < 0) ? body.substring(start) : body.substring(start, semi);
            pair.trim();
            if (pair.length() > 0) {
                int eq = pair.indexOf('=');
                if (eq > 0) {
                    String key = pair.substring(0, eq);
                    String val = pair.substring(eq + 1);
                    String err;
                    if (!updateSettingValue(key, val, err)) failed++;
                } else {
                    failed++;
                }
            }
            if (semi < 0) break;
            start = semi + 1;
        }
        if (failed == 0) { settingsSendPending = SETTINGS_SEND_BURST; sendBleAck("SETALL"); }
        else sendBleError("SETALL", "partial");
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
            // Re-push the "S" packet so the app UI reflects the applied value. Without this a lone
            // SET (e.g. the aT autotune toggle) changed firmware state but the app kept rendering
            // the stale settings snapshot — the button looked dead.
            settingsSendPending = SETTINGS_SEND_BURST;
            sendBleAck(key.c_str());
        } else {
            sendBleError(key.c_str(), error.c_str());
        }
        return;
    }

    sendBleError("BLE", "unknown_cmd");
}

void TaskBleCommands(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_add(nullptr);
    BleCommand cmd;
    for (;;) {
        if (xQueueReceive(bleCmdQueue, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
            String msg(cmd.data);
            msg.trim();
            if (msg.length() > 0) processBleCommand(msg);
        }
        esp_task_wdt_reset();
    }
}

class MyCallbacks : public NimBLECharacteristicCallbacks {
    // NimBLE 2.x passes NimBLEConnInfo; on 1.4.x it is just onWrite(NimBLECharacteristic*).
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
        (void)connInfo;
        enqueueBleCommand(String(pCharacteristic->getValue().c_str()));
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

void updateSafety(const SensorData &d, float softLimpBar, float hardLimpBar) {
    if (d.rawPIM < 0.1f || d.rawPIM > 4.9f || d.rawVTA < 0.1f || d.rawVTA > 4.9f) {
        systemMode = HARD_LIMP;
    } else if (isnan(d.boost) || d.boost > hardLimpBar || (d.rpm < 300.0f && d.tps > 5.0f)) {
        systemMode = HARD_LIMP;
    } else if (d.boost > softLimpBar) {
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

// Median of the 5-element RPM history. Rejects up to 2 outliers and smooths the real
// cycle-to-cycle idle variation, while staying responsive at higher RPM.
static float medianRpm5(const float h[5]) {
    float a[5];
    for (int i = 0; i < 5; i++) a[i] = h[i];
    for (int i = 1; i < 5; i++) {
        float key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
    return a[2];
}

void TaskSensors(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_add(nullptr);
    unsigned long lastSpdCalcMs = millis();
    float rpmHist[5] = {0, 0, 0, 0, 0};
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
        // Seeded from the live calibration so a bus glitch in the first cycles rides on a real
        // atmosphere reading, not a hard-coded value from the previous sensor.
        static float lastGoodPIM = localCfg.offsetPIM;
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
        // Read the 2-bar sensor through its OWN characteristic (oP/sP).
        float real_boost_bar = (filtered_map_volts - localCfg.offsetPIM) * localCfg.scalePIM;

        // Synthesize the ECU signal through the STOCK sensor characteristic (oE/sE) — the ECU still
        // lives in the old sensor's coordinate system and must never see the 2-bar scaling. Capped
        // at limitBoostBar (the FCD shelf), then clamped to a physically plausible stock-sensor range.
        float ecu_boost_bar = min(real_boost_bar, localCfg.limitBoostBar);
        float out_volts = (ecu_boost_bar / localCfg.scaleECU) + localCfg.offsetECU;
        out_volts = constrainFloat(out_volts, ECU_OUT_MIN_VOLTS, ECU_OUT_MAX_VOLTS);
        g_ecuOutVolts = out_volts;   // surfaced in the Diag block for bench verification
        uint16_t dac_value = (uint16_t)((out_volts / DAC_REFERENCE_VOLTAGE) * 4095.0f);
        if (DAC_OUTPUT_ENABLED) {
            dac.setVoltage(dac_value, false);   // disable via DAC_OUTPUT_ENABLED to A/B test the bus
        }

        float tpsSpan = max(0.3f, 3.71f - localCfg.offsetVTA);
        float tpsRaw = constrainFloat((rawVTA - localCfg.offsetVTA) / tpsSpan * 100.0f, 0.0f, 100.0f);

        unsigned long period;
        unsigned long lastTime;
        uint32_t currentEdges;
        portENTER_CRITICAL(&rpmMux);
        period = rpmPeriodUs;
        lastTime = lastRpmMicros;
        currentEdges = g_rpmEdges;
        portEXIT_CRITICAL(&rpmMux);

        float instRPM = 0.0f;
        if (micros() - lastTime <= 150000 && period > 0) {
            instRPM = (1000000.0f / period) * 60.0f / localCfg.pulsesPerRev;
        }
        instRPM = constrainFloat(instRPM, 0.0f, 12000.0f);

        // Shift the history ONLY when a real new tach edge arrived (per-edge, not per-10 ms loop).
        // This is the key fix: otherwise the same period is copied into several slots and the
        // median follows the jitter instead of rejecting it. With real consecutive periods, a
        // jitter pair [long, short] cancels out and the median returns the true value.
        static uint32_t lastProcessedEdges = 0;
        if (currentEdges != lastProcessedEdges) {
            for (int i = 4; i > 0; i--) rpmHist[i] = rpmHist[i - 1];
            rpmHist[0] = instRPM;
            lastProcessedEdges = currentEdges;
        } else if (instRPM == 0.0f) {
            for (int i = 0; i < 5; i++) rpmHist[i] = 0.0f;   // engine stopped → clear stale history
        }

        float medRPM = medianRpm5(rpmHist);

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
            sensors.boost = constrainFloat(real_boost_bar, -1.0f, 2.2f);   // 2-bar sensor tops out ~1.99 bar gauge
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

        updateSafety(d, localCfg.softLimpBar, localCfg.hardLimpBar);
        currentBaseDuty = getMappedBaseDuty2D(d.rpm, d.tps);

        // F1: precise loop timing from micros() (unsigned subtraction handles the ~71 min wrap).
        uint32_t nowUs = micros();
        float dt = (nowUs - lastPidMicros) / 1000000.0f;
        if (dt <= 0.0002f || dt > 0.5f) dt = CONTROL_PERIOD_MS / 1000.0f;
        lastPidMicros = nowUs;

        // Momentary target = user target × RPM shape × TPS scale. The 0.35 s target filter below
        // smooths pedal-driven target moves; on a fast tip-in the deficit saturates the spool blend
        // from the first cycle anyway, so spool feel is unchanged at WOT.
        // With pedalTargetScaling off the TPS term drops out entirely: the target is flat at the
        // user's setting and the controller chases it whenever boost is achievable (AVC-R style).
        float pedalScale = localCfg.pedalTargetScaling ? getTpsTargetScale(d.tps) : 1.0f;
        float shapedTarget = localCfg.targetBoost * getTargetShape(d.rpm) * pedalScale;
        float desiredDynamicTarget = constrainFloat(shapedTarget, 0.30f, localCfg.targetBoost);
        if (!filteredDynamicTargetInitialized) {
            filteredDynamicTarget = desiredDynamicTarget;
            filteredDynamicTargetInitialized = true;
        } else {
            float alpha = dt / (TARGET_FILTER_TAU_S + dt);
            filteredDynamicTarget += (desiredDynamicTarget - filteredDynamicTarget) * alpha;
        }

        float dynamicTarget = filteredDynamicTarget;
        g_dynamicTarget = dynamicTarget;
        float targetRate = (dynamicTarget - prevControlTarget) / dt;   // bar/s, smooth (target is low-passed)
        prevControlTarget = dynamicTarget;

        // One-shot spool latch: the open-loop assist must fire only on the genuine spool-up, not re-arm
        // every time boost dips below target during regulation. Re-firing slammed duty back to ~95% on
        // each dip and set up a boost/duty limit cycle (logged: boost 1.06→0.75→0.99 hunting, duty
        // 47→95→54). spoolArmed latches OFF once boost first reaches the target band (below) and re-arms
        // on pedal lift (else branch), so a fresh throttle application / post-shift gets a full spool.
        static bool spoolArmed = true;
        static uint32_t spoolAssistSinceMs = 0;   // start of the CONTINUOUS assist run; 0 = not assisting

        if (systemMode != HARD_LIMP && d.tps > 10.0f && d.rpm >= 1300.0f) {
            float err = dynamicTarget - d.boost;

            // Gain scheduling: aggressive on spool, gentle up top.
            float gainFactor = spoolGainFactor(d.rpm);
            // Autotune ON  -> run on the self-tuned gains; the manual kP/kI/kD act as the seed and
            //                 the "tune from here" reset (see updateSettingValue).
            // Autotune OFF -> run on the manual gains, so switching the tuner off actually pins the
            //                 gains the user can see. Previously the loop kept using kPa/kIa/kDa
            //                 whatever the switch said, so the displayed kP and the running gain
            //                 could silently disagree. Clamped to the same safety envelope the
            //                 tuner is held to: the manual sliders accept 0..200, which must never
            //                 reach the loop unbounded.
            float kpBase = at.enabled ? pid.kPa : constrainFloat(pid.kP, KP_MIN, KP_MAX);
            float kiBase = at.enabled ? pid.kIa : constrainFloat(pid.kI, KI_MIN, KI_MAX);
            float kdBase = at.enabled ? pid.kDa : constrainFloat(pid.kD, KD_MIN, KD_MAX);
            float kpEff = kpBase * gainFactor;
            float kiEff = kiBase * (1.0f + (gainFactor - 1.0f) * 0.5f);
            float kdEff = kdBase;

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

            // Open-loop spool assist: how far below target are we (bar)? blend = 1 when the deficit is
            // large (full assist → command SPOOL_DUTY_MAX), fading to 0 as boost nears the target so we
            // hand smoothly back to the PID. This is what makes spool dense AND tames the overshoot:
            // the duty starts easing down BEFORE the target instead of after boost has already shot past.
            float spoolDeficit = dynamicTarget - d.boost;
            float spoolSpan = max(localCfg.spoolBlendHigh - localCfg.spoolBlendLow, 0.01f);
            float spoolBlend = constrainFloat((spoolDeficit - localCfg.spoolBlendLow) / spoolSpan, 0.0f, 1.0f);
            // Latch the assist off the moment boost first reaches the target band; from then on the PID
            // regulates alone for the rest of this pull (a momentary dip no longer re-slams 95% duty).
            if (spoolDeficit < localCfg.spoolBlendLow) spoolArmed = false;
            // Pedal gate (see SPOOL_MIN_TPS_DEF): the assist only has authority when the driver has
            // actually opened the throttle. At part throttle the restriction is the throttle plate,
            // not the wastegate, so commanding 95% duty there achieves nothing and cooks the solenoid.
            bool spoolPedalOk = d.tps >= localCfg.spoolMinTps;
            float effSpoolBlend = (spoolArmed && spoolPedalOk) ? spoolBlend : 0.0f;
            bool spoolAssisting = effSpoolBlend > SPOOL_FREEZE_BLEND;

            // Thermal budget (SPOOL_MAX_ASSIST_MS): the solenoid must never be HELD above 85%.
            // If the assist runs continuously past the budget without the target being reached,
            // latch it off for the rest of this pull — the PID (≤ MAP_DUTY_MAX) takes over.
            if (spoolAssisting) {
                uint32_t spoolNowMs = millis();
                if (spoolAssistSinceMs == 0) {
                    spoolAssistSinceMs = spoolNowMs;
                } else if (spoolNowMs - spoolAssistSinceMs >= SPOOL_MAX_ASSIST_MS) {
                    spoolArmed = false;
                    effSpoolBlend = 0.0f;
                    spoolAssisting = false;
                }
            } else {
                spoolAssistSinceMs = 0;
            }

            // Back-calculation (tracking) anti-windup. Integrate EVERY cycle — no setpoint-band gate
            // (the old INTEGRAL_BAND froze the integrator whenever |err| exceeded the band, which on a
            // cold/cool map left a permanent steady-state offset it could never trim out → underboost
            // trap, and starved the autotune). To stop windup while the output is saturated, we form the
            // unclamped output, clamp it to the actuator range, and feed the saturation excess
            // (dutyClamped - dutyUnclamped) back into the integrator over AW_TRACK_TAU_S. While saturated
            // the integrator "tracks" the achievable value instead of growing; near the setpoint the
            // correction is ~0 so the integrator works normally and eliminates the offset.
            // While the open-loop spool assist dominates, freeze fresh integration (kiThisCycle = 0) so
            // the integrator doesn't wind up behind the 95% open-loop duty and overshoot on handover —
            // the back-calc term still runs, letting it settle to the achievable bias.
            float kiThisCycle = spoolAssisting ? 0.0f : kiEff;
            float integralRaw = pid.integral + err * kiThisCycle * dt;
            float dutyUnclamped = currentBaseDuty + (err * kpEff) + integralRaw + (pid.filteredDerivative * kdEff) + setpointFF;
            float dutyClamped = constrainFloat(dutyUnclamped, MAP_DUTY_MIN, MAP_DUTY_MAX);
            integralRaw += (dutyClamped - dutyUnclamped) * (dt / AW_TRACK_TAU_S);
            pid.integral = constrainFloat(integralRaw, -INTEGRAL_LIMIT, INTEGRAL_LIMIT);

            // Crossfade the PID duty (≤ MAP_DUTY_MAX) up toward the transient spool ceiling. max() keeps
            // it assist-only: we never pull BELOW what the PID asks. Above target spoolBlend is 0, so
            // this collapses to the plain PID output and the soft-limp bleed below behaves as before.
            float spoolTargetDuty = (1.0f - effSpoolBlend) * dutyClamped + effSpoolBlend * SPOOL_DUTY_MAX;
            currentOutDuty = max(dutyClamped, spoolTargetDuty);

            // Soft limp: instead of slamming duty to a fixed 20% step (which collapsed spool and set
            // up the boost oscillation), bleed the PID output down PROPORTIONALLY to how far boost has
            // overshot the soft threshold — full output at softLimpBar, fading linearly to 0 as it
            // approaches hardLimpBar. The PID keeps running underneath, so recovery is a smooth ramp
            // back up, not an abrupt relatch. Hard limp (boost > hardLimpBar) still cuts to 0 below.
            if (systemMode == SOFT_LIMP) {
                float span = max(localCfg.hardLimpBar - localCfg.softLimpBar, 0.05f);
                float x = constrainFloat((d.boost - localCfg.softLimpBar) / span, 0.0f, 1.0f);
                // Quadratic soft-start: bleed = 1 - x^2. Near the soft threshold (x small) the squeeze
                // is almost nil so the PID alone handles minor overshoot; it ramps in steeply only as
                // boost approaches hardLimpBar, avoiding a fight with the PID right at the boundary.
                float bleed = 1.0f - x * x;
                currentOutDuty *= bleed;
            }

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

            // ---- V2 self-tuning PID: capture this tip-in as a "tuning episode" ----
            // Purely observational — reads err/boost/mode already computed above, never alters the
            // duty path. A valid episode ends on settle or timeout and feeds autoTunePID().
            if (at.enabled) {
                uint32_t atNowMs = millis();
                if (!ep.active) {
                    bool genuineTipIn = spoolArmed && spoolDeficit > localCfg.spoolBlendHigh &&
                                        d.tps > LEARN_MIN_TPS && d.rpm > LEARN_MIN_RPM && d.speed > LEARN_MIN_SPEED;
                    if (genuineTipIn) {
                        ep = Episode{};
                        ep.active = true;
                        ep.startMs = atNowMs;
                        ep.peakBoost = d.boost;
                    }
                } else {
                    if (d.boost > ep.peakBoost) ep.peakBoost = d.boost;
                    if (systemMode != NORMAL) ep.sawLimp = true;
                    if (!ep.reachedTarget && err < AT_SETTLE_BAND) {
                        ep.reachedTarget = true;
                        ep.riseMs = atNowMs - ep.startMs;
                        ep.lastErrSign = (err >= 0.0f) ? 1 : -1;
                    }
                    if (ep.reachedTarget) {
                        ep.lateErr = ep.lateErr * 0.8f + fabs(err) * 0.2f;
                        if (fabs(err) > 0.015f) {   // noise gate on the hunting counter
                            int8_t s = (err >= 0.0f) ? 1 : -1;
                            if (s != ep.lastErrSign) { ep.signChanges++; ep.lastErrSign = s; }
                        }
                        if (fabs(err) < AT_SETTLE_BAND) {
                            if (ep.settleStartMs == 0) ep.settleStartMs = atNowMs;
                        } else {
                            ep.settleStartMs = 0;
                        }
                        bool settleDone = ep.settleStartMs != 0 && (atNowMs - ep.settleStartMs >= AT_SETTLE_HOLD_MS);
                        bool timeoutDone = (atNowMs - ep.startMs >= AT_EPISODE_TIMEOUT_MS);
                        if (settleDone || timeoutDone) {
                            autoTunePID(dynamicTarget, localCfg.hardLimpBar);
                            ep.active = false;
                        }
                    } else if (atNowMs - ep.startMs >= AT_EPISODE_TIMEOUT_MS) {
                        ep.active = false;   // never reached target → discard (interrupted pull)
                    }
                }
            }
        } else {
            // V2: pedal lift / hard limp while an episode was open — finalize if it reached target
            // (peakBoost already captured any overboost), otherwise discard.
            if (ep.active) {
                if (at.enabled && ep.reachedTarget) autoTunePID(dynamicTarget, localCfg.hardLimpBar);
                ep.active = false;
            }
            learningState.stableSinceMs = millis();
            pid.integral *= 0.90f;
            pid.lastError = 0.0f;
            pid.lastMeas = d.boost;
            lastMeasMicros = nowUs;
            pid.filteredDerivative = 0.0f;
            // Pedal lifted / idle: re-arm the spool latch so the next throttle application (or the
            // next gear after a shift) gets a fresh full open-loop spool, with a fresh thermal budget.
            spoolArmed = true;
            spoolAssistSinceMs = 0;
            // Hard limp or genuine idle (low TPS/RPM): solenoid fully released, wastegate open.
            // Soft limp now stays in the PID branch above and bleeds down smoothly instead.
            currentOutDuty = 0.0f;
        }

        // Slew-rate limit: ramp the commanded duty toward its target instead of stepping, so the
        // solenoid never gets an instant jump it can't follow. HARD_LIMP bypasses it — protection
        // must drop the gate immediately, not ramp down.
        static float slewLimitedDuty = 0.0f;
        if (systemMode == HARD_LIMP) {
            slewLimitedDuty = 0.0f;
        } else {
            // Asymmetric: rise fast (DUTY_RISE_SLEW_PER_S) so the spool isn't choked; fall at the
            // tunable cfg.dutyFallSlewPerS so the proactive spool back-off stays controlled.
            float riseStep = DUTY_RISE_SLEW_PER_S * dt;
            float fallStep = localCfg.dutyFallSlewPerS * dt;
            slewLimitedDuty += constrainFloat(currentOutDuty - slewLimitedDuty, -fallStep, riseStep);
        }
        currentOutDuty = slewLimitedDuty;

        // Bench test duty is self-expiring: cleared as soon as the car actually moves, and after
        // TEST_DUTY_TIMEOUT_MS regardless (also cleared on BLE disconnect). Without this a
        // leftover slider value re-engaged the solenoid at every stop (speed < 2).
        if (testDuty > 0 && (d.speed >= 2.0f || millis() - testDutySetAtMs >= TEST_DUTY_TIMEOUT_MS)) {
            testDuty = 0;
        }

        // F2: write the solenoid PWM immediately here — no separate TaskPWM, no 0–20 ms dead time.
        // Manual solenoid test (DUTY:) still overrides while the car is stationary.
        // Round to the full 0-255 range (not truncate at 1% steps) for finer solenoid resolution.
        int requestedTestDuty = constrain(static_cast<int>(testDuty), 0, 100);
        int pwmByte = (d.speed < 2.0f && requestedTestDuty > 0)
            ? static_cast<int>(lroundf(requestedTestDuty / 100.0f * 255.0f))
            : static_cast<int>(lroundf(constrainFloat(currentOutDuty, 0.0f, 100.0f) / 100.0f * 255.0f));
        pwmByte = constrain(pwmByte, 0, 255);
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
            lastPrint = millis();
#if DEBUG_DIAG
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
            // ch0 volts is the number to type into oP when the manifold is at atmosphere.
            // ECUout is what the MCP4725 is feeding the ECU right now — check it against a
            // multimeter on the signal wire, and against the MAP value the ECU itself reports.
            Serial.printf("ADC ch0(MAP):%s %.3fV  ch1(TPS):%s %.3fV  DAC:%s  ECUout:%.3fV\n",
                g_ch0ok ? "OK" : "FAIL", g_ch0v,
                g_ch1ok ? "OK" : "FAIL", g_ch1v,
                DAC_OUTPUT_ENABLED ? "on" : "off", g_ecuOutVolts);
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
#endif
        }

        if (deviceConnected && millis() - lastNotifyMs >= BLE_NOTIFY_INTERVAL_MS) {
            if (settingsSendPending > 0) {
                RuntimeConfig localCfg = snapshotConfig();

                snprintf(bleBuffer, sizeof(bleBuffer),
                    "{\"S\":1,\"pR\":%.1f,\"oP\":%.3f,\"sP\":%.4f,\"oE\":%.3f,\"sE\":%.4f,\"oV\":%.2f,\"tB\":%.2f,\"lB\":%.2f,\"sL\":%.2f,\"hL\":%.2f,\"kP\":%.1f,\"kI\":%.1f,\"kD\":%.1f,\"tW\":%d,\"tA\":%d,\"tR\":%d,\"eH\":%.2f,\"vP\":%.2f,\"lA\":%.3f,\"bH\":%.2f,\"bL\":%.2f,\"fS\":%.0f,\"sT\":%.0f,\"pT\":%d,\"aT\":%d}\n",
                    localCfg.pulsesPerRev, localCfg.offsetPIM, localCfg.scalePIM,
                    localCfg.offsetECU, localCfg.scaleECU, localCfg.offsetVTA,
                    localCfg.targetBoost, localCfg.limitBoostBar, localCfg.softLimpBar, localCfg.hardLimpBar,
                    pid.kP, pid.kI, pid.kD,
                    localCfg.tireW, localCfg.tireA, localCfg.tireR,
                    stationaryEngineHours, localCfg.vssPulsesPerRev, pid.learnCoeff,
                    localCfg.spoolBlendHigh, localCfg.spoolBlendLow, localCfg.dutyFallSlewPerS,
                    localCfg.spoolMinTps, localCfg.pedalTargetScaling ? 1 : 0, at.enabled ? 1 : 0
                );
                sendBleText(bleBuffer);
                settingsSendPending--;   // one packet per cycle: burst is spread across telemetry cycles, no stall
            }

            snprintf(bleBuffer, sizeof(bleBuffer),
                "{\"T\":1,\"b\":%.2f,\"miB\":%.2f,\"maB\":%.2f,\"r\":%.0f,\"maR\":%.0f,\"s\":%.0f,\"maS\":%.0f,\"v\":%.0f,\"oD\":%.2f,\"bD\":%.1f,\"cD\":%.1f,\"mode\":%d,\"tg\":%.2f,\"kPa\":%.1f,\"kIa\":%.1f,\"kDa\":%.1f,\"ate\":%u}\n",
                d.boost, d.minBoost, d.maxBoost, d.rpm, d.maxRPM,
                d.speed, d.maxSpeed, d.tps, totalDistanceKm, currentBaseDuty, currentOutDuty, static_cast<int>(systemMode),
                g_dynamicTarget, pid.kPa, pid.kIa, pid.kDa, at.episodes
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
            // Snapshot the config under the lock, then write NVS OUTSIDE it. The 24 flash writes used
            // to hold configMutex the whole time, stalling the 20 Hz control loop (snapshotConfig()
            // needs the same mutex every cycle). Copying the POD structs is cheap; flash is slow.
            RuntimeConfig cfgSnap;
            PID_Config pidSnap;
            bool atEnabledSnap = false;
            bool haveSnap = false;
            if (takeMutex(configMutex, pdMS_TO_TICKS(80))) {
                cfgSnap = cfg;
                pidSnap = pid;
                atEnabledSnap = at.enabled;
                settingsNeedsSaving = false;
                forceSettingsSaveRequested = false;
                haveSnap = true;
                xSemaphoreGive(configMutex);
            }
            if (haveSnap) {
                prefs.putFloat("oP", cfgSnap.offsetPIM);
                prefs.putFloat("sP", cfgSnap.scalePIM);
                prefs.putFloat("oE", cfgSnap.offsetECU);
                prefs.putFloat("sE", cfgSnap.scaleECU);
                prefs.putFloat("sT", cfgSnap.spoolMinTps);
                prefs.putBool("pT", cfgSnap.pedalTargetScaling);
                prefs.putFloat("pR", cfgSnap.pulsesPerRev);
                prefs.putFloat("oV", cfgSnap.offsetVTA);
                prefs.putFloat("tB", cfgSnap.targetBoost);
                prefs.putFloat("kP", pidSnap.kP);
                prefs.putFloat("kI", pidSnap.kI);
                prefs.putFloat("kD", pidSnap.kD);
                prefs.putFloat("kPa", pidSnap.kPa);   // V2: auto-tuned gains, own keys
                prefs.putFloat("kIa", pidSnap.kIa);
                prefs.putFloat("kDa", pidSnap.kDa);
                prefs.putBool("aT", atEnabledSnap);
                prefs.putFloat("lA", pidSnap.learnCoeff);
                prefs.putFloat("vP", cfgSnap.vssPulsesPerRev);
                prefs.putInt("tW", cfgSnap.tireW);
                prefs.putInt("tA", cfgSnap.tireA);
                prefs.putInt("tR", cfgSnap.tireR);
                prefs.putFloat("lB", cfgSnap.limitBoostBar);
                prefs.putFloat("sL", cfgSnap.softLimpBar);
                prefs.putFloat("hL", cfgSnap.hardLimpBar);
                prefs.putFloat("bH", cfgSnap.spoolBlendHigh);
                prefs.putFloat("bL", cfgSnap.spoolBlendLow);
                prefs.putFloat("fS", cfgSnap.dutyFallSlewPerS);
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

    // Created before BLE init so onWrite() can enqueue from the very first client write.
    bleCmdQueue = xQueueCreate(8, sizeof(BleCommand));
    if (bleCmdQueue == nullptr) {
        Serial.println("Crit Error: BLE command queue creation failed!");
    }

    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_SEC * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&twdt_config);

    Serial.println("==== FIRMWARE V2: SELF-TUNING PID (auto kPa/kIa/kDa) ====");
    Serial.println("Step: NVS init...");
    if (!prefs.begin("yrv_v4", false)) {
        Serial.println("Crit Error: NVS Init failed!");
    }

    otaModeEnabled = prefs.getBool("ota_mode", false);

    if (takeMutex(configMutex, pdMS_TO_TICKS(100))) {
        cfg.offsetPIM = constrainFloat(prefs.getFloat("oP", DEFAULT_OFFSET_PIM), 0.1f, 4.5f);
        cfg.scalePIM = constrainFloat(prefs.getFloat("sP", DEFAULT_SCALE_PIM), 0.1f, 2.0f);
        cfg.offsetECU = constrainFloat(prefs.getFloat("oE", DEFAULT_OFFSET_ECU), 0.1f, 4.5f);
        cfg.scaleECU = constrainFloat(prefs.getFloat("sE", DEFAULT_SCALE_ECU), 0.1f, 2.0f);
        cfg.spoolMinTps = constrainFloat(prefs.getFloat("sT", SPOOL_MIN_TPS_DEF), 20.0f, 100.0f);
        cfg.pedalTargetScaling = prefs.getBool("pT", true);
        cfg.pulsesPerRev = constrainFloat(prefs.getFloat("pR", 2.0f), 0.5f, 8.0f);
        cfg.offsetVTA = constrainFloat(prefs.getFloat("oV", 0.42f), 0.1f, 3.5f);
        cfg.targetBoost = constrainFloat(prefs.getFloat("tB", 0.80f), 0.3f, 1.5f);
        cfg.vssPulsesPerRev = constrainFloat(prefs.getFloat("vP", 5.18f), 1.0f, 40.0f);
        cfg.tireW = constrain(prefs.getInt("tW", 195), 100, 300);
        cfg.tireA = constrain(prefs.getInt("tA", 55), 20, 100);
        cfg.tireR = constrain(prefs.getInt("tR", 15), 10, 24);
        cfg.limitBoostBar = constrainFloat(prefs.getFloat("lB", 0.95f), 0.3f, 2.0f);
        recomputeLimpThresholdsLocked();   // soft/hard limp are derived from target (+0.15 / +0.20), not loaded
        cfg.spoolBlendHigh = constrainFloat(prefs.getFloat("bH", SPOOL_BLEND_HIGH_DEF), 0.05f, 1.0f);
        cfg.spoolBlendLow  = constrainFloat(prefs.getFloat("bL", SPOOL_BLEND_LOW_DEF), 0.0f, 0.5f);
        cfg.dutyFallSlewPerS = constrainFloat(prefs.getFloat("fS", DUTY_FALL_SLEW_DEF), 50.0f, 1000.0f);
        calcWheelSizeLocked();
        pid.kP = constrainFloat(prefs.getFloat("kP", DEFAULT_KP), 0.0f, 200.0f);
        pid.kI = constrainFloat(prefs.getFloat("kI", DEFAULT_KI), 0.0f, 200.0f);
        pid.kD = constrainFloat(prefs.getFloat("kD", DEFAULT_KD), 0.0f, 50.0f);
        // V2: auto gains default to the manual gains (seeds them on a fresh V2 flash).
        pid.kPa = constrainFloat(prefs.getFloat("kPa", pid.kP), KP_MIN, KP_MAX);
        pid.kIa = constrainFloat(prefs.getFloat("kIa", pid.kI), KI_MIN, KI_MAX);
        pid.kDa = constrainFloat(prefs.getFloat("kDa", pid.kD), KD_MIN, KD_MAX);
        at.enabled = prefs.getBool("aT", AUTOTUNE_ENABLED_DEF);
        pid.learnCoeff = constrainFloat(prefs.getFloat("lA", DEFAULT_LEARN_RATE), LEARN_RATE_MIN, LEARN_RATE_MAX);

        if (prefs.getInt("tuneVer", 0) < 6) {
            pid.kP = DEFAULT_KP;
            pid.kI = DEFAULT_KI;
            pid.kD = DEFAULT_KD;
            pid.learnCoeff = DEFAULT_LEARN_RATE;
            cfg.offsetPIM = DEFAULT_OFFSET_PIM;
            cfg.scalePIM = DEFAULT_SCALE_PIM;
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

    // calibration are left untouched so any fine-tuning saved by the user survives.
        if (prefs.getInt("tuneVer", 0) < 7) {
            prefs.remove("map2D");
            prefs.remove("conf2D");
            prefs.remove("samples2D");
            prefs.putInt("tuneVer", 7);
        }

        // v8 migration: new base tune — calmer plateau PID (kP 28→20, kI 16→14, kD 10→8) and a softer
        // spool handover (bH→0.15, bL→0.03, fS→150). Force these once over stale NVS values so the new
        // tune is live after flashing without a manual reset; the app can still fine-tune afterwards and
        // the changes persist. The learned duty map is left intact (this only retunes gains/crossfade).
        if (prefs.getInt("tuneVer", 0) < 8) {
            pid.kP = DEFAULT_KP;
            pid.kI = DEFAULT_KI;
            pid.kD = DEFAULT_KD;
            cfg.spoolBlendHigh   = SPOOL_BLEND_HIGH_DEF;
            cfg.spoolBlendLow    = SPOOL_BLEND_LOW_DEF;
            cfg.dutyFallSlewPerS = DUTY_FALL_SLEW_DEF;
            prefs.putFloat("kP", pid.kP);
            prefs.putFloat("kI", pid.kI);
            prefs.putFloat("kD", pid.kD);
            prefs.putFloat("bH", cfg.spoolBlendHigh);
            prefs.putFloat("bL", cfg.spoolBlendLow);
            prefs.putFloat("fS", cfg.dutyFallSlewPerS);
            prefs.putInt("tuneVer", 8);
        }

        // v9 migration: 2-bar MAP sensor fitted (2026-07-29). Force the new INPUT characteristic and
        // the newly separated ECU OUTPUT characteristic over the stale single-pair NVS values — the
        // old 2.56/0.55 applied to the new sensor reads -0.56 bar at atmosphere, feeds the ECU 45 kPa
        // and the engine will not start. Also seeds the spool pedal gate. The learned duty map is
        // deliberately KEPT: it maps duty vs RPM/TPS, which the sensor swap does not change.
        if (prefs.getInt("tuneVer", 0) < 9) {
            cfg.offsetPIM   = DEFAULT_OFFSET_PIM;
            cfg.scalePIM    = DEFAULT_SCALE_PIM;
            cfg.offsetECU   = DEFAULT_OFFSET_ECU;
            cfg.scaleECU    = DEFAULT_SCALE_ECU;
            cfg.spoolMinTps = SPOOL_MIN_TPS_DEF;
            prefs.putFloat("oP", cfg.offsetPIM);
            prefs.putFloat("sP", cfg.scalePIM);
            prefs.putFloat("oE", cfg.offsetECU);
            prefs.putFloat("sE", cfg.scaleECU);
            prefs.putFloat("sT", cfg.spoolMinTps);
            prefs.putInt("tuneVer", 9);
        }

        // v10 migration: new spool handover (bH 0.15->0.30, bL 0.03->0.10, fS 150->400), derived
        // from the 2026-07-29 log — see SPOOL_BLEND_HIGH_DEF above for the measurements. Forced over
        // stale NVS so a normal (non-erasing) flash picks them up; everything else — calibrations,
        // odometer, engine hours, learned map — is left untouched. Still tunable from the app after.
        if (prefs.getInt("tuneVer", 0) < 10) {
            cfg.spoolBlendHigh   = SPOOL_BLEND_HIGH_DEF;
            cfg.spoolBlendLow    = SPOOL_BLEND_LOW_DEF;
            cfg.dutyFallSlewPerS = DUTY_FALL_SLEW_DEF;
            prefs.putFloat("bH", cfg.spoolBlendHigh);
            prefs.putFloat("bL", cfg.spoolBlendLow);
            prefs.putFloat("fS", cfg.dutyFallSlewPerS);
            prefs.putInt("tuneVer", 10);
        }

        // V2 first-boot: seed the auto gains (kPa/kIa/kDa) once from the manual V1 gains so a fresh
        // V2 flash starts exactly where V1 left off, then they self-tune from there. Separate version
        // key (tuneVerV2) so this never touches the V1 migration chain above.
        if (prefs.getInt("tuneVerV2", 0) < 1) {
            pid.kPa = constrainFloat(pid.kP, KP_MIN, KP_MAX);
            pid.kIa = constrainFloat(pid.kI, KI_MIN, KI_MAX);
            pid.kDa = constrainFloat(pid.kD, KD_MIN, KD_MAX);
            at.enabled = AUTOTUNE_ENABLED_DEF;
            prefs.putFloat("kPa", pid.kPa);
            prefs.putFloat("kIa", pid.kIa);
            prefs.putFloat("kDa", pid.kDa);
            prefs.putBool("aT", at.enabled);
            prefs.putInt("tuneVerV2", 1);
        }
        // Last-known-good rollback target starts at the loaded auto gains.
        at.lkgKp = pid.kPa; at.lkgKi = pid.kIa; at.lkgKd = pid.kDa;
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
    dac.begin(0x60);   // MCP4725 actual address: ADDR pad soldered to GND (A0=0 -> 0x60)

    // MAP zero is MEASURED AND REPORTED, never auto-applied. Auto-zeroing silently rewrote the
    // calibration from whatever pressure happened to be in the manifold at boot (mid-drive reboot,
    // altitude, weather) and made the ECU passthrough drift with it. The reading below is printed
    // so oP can be set deliberately from the app: with the engine off and the manifold at
    // atmosphere, type this voltage into "Ноль MAP (oP)" and save.
    Serial.println("Step: MAP atmosphere reading (diagnostic only, not applied)...");
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
        if (validSamples > 0) {
            float avgVolts = sumVolts / validSamples;
            // Absolute kPa straight from the 2-bar sensor datasheet, as a sanity check against the
            // barometer: a sane atmosphere is ~95-105 kPa. Far off means wrong sensor/wiring/ground.
            float absKpa = (avgVolts - 0.0964f) * 65.88f;
            Serial.printf("MAP atmosphere: %.3fV (=%.1f kPa abs) | active oP=%.3fV sP=%.4f -> %.2f bar\n",
                avgVolts, absKpa, cfg.offsetPIM, cfg.scalePIM,
                (avgVolts - cfg.offsetPIM) * cfg.scalePIM);
        } else {
            Serial.printf("MAP atmosphere: ADC did not answer, keeping oP=%.3fV\n", cfg.offsetPIM);
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
    NimBLEDevice::init("YRV_Boost_BLE");
    NimBLEDevice::setMTU(247);
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    NimBLEService *pService = pServer->createService(SERVICE_UUID);

    // NimBLE auto-creates the 0x2902 CCCD for a NOTIFY characteristic — no BLE2902 to add.
    pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, NIMBLE_PROPERTY::NOTIFY);

    NimBLECharacteristic *pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, NIMBLE_PROPERTY::WRITE);
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    // CRITICAL: the app finds the device by NAME (Kable Filter.Name → Android ScanFilter.setDeviceName),
    // and Android's offloaded name filter only inspects the PRIMARY advertisement, NOT the scan
    // response. So the name MUST live in the primary packet. A 128-bit service UUID cannot share the
    // 31-byte primary packet with the name — advertising it would push the name into the scan response,
    // leaving the device visible to generic scanners (active scan) but INVISIBLE to the app's name
    // filter. So advertise ONLY the name; the service is discovered after connecting (the app never
    // filters by service UUID). On NimBLE 1.4.x use setScanResponse(true) instead of enableScanResponse().
    pAdvertising->setName("YRV_Boost_BLE");
    pAdvertising->enableScanResponse(true);
    // Connection interval is negotiated actively in onConnect via updateConnParams().
    pAdvertising->start();

    Serial.println("Step: starting tasks...");
    xTaskCreatePinnedToCore(TaskSensors, "SENS", 4096, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(TaskKLineReader, "KLINE", 3072, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(TaskControl, "CTRL", 4096, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(TaskTelemetry, "TELEM", 4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(TaskOdometerAndStorage, "ODO_STOR", 4096, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(TaskBleCommands, "BLECMD", 4096, nullptr, 1, nullptr, 0);
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
