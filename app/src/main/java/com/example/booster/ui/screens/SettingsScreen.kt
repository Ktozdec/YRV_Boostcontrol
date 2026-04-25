package com.example.booster.ui.screens

import android.widget.Toast
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.booster.viewmodel.BoosterViewModel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.util.Locale

@Composable
fun SettingsScreen(viewModel: BoosterViewModel) {
    val data by viewModel.telemetry.collectAsStateWithLifecycle()
    val connectionStatus by viewModel.connectionStatus.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var testDutySlider by remember { mutableFloatStateOf(0f) }
    var draftTb by remember { mutableFloatStateOf(0f) }
    var draftKp by remember { mutableFloatStateOf(0f) }
    var draftKi by remember { mutableFloatStateOf(0f) }
    var draftKd by remember { mutableFloatStateOf(0f) }
    var draftOp by remember { mutableFloatStateOf(0f) }
    var draftSp by remember { mutableFloatStateOf(0f) }
    var draftPr by remember { mutableFloatStateOf(0f) }
    var draftOv by remember { mutableFloatStateOf(0f) }
    var draftVp by remember { mutableFloatStateOf(0f) }
    var draftLa by remember { mutableFloatStateOf(0f) }

    var isInitialized by remember { mutableStateOf(false) }
    var showOtaDialog by remember { mutableStateOf(false) }

    LaunchedEffect(data.targetBoost) {
        if (!isInitialized && data.targetBoost != 0f) {
            draftTb = data.targetBoost
            draftKp = data.kP
            draftKi = data.kI
            draftKd = data.kD
            draftOp = data.offsetMap
            draftSp = data.scaleMap
            draftOv = data.offsetTps
            draftPr = data.pulsesRpm
            draftVp = data.vssPulses
            draftLa = data.learnCoeff
            isInitialized = true
        }
    }

    LaunchedEffect(data.lastError) {
        val error = data.lastError ?: return@LaunchedEffect
        Toast.makeText(context, "Ошибка ЭБУ: $error", Toast.LENGTH_SHORT).show()
    }

    LaunchedEffect(connectionStatus) {
        if (connectionStatus == "Подключено") {
            viewModel.sendCommand("GET:SETTINGS")
        }
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize().background(DarkBg).padding(horizontal = 8.dp),
        contentPadding = PaddingValues(top = 16.dp, bottom = 80.dp)
    ) {
        item {
            Text(
                "Настройки ЭБУ",
                fontSize = 24.sp,
                fontWeight = FontWeight.Black,
                color = NeonWhite,
                modifier = Modifier.padding(bottom = 16.dp)
            )
        }

        item {
            SettingsCard(title = "Target Boost (Бар)") {
                TuneRow("Целевой наддув", draftTb, 0.05f, "%.2f", 0.3f, 1.5f) { draftTb = it }
            }
        }

        item {
            SettingsCard(title = "ПИД-регулятор и обучение") {
                TuneRow("Proportional (kP)", draftKp, 1.0f, "%.1f", 0f, 200f) { draftKp = it }
                TuneRow("Integral (kI)", draftKi, 1.0f, "%.1f", 0f, 200f) { draftKi = it }
                TuneRow("Derivative (kD)", draftKd, 1.0f, "%.1f", 0f, 50f) { draftKd = it }
                TuneRow("Скорость обучения (lA)", draftLa, 0.005f, "%.3f", 0.005f, 0.15f) { draftLa = it }
            }
        }

        item {
            SettingsCard(title = "Калибровки датчиков") {
                TuneRow("Ноль MAP (oP)", draftOp, 0.01f, "%.2f", 0.1f, 4.5f) { draftOp = it }
                TuneRow("Множ. MAP (sP)", draftSp, 0.01f, "%.2f", 0.1f, 2.0f) { draftSp = it }
                TuneRow("Ноль TPS (oV)", draftOv, 0.01f, "%.2f", 0.1f, 3.5f) { draftOv = it }
                Spacer(Modifier.height(16.dp))
                Text("Датчики вращения:", color = TextGray, fontSize = 12.sp)
                TuneRow("Зубья ДПКВ (pR)", draftPr, 0.5f, "%.1f", 0.5f, 8.0f) { draftPr = it }
                TuneRow("Имп. скорости (vP)", draftVp, 0.1f, "%.2f", 1.0f, 40.0f) { draftVp = it }
            }
        }

        if (data.mode != 0) {
            item {
                val modeText = if (data.mode == 1) "SOFT LIMP" else "HARD LIMP"
                val modeColor = if (data.mode == 1) Color(0xFFFFC107) else BoostRed
                SettingsCard(title = "Статус защиты") {
                    Text(
                        "ЭБУ сейчас в режиме $modeText. На время настройки проверь датчики, калибровки и фактический буст.",
                        color = modeColor,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.Bold
                    )
                }
            }
        }

        item {
            val isSettingsLoaded = isInitialized && data.targetBoost != 0f
            Button(
                onClick = {
                    if (!isSettingsLoaded) {
                        Toast.makeText(context, "Дождитесь загрузки основных настроек из ЭБУ", Toast.LENGTH_SHORT).show()
                        return@Button
                    }
                    scope.launch {
                        viewModel.sendCommand("SET:tB:${fmt(clampValue(draftTb, 0.3f, 1.5f), "%.2f")}")
                        delay(90)
                        viewModel.sendCommand("SET:kP:${fmt(clampValue(draftKp, 0f, 200f), "%.1f")}")
                        delay(90)
                        viewModel.sendCommand("SET:kI:${fmt(clampValue(draftKi, 0f, 200f), "%.1f")}")
                        delay(90)
                        viewModel.sendCommand("SET:kD:${fmt(clampValue(draftKd, 0f, 50f), "%.1f")}")
                        delay(90)
                        viewModel.sendCommand("SET:lA:${fmt(clampValue(draftLa, 0.005f, 0.15f), "%.3f")}")
                        delay(90)
                        viewModel.sendCommand("SET:oP:${fmt(clampValue(draftOp, 0.1f, 4.5f), "%.2f")}")
                        delay(90)
                        viewModel.sendCommand("SET:sP:${fmt(clampValue(draftSp, 0.1f, 2.0f), "%.2f")}")
                        delay(90)
                        viewModel.sendCommand("SET:oV:${fmt(clampValue(draftOv, 0.1f, 3.5f), "%.2f")}")
                        delay(90)
                        viewModel.sendCommand("SET:pR:${fmt(clampValue(draftPr, 0.5f, 8.0f), "%.1f")}")
                        delay(90)
                        viewModel.sendCommand("SET:vP:${fmt(clampValue(draftVp, 1.0f, 40.0f), "%.2f")}")
                        delay(120)
                        viewModel.sendCommand("SAVE")
                        Toast.makeText(context, "Настройки сохранены без карты", Toast.LENGTH_SHORT).show()
                    }
                },
                modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp).height(64.dp),
                colors = ButtonDefaults.buttonColors(containerColor = if (isSettingsLoaded) StatusGreen else Color.DarkGray)
            ) {
                Text(
                    "СОХРАНИТЬ",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    color = if (isSettingsLoaded) NeonWhite else TextGray
                )
            }
        }

        item {
            SettingsCard(title = "Сервисные функции") {
                Text("ТЕСТ СОЛЕНОИДА: ${testDutySlider.toInt()}%", fontWeight = FontWeight.Bold, color = NeonWhite)
                Slider(
                    value = testDutySlider,
                    onValueChange = { testDutySlider = it.coerceIn(0f, 100f) },
                    onValueChangeFinished = { viewModel.sendCommand("DUTY:${testDutySlider.toInt()}") },
                    valueRange = 0f..100f,
                    enabled = data.speed < 2
                )

                Spacer(Modifier.height(16.dp))
                Text("Live Base Duty: ${data.baseDuty}% | Live Out Duty: ${data.currentDuty}%", color = TextGray, fontSize = 12.sp)
                Spacer(Modifier.height(16.dp))

                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = { showOtaDialog = true },
                        modifier = Modifier.weight(1f).height(48.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFD32F2F))
                    ) {
                        Text("OTA РЕЖИМ", fontSize = 12.sp, color = NeonWhite)
                    }

                    Button(
                        onClick = { viewModel.disconnect() },
                        modifier = Modifier.weight(1f).height(48.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = Color.DarkGray)
                    ) {
                        Text("ОТКЛЮЧИТЬ", fontSize = 12.sp, color = NeonWhite)
                    }
                }
            }
        }
    }

    if (showOtaDialog) {
        AlertDialog(
            onDismissRequest = { showOtaDialog = false },
            containerColor = TrackBg,
            title = { Text("Обновление прошивки (OTA)", color = NeonWhite) },
            text = {
                Text(
                    "Внимание!\n\n1. Соединение будет разорвано.\n2. Подключитесь к Wi‑Fi сети 'YRV_Boost_Pro'.\n3. Откройте в браузере http://192.168.4.1\n4. Выберите .bin файл для загрузки.",
                    color = TextGray
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.sendCommand("OTA:ON")
                    viewModel.disconnect()
                    showOtaDialog = false
                }) {
                    Text("ПОНЯТНО, НАЧАТЬ", color = BoostRed, fontWeight = FontWeight.Bold)
                }
            },
            dismissButton = {
                TextButton(onClick = { showOtaDialog = false }) {
                    Text("ОТМЕНА", color = NeonWhite)
                }
            }
        )
    }
}

@Composable
fun SettingsCard(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth().padding(bottom = 12.dp),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp),
        colors = CardDefaults.cardColors(containerColor = TrackBg)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(title, fontSize = 16.sp, fontWeight = FontWeight.Bold, color = NeonWhite)
            Spacer(Modifier.height(12.dp))
            content()
        }
    }
}

@Composable
fun TuneRow(
    label: String,
    value: Float,
    step: Float,
    format: String,
    minValue: Float,
    maxValue: Float,
    onValueChange: (Float) -> Unit
) {
    var textValue by remember(value) { mutableStateOf(String.format(Locale.US, format, value)) }

    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(label, modifier = Modifier.weight(1f), fontSize = 16.sp, color = NeonWhite)
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedButton(
                onClick = { onValueChange(clampValue(value - step, minValue, maxValue)) },
                modifier = Modifier.size(40.dp),
                contentPadding = PaddingValues(0.dp),
                shape = MaterialTheme.shapes.small
            ) {
                Text("-", fontSize = 20.sp, color = NeonWhite)
            }
            Box(
                modifier = Modifier.width(80.dp).padding(horizontal = 8.dp).background(DarkBg, MaterialTheme.shapes.small).padding(vertical = 8.dp),
                contentAlignment = Alignment.Center
            ) {
                BasicTextField(
                    value = textValue,
                    onValueChange = {
                        textValue = it
                        it.toFloatOrNull()?.let { parsed ->
                            onValueChange(clampValue(parsed, minValue, maxValue))
                        }
                    },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    textStyle = TextStyle(textAlign = TextAlign.Center, fontWeight = FontWeight.Bold, color = NeonWhite),
                    singleLine = true
                )
            }
            OutlinedButton(
                onClick = { onValueChange(clampValue(value + step, minValue, maxValue)) },
                modifier = Modifier.size(40.dp),
                contentPadding = PaddingValues(0.dp),
                shape = MaterialTheme.shapes.small
            ) {
                Text("+", fontSize = 20.sp, color = NeonWhite)
            }
        }
    }
}

private fun clampValue(value: Float, min: Float, max: Float): Float = value.coerceIn(min, max)

private fun fmt(value: Float, pattern: String): String = String.format(Locale.US, pattern, value)
