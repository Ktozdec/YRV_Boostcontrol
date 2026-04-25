package com.example.booster.ble

import android.util.Log
import com.example.booster.data.TelemetryData
import com.juul.kable.AndroidPeripheral
import com.juul.kable.Filter
import com.juul.kable.Peripheral
import com.juul.kable.Scanner
import com.juul.kable.State
import com.juul.kable.WriteType
import com.juul.kable.characteristicOf
import com.juul.kable.peripheral
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject

class BleManager {
    private val serviceUuid = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
    private val rxCharacteristic = characteristicOf(serviceUuid, "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    private val txCharacteristic = characteristicOf(serviceUuid, "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

    private val scanner = Scanner { filters = listOf(Filter.Name("YRV_Boost_BLE")) }
    private val scope = CoroutineScope(Dispatchers.IO + Job())
    private val writeMutex = Mutex()

    private var peripheral: Peripheral? = null
    private var isIntendedToConnect = false
    private var connectionJob: Job? = null
    private var telemetryJob: Job? = null

    private val _connectionStatus = MutableStateFlow("Отключено")
    val connectionStatus = _connectionStatus.asStateFlow()

    private val _telemetry = MutableStateFlow(TelemetryData())
    val telemetry = _telemetry.asStateFlow()

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
                    delay(500)

                    try {
                        (newPeripheral as? AndroidPeripheral)?.requestMtu(247)
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
                                    lastError = null,
                                    telemetryUpdatedAtMillis = System.currentTimeMillis()
                                )

                                json.has("S") -> current.copy(
                                    offsetMap = json.optDouble("oP", current.offsetMap.toDouble()).toFloat(),
                                    scaleMap = json.optDouble("sP", current.scaleMap.toDouble()).toFloat(),
                                    offsetTps = json.optDouble("oV", current.offsetTps.toDouble()).toFloat(),
                                    targetBoost = json.optDouble("tB", current.targetBoost.toDouble()).toFloat(),
                                    kP = json.optDouble("kP", current.kP.toDouble()).toFloat(),
                                    kI = json.optDouble("kI", current.kI.toDouble()).toFloat(),
                                    kD = json.optDouble("kD", current.kD.toDouble()).toFloat(),
                                    learnCoeff = json.optDouble("lA", current.learnCoeff.toDouble()).toFloat(),
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

    fun sendCommand(command: String) {
        if (command.isBlank()) return

        scope.launch {
            writeMutex.withLock {
                val currentPeripheral = peripheral
                if (currentPeripheral == null) {
                    Log.w("BLE", "Команда пропущена, соединение отсутствует: $command")
                    return@withLock
                }

                try {
                    currentPeripheral.write(rxCharacteristic, command.toByteArray(), WriteType.WithResponse)
                    Log.d("BLE", "Отправлено: $command")
                    delay(50)
                } catch (e: Exception) {
                    Log.e("BLE", "Ошибка отправки команды: $command", e)
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
            peripheral?.disconnect()
            peripheral = null
            _connectionStatus.value = "Отключено"
        }
    }
}
