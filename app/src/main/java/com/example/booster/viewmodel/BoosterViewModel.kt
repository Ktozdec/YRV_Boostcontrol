package com.example.booster.viewmodel

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.example.booster.ble.BleManager
import com.example.booster.data.TelemetryData
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.io.BufferedWriter
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStream
import java.io.OutputStreamWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.max
import kotlin.math.min

private data class TripLogRow(
    val recordedAtMillis: Long,
    val telemetry: TelemetryData
)

private data class TripLogColumn(val header: String, val value: (TripLogRow) -> String)

// Matches DAC_REFERENCE_VOLTAGE in boost_controller_refined.ino; caps the reconstructed МАП аут voltage.
private const val DAC_REFERENCE_VOLTAGE = 5.02f

class BoosterViewModel(application: Application) : AndroidViewModel(application) {
    private val bleManager = BleManager()

    val connectionStatus = bleManager.connectionStatus
    val telemetry = bleManager.telemetry
    val settingsState = bleManager.settings
    val mtu = bleManager.mtu
    val ackEvents = bleManager.ackEvents

    private val _tripLogSize = MutableStateFlow(0)
    val tripLogSize = _tripLogSize.asStateFlow()
    private val _diagnosticLog = MutableStateFlow<List<String>>(emptyList())
    val diagnosticLog = _diagnosticLog.asStateFlow()

    private var lastLoggedTelemetryAt = 0L
    private var lastKlineFrameCount = 0L
    private var lastKlineResponse = ""

    // Used only while holding logLock, so a single shared (non-thread-safe) instance is fine.
    private val timeFormat = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    // Streaming trip log (A1): rows are written straight to a file on disk instead of being
    // accumulated in a List in RAM, so a multi-hour drive can't trigger an OutOfMemoryError,
    // and export streams the file rather than building one giant String.
    private val logFile = File(application.cacheDir, "trip_log_current.csv")
    private val logLock = Any()
    private var logWriter: BufferedWriter? = null
    private var headerWritten = false
    private var logRowCount = 0
    private var rowsSinceFlush = 0

    init {
        synchronized(logLock) {
            // Explicit UTF-8: the CSV has Cyrillic headers/units, and FileWriter's platform
            // default charset would otherwise garble them. A BOM is added with the header below.
            runCatching {
                logWriter = BufferedWriter(OutputStreamWriter(FileOutputStream(logFile, false), Charsets.UTF_8))
            }  // fresh file per app session
            headerWritten = false
            logRowCount = 0
            rowsSinceFlush = 0
        }
        viewModelScope.launch(Dispatchers.IO) {
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

    fun sendCommands(cmds: List<String>) {
        bleManager.sendCommands(cmds)
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
    // so columns and values can never drift out of order. Each sensor column shows the
    // engineering value together with the reconstructed sensor voltage, e.g. "1.00бар/4.38В",
    // so the log is readable without cross-referencing calibration coefficients.
    private val tripLogColumns: List<TripLogColumn> = listOf(
        TripLogColumn("Время") { timeFormat.format(Date(it.recordedAtMillis)) },
        TripLogColumn("МАП ин") { barVoltsCell(it.telemetry.boost, mapInVolts(it.telemetry)) },
        TripLogColumn("МАП аут") { barVoltsCell(mapOutBar(it.telemetry), mapOutVolts(it.telemetry)) },
        TripLogColumn("Педаль") { "${it.telemetry.tps}%/${fmt2(tpsVolts(it.telemetry))}В" },
        TripLogColumn("Обороты") { "${it.telemetry.rpm}об/мин" },
        TripLogColumn("Скорость") { "${it.telemetry.speed}км/ч" },
        TripLogColumn("Дьюти карты") { "${fmt1(it.telemetry.baseDuty)}%" },
        TripLogColumn("Дьюти коррекция") { "${fmt1(it.telemetry.currentDuty)}%" }
    )

    // Streams the on-disk log to the chosen destination (no giant in-memory String).
    // Call off the main thread — copies the file in chunks.
    fun copyTripLogTo(out: OutputStream) {
        synchronized(logLock) {
            runCatching { logWriter?.flush() }
            rowsSinceFlush = 0
            if (logFile.exists()) {
                logFile.inputStream().use { it.copyTo(out) }
            }
        }
    }

    override fun onCleared() {
        synchronized(logLock) {
            runCatching {
                logWriter?.flush()
                logWriter?.close()
            }
            logWriter = null
        }
        bleManager.release()
        super.onCleared()
    }

    private fun appendTripLogRowIfNeeded(data: TelemetryData) {
        val telemetryAt = data.telemetryUpdatedAtMillis
        if (telemetryAt == 0L || telemetryAt == lastLoggedTelemetryAt) return

        val row = TripLogRow(recordedAtMillis = telemetryAt, telemetry = data)

        synchronized(logLock) {
            val writer = logWriter ?: return
            runCatching {
                if (!headerWritten) {
                    writer.write("﻿")   // UTF-8 BOM (U+FEFF) so Excel opens the Cyrillic CSV as UTF-8 instead of ANSI
                    writer.write(tripLogColumns.joinToString(",") { it.header })
                    writer.newLine()
                    headerWritten = true
                }
                writer.write(tripLogColumns.joinToString(",") { it.value(row) })
                writer.newLine()
                logRowCount++
                if (++rowsSinceFlush >= 100) {   // bound data loss without flushing every line
                    writer.flush()
                    rowsSinceFlush = 0
                }
            }
        }

        lastLoggedTelemetryAt = telemetryAt
        _tripLogSize.value = logRowCount
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

    // Inverse of the firmware sensor maths (boost_controller_refined.ino): the app receives the
    // engineering value plus the live calibration coefficients, so the original sensor voltage can
    // be reconstructed exactly. scaleMap is guarded against a not-yet-received 0 to avoid NaN.
    private fun mapScale(t: TelemetryData): Float = if (t.scaleMap > 0.0001f) t.scaleMap else 0.55f

    private fun mapInVolts(t: TelemetryData): Float = t.boost / mapScale(t) + t.offsetMap

    private fun mapOutBar(t: TelemetryData): Float = min(t.boost, t.limitBoostBar)

    private fun mapOutVolts(t: TelemetryData): Float =
        (mapOutBar(t) / mapScale(t) + t.offsetMap).coerceIn(0.0f, DAC_REFERENCE_VOLTAGE)

    private fun tpsVolts(t: TelemetryData): Float {
        val span = max(0.3f, 3.71f - t.offsetTps)
        return t.tps / 100.0f * span + t.offsetTps
    }

    private fun barVoltsCell(bar: Float, volts: Float): String = "${fmt2(bar)}бар/${fmt2(volts)}В"

    private fun fmt2(value: Float): String = String.format(Locale.US, "%.2f", value)

    private fun fmt1(value: Float): String = String.format(Locale.US, "%.1f", value)
}
