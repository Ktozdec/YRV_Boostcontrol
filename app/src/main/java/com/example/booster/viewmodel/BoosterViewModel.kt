package com.example.booster.viewmodel

import androidx.lifecycle.ViewModel
import com.example.booster.ble.BleManager

class BoosterViewModel : ViewModel() {
    private val bleManager = BleManager()

    val connectionStatus = bleManager.connectionStatus
    val telemetry = bleManager.telemetry

    // init {} удален, так как коннект должен инициироваться ПОСЛЕ получения разрешений

    fun connect() {
        bleManager.connect()
    }

    fun disconnect() {
        bleManager.disconnect()
    }

    fun sendCommand(cmd: String) {
        bleManager.sendCommand(cmd)
    }

    override fun onCleared() {
        bleManager.release()
        super.onCleared()
    }
}
