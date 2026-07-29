package com.example.booster.data

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class TelemetryData(
    @SerialName("b") val boost: Float = 0.0f,
    @SerialName("miB") val minBoost: Float = 0.0f,
    @SerialName("maB") val maxBoost: Float = 0.0f,
    @SerialName("r") val rpm: Int = 0,
    @SerialName("maR") val maxRpm: Int = 0,
    @SerialName("s") val speed: Int = 0,
    @SerialName("maS") val maxSpeed: Int = 0,
    @SerialName("v") val tps: Int = 0,
    @SerialName("oD") val totalDistance: Float = 0.0f,
    @SerialName("bD") val baseDuty: Float = 0.0f,
    @SerialName("cD") val currentDuty: Float = 0.0f,
    @SerialName("mode") val mode: Int = 0,
    // Momentary target the PID is chasing right now (user target x RPM shape x TPS scale, low-passed).
    // Distinct from [targetBoost], which is the static setting from the "S" packet.
    @SerialName("tg") val dynamicTarget: Float = 0.0f,

    @SerialName("pR") val pulsesRpm: Float = 0.0f,
    @SerialName("vP") val vssPulses: Float = 0.0f,
    // INPUT characteristic — the 2-bar MAP sensor the controller reads and regulates from.
    @SerialName("oP") val offsetMap: Float = 0.0f,
    @SerialName("sP") val scaleMap: Float = 0.0f,
    // OUTPUT characteristic — the STOCK sensor curve synthesized for the engine ECU. Kept separate
    // from the input pair: the ECU still expects the original sensor's volts-per-bar.
    @SerialName("oE") val offsetEcu: Float = 2.56f,
    @SerialName("sE") val scaleEcu: Float = 0.55f,
    @SerialName("sT") val spoolMinTps: Float = 50.0f,
    // 1 = target scales with pedal, 0 = flat full target regardless of pedal (AVC-R style).
    @SerialName("pT") val pedalTargetScaling: Int = 1,
    @SerialName("oV") val offsetTps: Float = 0.0f,
    @SerialName("tB") val targetBoost: Float = 0.0f,
    @SerialName("lB") val limitBoostBar: Float = 0.95f,
    // Derived by the ECU from the target (+0.15 / +0.20); these are only pre-connect placeholders.
    @SerialName("sL") val softLimpBar: Float = 0.95f,
    @SerialName("hL") val hardLimpBar: Float = 1.00f,
    @SerialName("kP") val kP: Float = 0.0f,
    @SerialName("kI") val kI: Float = 0.0f,
    @SerialName("kD") val kD: Float = 0.0f,
    // V2 self-tuning PID: live auto-tuned gains (from the "T" packet) + master switch + episode count.
    @SerialName("kPa") val autoKp: Float = 0.0f,
    @SerialName("kIa") val autoKi: Float = 0.0f,
    @SerialName("kDa") val autoKd: Float = 0.0f,
    @SerialName("aT") val autoTuneEnabled: Int = 1,
    @SerialName("ate") val autoTuneEpisodes: Int = 0,
    @SerialName("lA") val learnCoeff: Float = 0.0f,
    @SerialName("bH") val spoolBlendHigh: Float = 0.30f,
    @SerialName("bL") val spoolBlendLow: Float = 0.10f,
    @SerialName("fS") val dutyFallSlew: Float = 400.0f,
    @SerialName("tW") val tireW: Int = 0,
    @SerialName("tA") val tireA: Int = 0,
    @SerialName("tR") val tireR: Int = 0,
    @SerialName("eH") val engineHours: Float = 0.0f,

    @SerialName("kb") val klineBytes: Long = 0L,
    @SerialName("kf") val klineFrames: Long = 0L,
    @SerialName("kl") val klineLastLen: Int = 0,
    @SerialName("ko") val klineOverflows: Long = 0L,
    @SerialName("kh") val klineLastHex: String = "",
    @SerialName("rx") val klineResponseHex: String = "",
    @SerialName("rxl") val klineResponseLen: Int = 0,
    @SerialName("ov") val klineResponseOverflow: Int = 0,

    val lastAck: String? = null,
    val lastError: String? = null,
    val telemetryUpdatedAtMillis: Long = 0L
)

/**
 * The slice of [TelemetryData] the Settings screen actually edits. It only carries fields that come
 * from the rare "S" settings packet (plus [lastError] for the ECU-error toast). Exposed as its own
 * flow so the Settings screen recomposes when a *setting* changes — not 8×/second under the live
 * "T" telemetry stream. Equality is value-based (data class), so a no-op re-send doesn't re-emit.
 */
data class SettingsUiState(
    val targetBoost: Float = 0.0f,
    val limitBoostBar: Float = 0.95f,
    val softLimpBar: Float = 0.95f,
    val hardLimpBar: Float = 1.00f,
    val kP: Float = 0.0f,
    val kI: Float = 0.0f,
    val kD: Float = 0.0f,
    val learnCoeff: Float = 0.0f,
    val spoolBlendHigh: Float = 0.30f,
    val spoolBlendLow: Float = 0.10f,
    val dutyFallSlew: Float = 400.0f,
    val offsetMap: Float = 0.0f,
    val scaleMap: Float = 0.0f,
    val offsetEcu: Float = 2.56f,
    val scaleEcu: Float = 0.55f,
    val spoolMinTps: Float = 50.0f,
    val pedalTargetScaling: Int = 1,
    val offsetTps: Float = 0.0f,
    val pulsesRpm: Float = 0.0f,
    val vssPulses: Float = 0.0f,
    val autoTuneEnabled: Int = 1,
    val lastError: String? = null
)

fun TelemetryData.toSettingsUiState(): SettingsUiState = SettingsUiState(
    targetBoost = targetBoost,
    limitBoostBar = limitBoostBar,
    softLimpBar = softLimpBar,
    hardLimpBar = hardLimpBar,
    kP = kP,
    kI = kI,
    kD = kD,
    learnCoeff = learnCoeff,
    spoolBlendHigh = spoolBlendHigh,
    spoolBlendLow = spoolBlendLow,
    dutyFallSlew = dutyFallSlew,
    offsetMap = offsetMap,
    scaleMap = scaleMap,
    offsetEcu = offsetEcu,
    scaleEcu = scaleEcu,
    spoolMinTps = spoolMinTps,
    pedalTargetScaling = pedalTargetScaling,
    offsetTps = offsetTps,
    pulsesRpm = pulsesRpm,
    vssPulses = vssPulses,
    autoTuneEnabled = autoTuneEnabled,
    lastError = lastError
)

/** One ECU command acknowledgement (the "ack" packet): which command, success, optional reason. */
data class AckResult(
    val cmd: String,
    val ok: Boolean,
    val error: String?
)
