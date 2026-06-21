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

    @SerialName("pR") val pulsesRpm: Float = 0.0f,
    @SerialName("vP") val vssPulses: Float = 0.0f,
    @SerialName("oP") val offsetMap: Float = 0.0f,
    @SerialName("sP") val scaleMap: Float = 0.0f,
    @SerialName("oV") val offsetTps: Float = 0.0f,
    @SerialName("tB") val targetBoost: Float = 0.0f,
    @SerialName("lB") val limitBoostBar: Float = 0.95f,
    @SerialName("sL") val softLimpBar: Float = 1.15f,
    @SerialName("hL") val hardLimpBar: Float = 1.30f,
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
    @SerialName("bL") val spoolBlendLow: Float = 0.05f,
    @SerialName("fS") val dutyFallSlew: Float = 300.0f,
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
    val softLimpBar: Float = 1.15f,
    val hardLimpBar: Float = 1.30f,
    val kP: Float = 0.0f,
    val kI: Float = 0.0f,
    val kD: Float = 0.0f,
    val learnCoeff: Float = 0.0f,
    val spoolBlendHigh: Float = 0.30f,
    val spoolBlendLow: Float = 0.05f,
    val dutyFallSlew: Float = 300.0f,
    val offsetMap: Float = 0.0f,
    val scaleMap: Float = 0.0f,
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
