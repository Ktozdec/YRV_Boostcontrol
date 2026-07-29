package com.example.booster.ble

import android.util.Log
import com.example.booster.data.AckResult
import com.example.booster.data.SettingsUiState
import com.example.booster.data.TelemetryData
import com.example.booster.data.toSettingsUiState
import com.juul.kable.AndroidPeripheral
import com.juul.kable.Peripheral
import com.juul.kable.Scanner
import com.juul.kable.State
import com.juul.kable.WriteType
import com.juul.kable.characteristicOf
import com.juul.kable.peripheral
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject

class BleManager {
    private val serviceUuid = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
    private val rxCharacteristic = characteristicOf(serviceUuid, "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    private val txCharacteristic = characteristicOf(serviceUuid, "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

    // No hardware (offloaded) name filter: Android's offloaded ScanFilter.setDeviceName matches only
    // the PRIMARY advertisement, so it silently misses a device whose name is carried in the scan
    // response. Scan unfiltered and match the name in software (below) — robust to where it's advertised.
    private val scanner = Scanner { }
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private val writeMutex = Mutex()

    private var peripheral: Peripheral? = null
    private var isIntendedToConnect = false
    private var connectionJob: Job? = null
    private var telemetryJob: Job? = null

    private val _connectionStatus = MutableStateFlow("Отключено")
    val connectionStatus = _connectionStatus.asStateFlow()

    private val _telemetry = MutableStateFlow(TelemetryData())
    val telemetry = _telemetry.asStateFlow()

    // Settings ride the same TelemetryData as the 8 Hz "T" stream but change only on an "S" packet.
    // This derived flow re-emits only when a settings field actually changes (value-based equality),
    // so the Settings screen isn't recomposed 8×/second by the live telemetry.
    val settings: StateFlow<SettingsUiState> = _telemetry
        .map { it.toSettingsUiState() }
        .distinctUntilChanged()
        // WhileSubscribed: stop mapping the 8 Hz stream when no screen is observing settings.
        .stateIn(scope, SharingStarted.WhileSubscribed(5000), SettingsUiState())

    // Negotiated ATT MTU (23 until the exchange succeeds). One characteristic write carries at most
    // MTU-3 bytes, so the batch "SETALL" save is only safe once a large MTU is negotiated; below that
    // the Settings screen falls back to short per-key SET commands.
    private val _mtu = MutableStateFlow(23)
    val mtu = _mtu.asStateFlow()

    // One-shot command acknowledgements from the ECU (the "ack" packet). A SharedFlow (events, not
    // state) so the save flow can await the specific SAVE ack without matching a stale value.
    private val _ackEvents = MutableSharedFlow<AckResult>(extraBufferCapacity = 16)
    val ackEvents: SharedFlow<AckResult> = _ackEvents

    fun connect() {
        if (isIntendedToConnect) return
        isIntendedToConnect = true

        connectionJob = scope.launch {
            while (isIntendedToConnect) {
                try {
                    _connectionStatus.value = "Поиск устройства..."
                    val advertisement = scanner.advertisements
                        .filter { it.name == "YRV_Boost_BLE" }
                        .first()

                    // Diagnostic: if this line never appears in logcat (tag "BLE"), the SCAN stage is
                    // failing (permissions/неверный neverForLocation), not the connect stage.
                    Log.i("BLE", "Найдено в скане: name=${advertisement.name}")
                    _connectionStatus.value = "Найдено, подключаемся..."
                    val newPeripheral = scope.peripheral(advertisement)
                    peripheral = newPeripheral

                    val stateJob = newPeripheral.state.onEach { state ->
                        _connectionStatus.value = when (state) {
                            is State.Connecting -> "Подключение..."
                            is State.Connected -> "Подключено"
                            is State.Disconnecting -> "Отключение..."
                            is State.Disconnected -> if (isIntendedToConnect) "Обрыв, переподключение..." else "Отключено"
                        }
                    }.launchIn(this)

                    newPeripheral.connect()
                    // Short settle, then ask for the big MTU right away so the settings burst that
                    // the firmware sends on connect lands at full MTU (fewer chunks, fewer drops).
                    delay(150)

                    _mtu.value = 23
                    try {
                        val negotiated = (newPeripheral as? AndroidPeripheral)?.requestMtu(247)
                        if (negotiated != null) _mtu.value = negotiated
                    } catch (e: Exception) {
                        Log.w("BLE", "MTU не расширен: ${e.message}")
                    }

                    startObservingTelemetry(newPeripheral)
                    sendCommand("GET:SETTINGS")

                    newPeripheral.state.filter { it is State.Disconnected }.first()
                    stateJob.cancel()
                } catch (e: Exception) {
                    Log.e("BLE", "Ошибка подключения", e)
                    delay(2000)
                }
            }
            _connectionStatus.value = "Отключено"
        }
    }

    private fun startObservingTelemetry(targetPeripheral: Peripheral) {
        val jsonBuffer = StringBuilder()
        telemetryJob?.cancel()

        telemetryJob = targetPeripheral.observe(txCharacteristic)
            .onEach { data ->
                val chunk = data.decodeToString()
                jsonBuffer.append(chunk)

                var newlineIndex = jsonBuffer.indexOf("\n")
                while (newlineIndex != -1) {
                    val completeJsonStr = jsonBuffer.substring(0, newlineIndex).trim()
                    jsonBuffer.delete(0, newlineIndex + 1)

                    if (completeJsonStr.startsWith("{") && completeJsonStr.endsWith("}")) {
                        try {
                            val json = JSONObject(completeJsonStr)
                            val current = _telemetry.value

                            _telemetry.value = when {
                                json.has("T") -> current.copy(
                                    boost = json.optDouble("b", current.boost.toDouble()).toFloat(),
                                    rpm = json.optInt("r", current.rpm),
                                    speed = json.optInt("s", current.speed),
                                    minBoost = json.optDouble("miB", current.minBoost.toDouble()).toFloat(),
                                    maxBoost = json.optDouble("maB", current.maxBoost.toDouble()).toFloat(),
                                    maxRpm = json.optInt("maR", current.maxRpm),
                                    maxSpeed = json.optInt("maS", current.maxSpeed),
                                    tps = json.optInt("v", current.tps),
                                    totalDistance = json.optDouble("oD", current.totalDistance.toDouble()).toFloat(),
                                    baseDuty = json.optDouble("bD", current.baseDuty.toDouble()).toFloat(),
                                    currentDuty = json.optDouble("cD", current.currentDuty.toDouble()).toFloat(),
                                    mode = json.optInt("mode", current.mode),
                                    dynamicTarget = json.optDouble("tg", current.dynamicTarget.toDouble()).toFloat(),
                                    autoKp = json.optDouble("kPa", current.autoKp.toDouble()).toFloat(),
                                    autoKi = json.optDouble("kIa", current.autoKi.toDouble()).toFloat(),
                                    autoKd = json.optDouble("kDa", current.autoKd.toDouble()).toFloat(),
                                    autoTuneEpisodes = json.optInt("ate", current.autoTuneEpisodes),
                                    lastError = null,
                                    telemetryUpdatedAtMillis = System.currentTimeMillis()
                                )

                                json.has("S") -> current.copy(
                                    offsetMap = json.optDouble("oP", current.offsetMap.toDouble()).toFloat(),
                                    scaleMap = json.optDouble("sP", current.scaleMap.toDouble()).toFloat(),
                                    offsetEcu = json.optDouble("oE", current.offsetEcu.toDouble()).toFloat(),
                                    scaleEcu = json.optDouble("sE", current.scaleEcu.toDouble()).toFloat(),
                                    spoolMinTps = json.optDouble("sT", current.spoolMinTps.toDouble()).toFloat(),
                                    pedalTargetScaling = json.optInt("pT", current.pedalTargetScaling),
                                    offsetTps = json.optDouble("oV", current.offsetTps.toDouble()).toFloat(),
                                    targetBoost = json.optDouble("tB", current.targetBoost.toDouble()).toFloat(),
                                    limitBoostBar = json.optDouble("lB", current.limitBoostBar.toDouble()).toFloat(),
                                    softLimpBar = json.optDouble("sL", current.softLimpBar.toDouble()).toFloat(),
                                    hardLimpBar = json.optDouble("hL", current.hardLimpBar.toDouble()).toFloat(),
                                    kP = json.optDouble("kP", current.kP.toDouble()).toFloat(),
                                    kI = json.optDouble("kI", current.kI.toDouble()).toFloat(),
                                    kD = json.optDouble("kD", current.kD.toDouble()).toFloat(),
                                    learnCoeff = json.optDouble("lA", current.learnCoeff.toDouble()).toFloat(),
                                    spoolBlendHigh = json.optDouble("bH", current.spoolBlendHigh.toDouble()).toFloat(),
                                    spoolBlendLow = json.optDouble("bL", current.spoolBlendLow.toDouble()).toFloat(),
                                    dutyFallSlew = json.optDouble("fS", current.dutyFallSlew.toDouble()).toFloat(),
                                    autoTuneEnabled = json.optInt("aT", current.autoTuneEnabled),
                                    pulsesRpm = json.optDouble("pR", current.pulsesRpm.toDouble()).toFloat(),
                                    vssPulses = json.optDouble("vP", current.vssPulses.toDouble()).toFloat(),
                                    tireW = json.optInt("tW", current.tireW),
                                    tireA = json.optInt("tA", current.tireA),
                                    tireR = json.optInt("tR", current.tireR),
                                    engineHours = json.optDouble("eH", current.engineHours.toDouble()).toFloat(),
                                    lastError = null
                                )

                                json.has("K") -> current.copy(
                                    klineBytes = json.optLong("kb", current.klineBytes),
                                    klineFrames = json.optLong("kf", current.klineFrames),
                                    klineLastLen = json.optInt("kl", current.klineLastLen),
                                    klineOverflows = json.optLong("ko", current.klineOverflows),
                                    klineLastHex = json.optString("kh", current.klineLastHex),
                                    lastError = null
                                )

                                json.has("KRES") -> current.copy(
                                    klineResponseHex = json.optString("rx", current.klineResponseHex),
                                    klineResponseLen = json.optInt("rxl", current.klineResponseLen),
                                    klineResponseOverflow = json.optInt("ov", current.klineResponseOverflow),
                                    lastError = if (json.optBoolean("ok", false)) null else "kline_timeout"
                                )

                                json.has("ack") -> {
                                    val ack = json.optString("ack").takeIf { it.isNotBlank() }
                                    val ok = json.optBoolean("ok", true)
                                    val error = json.optString("err").takeIf { it.isNotBlank() }
                                    _ackEvents.tryEmit(AckResult(ack ?: "", ok, error))
                                    if (ok) {
                                        Log.d("BLE_ACK", "Подтверждено: $ack")
                                        current.copy(lastAck = ack, lastError = null)
                                    } else {
                                        Log.w("BLE_ACK", "Команда отклонена: $ack, err=$error")
                                        current.copy(lastAck = ack, lastError = error ?: "protocol_error")
                                    }
                                }

                                else -> current
                            }
                        } catch (e: Exception) {
                            Log.e("BLE", "JSON error: ${e.message} on str: $completeJsonStr")
                        }
                    }

                    newlineIndex = jsonBuffer.indexOf("\n")
                }

                if (jsonBuffer.length > 2048) {
                    jsonBuffer.clear()
                }
            }
            .catch { e -> Log.e("BLE", "Ошибка чтения", e) }
            .launchIn(scope)
    }

    fun sendCommand(command: String) = sendCommands(listOf(command))

    /**
     * Writes [commands] in order under a single lock hold, so the dispatcher can't reorder them —
     * e.g. "SAVE" must never run before the settings it is meant to persist. Stops the sequence on
     * the first failed write, so a failed setting is not followed by a SAVE of a half-applied state.
     */
    fun sendCommands(commands: List<String>) {
        if (commands.isEmpty()) return

        scope.launch {
            writeMutex.withLock {
                val currentPeripheral = peripheral
                if (currentPeripheral == null) {
                    Log.w("BLE", "Команды пропущены, соединение отсутствует: $commands")
                    return@withLock
                }

                for (command in commands) {
                    if (command.isBlank()) continue
                    try {
                        currentPeripheral.write(rxCharacteristic, command.toByteArray(), WriteType.WithResponse)
                        Log.d("BLE", "Отправлено: $command")
                    } catch (e: Exception) {
                        Log.e("BLE", "Ошибка отправки команды: $command", e)
                        break
                    }
                }
            }
        }
    }

    fun disconnect() {
        isIntendedToConnect = false
        scope.launch {
            connectionJob?.cancel()
            telemetryJob?.cancel()
            peripheral?.disconnect()
            peripheral = null
            _connectionStatus.value = "Отключено"
        }
    }

    fun release() {
        isIntendedToConnect = false
        connectionJob?.cancel()
        telemetryJob?.cancel()
        scope.launch {
            try {
                peripheral?.disconnect()
            } catch (e: Exception) {
                Log.w("BLE", "Ошибка при освобождении: ${e.message}")
            } finally {
                peripheral = null
                _connectionStatus.value = "Отключено"
                scope.cancel()
            }
        }
    }
}
