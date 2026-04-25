package com.example.booster.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.example.booster.ble.BleManager
import com.example.booster.data.TelemetryData
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private data class TripLogRow(
    val recordedAtMillis: Long,
    val sessionMillis: Long,
    val connectionStatus: String,
    val telemetry: TelemetryData,
    val mapSnapshot: Array<FloatArray>
)

class BoosterViewModel : ViewModel() {
    private val bleManager = BleManager()

    val connectionStatus = bleManager.connectionStatus
    val telemetry = bleManager.telemetry

    private val mapCache = Array(4) { FloatArray(11) }
    private val tripLogRows = mutableListOf<TripLogRow>()
    private val _tripLogSize = MutableStateFlow(0)
    val tripLogSize = _tripLogSize.asStateFlow()

    private var lastLoggedTelemetryAt = 0L
    private var logSessionStartedAt = 0L

    init {
        viewModelScope.launch {
            telemetry.collect { data ->
                updateMapCache(data)
                appendTripLogRowIfNeeded(data)
            }
        }
    }

    fun connect() {
        bleManager.connect()
    }

    fun disconnect() {
        bleManager.disconnect()
    }

    fun sendCommand(cmd: String) {
        bleManager.sendCommand(cmd)
    }

    fun defaultTripLogFileName(): String {
        val formatter = SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US)
        return "booster_trip_log_${formatter.format(Date())}.csv"
    }

    fun exportTripLogCsv(): String = buildString {
        appendLine(buildHeader())
        tripLogRows.forEach { row ->
            append(row.recordedAtMillis)
            append(',')
            append(row.sessionMillis)
            append(',')
            append(csvCell(row.connectionStatus))
            append(',')
            append(formatFloat(row.telemetry.boost))
            append(',')
            append(formatFloat(row.telemetry.minBoost))
            append(',')
            append(formatFloat(row.telemetry.maxBoost))
            append(',')
            append(row.telemetry.rpm)
            append(',')
            append(row.telemetry.maxRpm)
            append(',')
            append(row.telemetry.speed)
            append(',')
            append(row.telemetry.maxSpeed)
            append(',')
            append(row.telemetry.tps)
            append(',')
            append(formatFloat(row.telemetry.totalDistance))
            append(',')
            append(formatFloat(row.telemetry.baseDuty))
            append(',')
            append(formatFloat(row.telemetry.currentDuty))
            append(',')
            append(row.telemetry.mode)
            append(',')
            append(formatFloat(row.telemetry.pulsesRpm))
            append(',')
            append(formatFloat(row.telemetry.vssPulses))
            append(',')
            append(formatFloat(row.telemetry.offsetMap))
            append(',')
            append(formatFloat(row.telemetry.scaleMap))
            append(',')
            append(formatFloat(row.telemetry.offsetTps))
            append(',')
            append(formatFloat(row.telemetry.targetBoost))
            append(',')
            append(formatFloat(row.telemetry.kP))
            append(',')
            append(formatFloat(row.telemetry.kI))
            append(',')
            append(formatFloat(row.telemetry.kD))
            append(',')
            append(formatFloat(row.telemetry.learnCoeff))
            append(',')
            append(row.telemetry.tireW)
            append(',')
            append(row.telemetry.tireA)
            append(',')
            append(row.telemetry.tireR)
            append(',')
            append(formatFloat(row.telemetry.engineHours))
            append(',')
            append(row.telemetry.tab)
            append(',')
            append(csvCell(row.telemetry.lastAck.orEmpty()))
            append(',')
            append(csvCell(row.telemetry.lastError.orEmpty()))

            row.mapSnapshot.forEach { mapRow ->
                mapRow.forEach { value ->
                    append(',')
                    append(formatFloat(value))
                }
            }
            appendLine()
        }
    }

    override fun onCleared() {
        bleManager.release()
        super.onCleared()
    }

    private fun updateMapCache(data: TelemetryData) {
        val tab = data.tab
        if (tab !in 0..3) return

        val values = floatArrayOf(
            data.w0, data.w1, data.w2, data.w3, data.w4, data.w5,
            data.w6, data.w7, data.w8, data.w9, data.w10
        )
        values.forEachIndexed { index, value ->
            mapCache[tab][index] = value
        }
    }

    private fun appendTripLogRowIfNeeded(data: TelemetryData) {
        val telemetryAt = data.telemetryUpdatedAtMillis
        if (telemetryAt == 0L || telemetryAt == lastLoggedTelemetryAt) return

        if (logSessionStartedAt == 0L) {
            logSessionStartedAt = telemetryAt
        }

        tripLogRows += TripLogRow(
            recordedAtMillis = telemetryAt,
            sessionMillis = telemetryAt - logSessionStartedAt,
            connectionStatus = connectionStatus.value,
            telemetry = data,
            mapSnapshot = Array(4) { tab -> mapCache[tab].copyOf() }
        )
        lastLoggedTelemetryAt = telemetryAt
        _tripLogSize.value = tripLogRows.size
    }

    private fun buildHeader(): String = buildString {
        append("recorded_at_ms,session_ms,connection_status")
        append(",boost,min_boost,max_boost,rpm,max_rpm,speed,max_speed,tps,total_distance")
        append(",base_duty,current_duty,mode,pulses_rpm,vss_pulses,offset_map,scale_map,offset_tps")
        append(",target_boost,kp,ki,kd,learn_coeff,tire_w,tire_a,tire_r,engine_hours,active_tab,last_ack,last_error")
        repeat(4) { tab ->
            repeat(11) { index ->
                append(",map_${tab}_$index")
            }
        }
    }

    private fun csvCell(value: String): String {
        val escaped = value.replace("\"", "\"\"")
        return "\"$escaped\""
    }

    private fun formatFloat(value: Float): String = String.format(Locale.US, "%.4f", value)
}
