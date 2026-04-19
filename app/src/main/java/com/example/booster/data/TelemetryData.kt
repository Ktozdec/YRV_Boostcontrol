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

    @SerialName("tab") val tab: Int = 0,
    @SerialName("w0") val w0: Float = 0.0f,
    @SerialName("w1") val w1: Float = 0.0f,
    @SerialName("w2") val w2: Float = 0.0f,
    @SerialName("w3") val w3: Float = 0.0f,
    @SerialName("w4") val w4: Float = 0.0f,
    @SerialName("w5") val w5: Float = 0.0f,
    @SerialName("w6") val w6: Float = 0.0f,
    @SerialName("w7") val w7: Float = 0.0f,
    @SerialName("w8") val w8: Float = 0.0f,
    @SerialName("w9") val w9: Float = 0.0f,
    @SerialName("w10") val w10: Float = 0.0f,

    val lastAck: String? = null,
    val lastError: String? = null
)
