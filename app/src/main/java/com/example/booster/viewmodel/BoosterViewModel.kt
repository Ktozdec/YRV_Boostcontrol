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
    val telemetry: TelemetryData
)

class BoosterViewModel : ViewModel() {
    private val bleManager = BleManager()

    val connectionStatus = bleManager.connectionStatus
    val telemetry = bleManager.telemetry

    private val tripLogRows = mutableListOf<TripLogRow>()
    private val _tripLogSize = MutableStateFlow(0)
    val tripLogSize = _tripLogSize.asStateFlow()
    private val _diagnosticLog = MutableStateFlow<List<String>>(emptyList())
    val diagnosticLog = _diagnosticLog.asStateFlow()

    private var lastLoggedTelemetryAt = 0L
    private var logSessionStartedAt = 0L
    private var lastKlineFrameCount = 0L
    private var lastKlineResponse = ""

    init {
        viewModelScope.launch {
            telemetry.collect { data ->
                appendDiagnosticLogIfNeeded(data)
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

    fun sendKLineHexCommand(hex: String) {
        val normalized = hex.trim()
        if (normalized.isBlank()) return
        appendDiagnosticLog("TX > $normalized")
        bleManager.sendCommand("KREQ:$normalized")
    }

    fun scanDiagnosticTroubleCodes() {
        appendDiagnosticLog("DTC scan requested. Capture the working Launch request first, then we will bind it here.")
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
            append(row.telemetry.klineBytes)
            append(',')
            append(row.telemetry.klineFrames)
            append(',')
            append(csvCell(row.telemetry.klineLastHex))
            append(',')
            append(csvCell(row.telemetry.lastAck.orEmpty()))
            append(',')
            append(csvCell(row.telemetry.lastError.orEmpty()))
            appendLine()
        }
    }

    override fun onCleared() {
        bleManager.release()
        super.onCleared()
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
            telemetry = data
        )
        lastLoggedTelemetryAt = telemetryAt
        _tripLogSize.value = tripLogRows.size
    }

    private fun buildHeader(): String = buildString {
        append("recorded_at_ms,session_ms,connection_status")
        append(",boost,min_boost,max_boost,rpm,max_rpm,speed,max_speed,tps,total_distance")
        append(",base_duty,current_duty,mode,pulses_rpm,vss_pulses,offset_map,scale_map,offset_tps")
        append(",target_boost,kp,ki,kd,learn_coeff,tire_w,tire_a,tire_r,engine_hours,kline_bytes,kline_frames,kline_last_hex,last_ack,last_error")
    }

    private fun appendDiagnosticLogIfNeeded(data: TelemetryData) {
        if (data.klineFrames != lastKlineFrameCount) {
            lastKlineFrameCount = data.klineFrames
            if (data.klineLastHex.isNotBlank()) {
                appendDiagnosticLog("BUS < ${data.klineLastHex}")
            }
        }

        if (data.klineResponseHex.isNotBlank() && data.klineResponseHex != lastKlineResponse) {
            lastKlineResponse = data.klineResponseHex
            appendDiagnosticLog("RX < ${data.klineResponseHex}")
        }
    }

    private fun appendDiagnosticLog(line: String) {
        val timestamp = SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(Date())
        _diagnosticLog.value = (_diagnosticLog.value + "[$timestamp] $line").takeLast(120)
    }

    private fun csvCell(value: String): String {
        val escaped = value.replace("\"", "\"\"")
        return "\"$escaped\""
    }

    private fun formatFloat(value: Float): String = String.format(Locale.US, "%.4f", value)
}
