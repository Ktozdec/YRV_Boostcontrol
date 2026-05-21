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

private data class TripLogColumn(val header: String, val value: (TripLogRow) -> String)

private const val MAX_TRIP_LOG_ROWS = 50_000

class BoosterViewModel : ViewModel() {
    private val bleManager = BleManager()

    val connectionStatus = bleManager.connectionStatus
    val telemetry = bleManager.telemetry

    private val tripLogRows = ArrayDeque<TripLogRow>()
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

    // Single source of truth for CSV layout: header text and value extractor live together,
    // so columns and values can never drift out of order.
    private val tripLogColumns: List<TripLogColumn> = listOf(
        TripLogColumn("recorded_at_ms") { it.recordedAtMillis.toString() },
        TripLogColumn("session_ms") { it.sessionMillis.toString() },
        TripLogColumn("connection_status") { csvCell(it.connectionStatus) },
        TripLogColumn("boost") { formatFloat(it.telemetry.boost) },
        TripLogColumn("min_boost") { formatFloat(it.telemetry.minBoost) },
        TripLogColumn("max_boost") { formatFloat(it.telemetry.maxBoost) },
        TripLogColumn("rpm") { it.telemetry.rpm.toString() },
        TripLogColumn("max_rpm") { it.telemetry.maxRpm.toString() },
        TripLogColumn("speed") { it.telemetry.speed.toString() },
        TripLogColumn("max_speed") { it.telemetry.maxSpeed.toString() },
        TripLogColumn("tps") { it.telemetry.tps.toString() },
        TripLogColumn("total_distance") { formatFloat(it.telemetry.totalDistance) },
        TripLogColumn("base_duty") { formatFloat(it.telemetry.baseDuty) },
        TripLogColumn("current_duty") { formatFloat(it.telemetry.currentDuty) },
        TripLogColumn("mode") { it.telemetry.mode.toString() },
        TripLogColumn("pulses_rpm") { formatFloat(it.telemetry.pulsesRpm) },
        TripLogColumn("vss_pulses") { formatFloat(it.telemetry.vssPulses) },
        TripLogColumn("offset_map") { formatFloat(it.telemetry.offsetMap) },
        TripLogColumn("scale_map") { formatFloat(it.telemetry.scaleMap) },
        TripLogColumn("offset_tps") { formatFloat(it.telemetry.offsetTps) },
        TripLogColumn("target_boost") { formatFloat(it.telemetry.targetBoost) },
        TripLogColumn("limit_boost_bar") { formatFloat(it.telemetry.limitBoostBar) },
        TripLogColumn("kp") { formatFloat(it.telemetry.kP) },
        TripLogColumn("ki") { formatFloat(it.telemetry.kI) },
        TripLogColumn("kd") { formatFloat(it.telemetry.kD) },
        TripLogColumn("learn_coeff") { formatFloat(it.telemetry.learnCoeff) },
        TripLogColumn("tire_w") { it.telemetry.tireW.toString() },
        TripLogColumn("tire_a") { it.telemetry.tireA.toString() },
        TripLogColumn("tire_r") { it.telemetry.tireR.toString() },
        TripLogColumn("engine_hours") { formatFloat(it.telemetry.engineHours) },
        TripLogColumn("kline_bytes") { it.telemetry.klineBytes.toString() },
        TripLogColumn("kline_frames") { it.telemetry.klineFrames.toString() },
        TripLogColumn("kline_last_hex") { csvCell(it.telemetry.klineLastHex) },
        TripLogColumn("last_ack") { csvCell(it.telemetry.lastAck.orEmpty()) },
        TripLogColumn("last_error") { csvCell(it.telemetry.lastError.orEmpty()) }
    )

    fun exportTripLogCsv(): String = buildString {
        appendLine(tripLogColumns.joinToString(",") { it.header })
        tripLogRows.forEach { row ->
            appendLine(tripLogColumns.joinToString(",") { it.value(row) })
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

        tripLogRows.addLast(
            TripLogRow(
                recordedAtMillis = telemetryAt,
                sessionMillis = telemetryAt - logSessionStartedAt,
                connectionStatus = connectionStatus.value,
                telemetry = data
            )
        )
        while (tripLogRows.size > MAX_TRIP_LOG_ROWS) {
            tripLogRows.removeFirst()
        }
        lastLoggedTelemetryAt = telemetryAt
        _tripLogSize.value = tripLogRows.size
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
