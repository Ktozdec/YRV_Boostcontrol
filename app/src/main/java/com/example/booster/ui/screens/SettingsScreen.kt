package com.example.booster.ui.screens

import android.widget.Toast
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
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
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Remove
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.text.font.FontFamily
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
    var draftLb by remember { mutableFloatStateOf(0f) }
    var draftSl by remember { mutableFloatStateOf(0f) }
    var draftHl by remember { mutableFloatStateOf(0f) }
    var draftKp by remember { mutableFloatStateOf(0f) }
    var draftKi by remember { mutableFloatStateOf(0f) }
    var draftKd by remember { mutableFloatStateOf(0f) }
    var draftBh by remember { mutableFloatStateOf(0f) }
    var draftBl by remember { mutableFloatStateOf(0f) }
    var draftFs by remember { mutableFloatStateOf(0f) }
    var draftOp by remember { mutableFloatStateOf(0f) }
    var draftSp by remember { mutableFloatStateOf(0f) }
    var draftPr by remember { mutableFloatStateOf(0f) }
    var draftOv by remember { mutableFloatStateOf(0f) }
    var draftVp by remember { mutableFloatStateOf(0f) }
    var draftLa by remember { mutableFloatStateOf(0f) }

    var isInitialized by remember { mutableStateOf(false) }
    var showOtaDialog by remember { mutableStateOf(false) }
    var justSaved by remember { mutableStateOf(false) }

    // Keyed on isInitialized too: when the connect effect below resets it, this re-fires and
    // re-fills the drafts even if targetBoost didn't change value (fixes the stuck-init race).
    LaunchedEffect(data.targetBoost, isInitialized) {
        if (!isInitialized && data.targetBoost != 0f) {
            draftTb = data.targetBoost
            draftLb = data.limitBoostBar
            draftSl = data.softLimpBar
            draftHl = data.hardLimpBar
            draftKp = data.kP
            draftKi = data.kI
            draftKd = data.kD
            draftBh = data.spoolBlendHigh
            draftBl = data.spoolBlendLow
            draftFs = data.dutyFallSlew
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
            isInitialized = false
            viewModel.sendCommand("GET:SETTINGS")
        }
    }

    LaunchedEffect(justSaved) {
        if (justSaved) {
            delay(1400)
            justSaved = false
        }
    }

    // Button enables on whether real settings are present (targetBoost is never 0 once an "S"
    // packet arrived) — NOT on the one-shot isInitialized flag, which the connect effect resets.
    val isSettingsLoaded = data.targetBoost != 0f

    val saveButtonColor by animateColorAsState(
        targetValue = when {
            justSaved -> StatusGreen
            isSettingsLoaded -> BoostBlue.copy(alpha = 0.85f)
            else -> Color(0xFF232323)
        },
        animationSpec = tween(300),
        label = "saveColor"
    )

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
                letterSpacing = (-0.5).sp,
                modifier = Modifier.padding(bottom = 16.dp)
            )
        }

        item {
            SettingsCard(title = "Target Boost / FCD") {
                TuneRow("Целевой наддув (бар)", draftTb, 0.05f, "%.2f", 0.3f, 1.5f) { draftTb = it }
                TuneRow("Лимит FCD (бар)", draftLb, 0.05f, "%.2f", 0.3f, 2.0f) { draftLb = it }
            }
        }

        item {
            SettingsCard(title = "Защита от передува (Limp)") {
                TuneRow("Soft limp — дьюти 20% (бар)", draftSl, 0.05f, "%.2f", 0.5f, 2.0f) { draftSl = it }
                TuneRow("Hard limp — дьюти 0% (бар)", draftHl, 0.05f, "%.2f", 0.5f, 2.5f) { draftHl = it }
            }
        }

        item {
            SettingsCard(title = "ПИД-регулятор и обучение") {
                TuneRow("Proportional (kP)", draftKp, 1.0f, "%.1f", 0f, 200f) { draftKp = it }
                TuneRow("Integral (kI)", draftKi, 1.0f, "%.1f", 0f, 200f) { draftKi = it }
                TuneRow("Derivative (kD)", draftKd, 1.0f, "%.1f", 0f, 50f) { draftKd = it }
                TuneRow("Скорость адаптации карты (lA)", draftLa, 0.01f, "%.2f", 0.02f, 0.30f) { draftLa = it }
                Spacer(Modifier.height(8.dp))
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = {
                            draftLa = 0.30f
                            viewModel.sendCommand("SET:lA:0.30")
                        },
                        modifier = Modifier.weight(1f).height(44.dp),
                        shape = RoundedCornerShape(10.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = BoostRed.copy(alpha = 0.85f))
                    ) {
                        Text("ОБУЧЕНИЕ", fontSize = 12.sp, fontWeight = FontWeight.Bold, color = NeonWhite)
                    }
                    Button(
                        onClick = {
                            draftLa = 0.10f
                            viewModel.sendCommand("SET:lA:0.10")
                        },
                        modifier = Modifier.weight(1f).height(44.dp),
                        shape = RoundedCornerShape(10.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF2C2C2E))
                    ) {
                        Text("НОРМА", fontSize = 12.sp, fontWeight = FontWeight.Bold, color = TextGray)
                    }
                }
            }
        }

        item {
            SettingsCard(title = "Раздув (Spool)") {
                Text(
                    "Открытый дьюти на раздуве: чем больше дефицит до таргета, тем выше дьюти (до 95% — кратковременно). У таргета плавно спускается к ПИД. Потолок 95% задан в прошивке.",
                    color = TextGray,
                    fontSize = 12.sp,
                    letterSpacing = 0.3.sp,
                    modifier = Modifier.padding(bottom = 8.dp)
                )
                TuneRow("Полный раздув при дефиците (бар)", draftBh, 0.05f, "%.2f", 0.05f, 1.0f) { draftBh = it }
                TuneRow("Конец раздува у таргета (бар)", draftBl, 0.01f, "%.2f", 0.0f, 0.5f) { draftBl = it }
                TuneRow("Скорость спуска дьюти (%/с)", draftFs, 25f, "%.0f", 50f, 1000f) { draftFs = it }
            }
        }

        item {
            SettingsCard(title = "Калибровки датчиков") {
                TuneRow("Ноль MAP (oP)", draftOp, 0.01f, "%.2f", 0.1f, 4.5f) { draftOp = it }
                TuneRow("Множ. MAP (sP)", draftSp, 0.01f, "%.2f", 0.1f, 2.0f) { draftSp = it }
                TuneRow("Ноль TPS (oV)", draftOv, 0.01f, "%.2f", 0.1f, 3.5f) { draftOv = it }
                Spacer(Modifier.height(16.dp))
                Text("Датчики вращения:", color = TextGray, fontSize = 12.sp, letterSpacing = 0.5.sp)
                TuneRow("Зубья ДПКВ (pR)", draftPr, 0.5f, "%.1f", 0.5f, 8.0f) { draftPr = it }
                TuneRow("Имп. скорости (vP)", draftVp, 0.1f, "%.2f", 1.0f, 40.0f) { draftVp = it }
            }
        }

        if (data.mode != 0) {
            item {
                val modeText = if (data.mode == 1) "SOFT LIMP" else "HARD LIMP"
                val modeColor = if (data.mode == 1) AccentAmber else BoostRed
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
            Button(
                onClick = {
                    if (!isSettingsLoaded) {
                        Toast.makeText(context, "Дождитесь загрузки основных настроек из ЭБУ", Toast.LENGTH_SHORT).show()
                        return@Button
                    }
                    scope.launch {
                        viewModel.sendCommand("SET:tB:${fmt(clampValue(draftTb, 0.3f, 1.5f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SET:lB:${fmt(clampValue(draftLb, 0.3f, 2.0f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SET:sL:${fmt(clampValue(draftSl, 0.5f, 2.0f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SET:hL:${fmt(clampValue(draftHl, 0.5f, 2.5f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SET:kP:${fmt(clampValue(draftKp, 0f, 200f), "%.1f")}")
                        delay(30)
                        viewModel.sendCommand("SET:kI:${fmt(clampValue(draftKi, 0f, 200f), "%.1f")}")
                        delay(30)
                        viewModel.sendCommand("SET:kD:${fmt(clampValue(draftKd, 0f, 50f), "%.1f")}")
                        delay(30)
                        viewModel.sendCommand("SET:lA:${fmt(clampValue(draftLa, 0.02f, 0.30f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SET:bH:${fmt(clampValue(draftBh, 0.05f, 1.0f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SET:bL:${fmt(clampValue(draftBl, 0.0f, 0.5f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SET:fS:${fmt(clampValue(draftFs, 50f, 1000f), "%.0f")}")
                        delay(30)
                        viewModel.sendCommand("SET:oP:${fmt(clampValue(draftOp, 0.1f, 4.5f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SET:sP:${fmt(clampValue(draftSp, 0.1f, 2.0f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SET:oV:${fmt(clampValue(draftOv, 0.1f, 3.5f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SET:pR:${fmt(clampValue(draftPr, 0.5f, 8.0f), "%.1f")}")
                        delay(30)
                        viewModel.sendCommand("SET:vP:${fmt(clampValue(draftVp, 1.0f, 40.0f), "%.2f")}")
                        delay(30)
                        viewModel.sendCommand("SAVE")
                        Toast.makeText(context, "Настройки сохранены", Toast.LENGTH_SHORT).show()
                        justSaved = true
                    }
                },
                modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp).height(60.dp),
                shape = RoundedCornerShape(14.dp),
                colors = ButtonDefaults.buttonColors(containerColor = saveButtonColor)
            ) {
                Text(
                    if (justSaved) "СОХРАНЕНО" else "СОХРАНИТЬ",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 1.sp,
                    color = if (isSettingsLoaded || justSaved) NeonWhite else TextGray
                )
            }
        }

        item {
            SettingsCard(title = "Сервисные функции") {
                Text(
                    "ТЕСТ СОЛЕНОИДА: ${testDutySlider.toInt()}%",
                    fontWeight = FontWeight.Bold,
                    color = NeonWhite,
                    fontFamily = FontFamily.Monospace
                )
                Slider(
                    value = testDutySlider,
                    onValueChange = { testDutySlider = it.coerceIn(0f, 100f) },
                    onValueChangeFinished = { viewModel.sendCommand("DUTY:${testDutySlider.toInt()}") },
                    valueRange = 0f..100f,
                    enabled = data.speed < 2
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    "База: ${data.baseDuty}%  |  Выход: ${data.currentDuty}%",
                    color = TextGray,
                    fontSize = 12.sp,
                    fontFamily = FontFamily.Monospace
                )
                Spacer(Modifier.height(16.dp))

                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = { showOtaDialog = true },
                        modifier = Modifier.weight(1f).height(48.dp),
                        shape = RoundedCornerShape(10.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = BoostRed.copy(alpha = 0.85f))
                    ) {
                        Text("OTA РЕЖИМ", fontSize = 12.sp, fontWeight = FontWeight.Bold, color = NeonWhite)
                    }
                    Button(
                        onClick = { viewModel.disconnect() },
                        modifier = Modifier.weight(1f).height(48.dp),
                        shape = RoundedCornerShape(10.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF2C2C2E))
                    ) {
                        Text("ОТКЛЮЧИТЬ", fontSize = 12.sp, fontWeight = FontWeight.Bold, color = TextGray)
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
                    "Внимание!\n\n1. Соединение будет разорвано.\n2. Подключитесь к Wi-Fi сети 'YRV_Boost_Pro'.\n3. Откройте в браузере http://192.168.4.1\n4. Выберите .bin файл для загрузки.",
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
        modifier = Modifier.fillMaxWidth().padding(bottom = 10.dp),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = TrackBg)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                title,
                fontSize = 15.sp,
                fontWeight = FontWeight.Bold,
                color = NeonWhite,
                letterSpacing = 0.2.sp
            )
            Spacer(Modifier.height(14.dp))
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
        modifier = Modifier.fillMaxWidth().padding(vertical = 5.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(label, modifier = Modifier.weight(1f), fontSize = 14.sp, color = NeonWhite)
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            IconButton(
                onClick = { onValueChange(clampValue(value - step, minValue, maxValue)) },
                modifier = Modifier
                    .size(36.dp)
                    .background(DarkBg, RoundedCornerShape(8.dp))
                    .border(1.dp, Color(0xFF3A3A3C), RoundedCornerShape(8.dp))
            ) {
                Icon(
                    imageVector = Icons.Filled.Remove,
                    contentDescription = null,
                    tint = NeonWhite,
                    modifier = Modifier.size(16.dp)
                )
            }
            Box(
                modifier = Modifier
                    .width(76.dp)
                    .background(DarkBg, RoundedCornerShape(8.dp))
                    .border(1.dp, Color(0xFF3A3A3C), RoundedCornerShape(8.dp))
                    .padding(vertical = 9.dp, horizontal = 4.dp),
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
                    textStyle = TextStyle(
                        textAlign = TextAlign.Center,
                        fontWeight = FontWeight.SemiBold,
                        color = NeonWhite,
                        fontSize = 14.sp,
                        fontFamily = FontFamily.Monospace
                    ),
                    singleLine = true
                )
            }
            IconButton(
                onClick = { onValueChange(clampValue(value + step, minValue, maxValue)) },
                modifier = Modifier
                    .size(36.dp)
                    .background(DarkBg, RoundedCornerShape(8.dp))
                    .border(1.dp, Color(0xFF3A3A3C), RoundedCornerShape(8.dp))
            ) {
                Icon(
                    imageVector = Icons.Filled.Add,
                    contentDescription = null,
                    tint = NeonWhite,
                    modifier = Modifier.size(16.dp)
                )
            }
        }
    }
}

private fun clampValue(value: Float, min: Float, max: Float): Float = value.coerceIn(min, max)

private fun fmt(value: Float, pattern: String): String = String.format(Locale.US, pattern, value)
