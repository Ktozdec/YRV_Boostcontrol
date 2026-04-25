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
    @SerialName("kP") val kP: Float = 0.0f,
    @SerialName("kI") val kI: Float = 0.0f,
    @SerialName("kD") val kD: Float = 0.0f,
    @SerialName("lA") val learnCoeff: Float = 0.0f,
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
