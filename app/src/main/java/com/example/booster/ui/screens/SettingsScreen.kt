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
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
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
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeoutOrNull
import java.util.Locale

@Composable
fun SettingsScreen(viewModel: BoosterViewModel) {
    val settings by viewModel.settingsState.collectAsStateWithLifecycle()
    // Live telemetry stays as a State and is read (.value) only inside the cards that show live
    // values, so the 8 Hz stream recomposes just those cards — not the whole screen or the TuneRows.
    val liveState = viewModel.telemetry.collectAsStateWithLifecycle()
    val protectionMode by remember { derivedStateOf { liveState.value.mode } }
    val connectionStatus by viewModel.connectionStatus.collectAsStateWithLifecycle()
    val context = LocalContext.current

    var testDutySlider by remember { mutableFloatStateOf(0f) }
    var draftTb by remember { mutableFloatStateOf(0f) }
    var draftLb by remember { mutableFloatStateOf(0f) }
    var draftKp by remember { mutableFloatStateOf(0f) }
    var draftKi by remember { mutableFloatStateOf(0f) }
    var draftKd by remember { mutableFloatStateOf(0f) }
    var draftBh by remember { mutableFloatStateOf(0f) }
    var draftBl by remember { mutableFloatStateOf(0f) }
    var draftFs by remember { mutableFloatStateOf(0f) }
    var draftOp by remember { mutableFloatStateOf(0f) }
    var draftSp by remember { mutableFloatStateOf(0f) }
    var draftOe by remember { mutableFloatStateOf(0f) }
    var draftSe by remember { mutableFloatStateOf(0f) }
    var draftSt by remember { mutableFloatStateOf(0f) }
    var draftPr by remember { mutableFloatStateOf(0f) }
    var draftOv by remember { mutableFloatStateOf(0f) }
    var draftVp by remember { mutableFloatStateOf(0f) }
    var draftLa by remember { mutableFloatStateOf(0f) }

    var isInitialized by remember { mutableStateOf(false) }
    var showOtaDialog by remember { mutableStateOf(false) }
    var justSaved by remember { mutableStateOf(false) }
    var saveInFlight by remember { mutableStateOf(false) }
    var saveTrigger by remember { mutableStateOf(0) }

    // Keyed on isInitialized too: when the connect effect below resets it, this re-fires and
    // re-fills the drafts even if targetBoost didn't change value (fixes the stuck-init race).
    LaunchedEffect(settings.targetBoost, isInitialized) {
        if (!isInitialized && settings.targetBoost != 0f) {
            draftTb = settings.targetBoost
            draftLb = settings.limitBoostBar
            draftKp = settings.kP
            draftKi = settings.kI
            draftKd = settings.kD
            draftBh = settings.spoolBlendHigh
            draftBl = settings.spoolBlendLow
            draftFs = settings.dutyFallSlew
            draftOp = settings.offsetMap
            draftSp = settings.scaleMap
            draftOe = settings.offsetEcu
            draftSe = settings.scaleEcu
            draftSt = settings.spoolMinTps
            draftOv = settings.offsetTps
            draftPr = settings.pulsesRpm
            draftVp = settings.vssPulses
            draftLa = settings.learnCoeff
            isInitialized = true
        }
    }

    LaunchedEffect(settings.lastError) {
        val error = settings.lastError ?: return@LaunchedEffect
        Toast.makeText(context, "Ошибка ЭБУ: $error", Toast.LENGTH_SHORT).show()
    }

    LaunchedEffect(connectionStatus) {
        if (connectionStatus == "Подключено") {
            isInitialized = false
            // Settings ("S") come over notify (unacknowledged); the firmware bursts them on connect,
            // but if every copy is lost the screen would stay empty. Retry until they arrive.
            var tries = 0
            while (viewModel.settingsState.value.targetBoost == 0f && tries < 6) {
                viewModel.sendCommand("GET:SETTINGS")
                delay(500)
                tries++
            }
        }
    }

    LaunchedEffect(justSaved) {
        if (justSaved) {
            delay(1400)
            justSaved = false
        }
    }

    // ACK-driven save: send the batch (or per-key fallback) and confirm only on the real SAVE ack
    // from the ECU; surface a rejection (e.g. partial SETALL) or a no-response timeout instead of
    // claiming success blindly. Subscribe to acks BEFORE sending so a fast ack isn't missed.
    LaunchedEffect(saveTrigger) {
        if (saveTrigger == 0) return@LaunchedEffect
        saveInFlight = true
        val pairs = listOf(
            "tB" to fmt(clampValue(draftTb, 0.3f, 1.5f), "%.2f"),
            "lB" to fmt(clampValue(draftLb, 0.3f, 2.0f), "%.2f"),
            "kP" to fmt(clampValue(draftKp, 0f, 200f), "%.1f"),
            "kI" to fmt(clampValue(draftKi, 0f, 200f), "%.1f"),
            "kD" to fmt(clampValue(draftKd, 0f, 50f), "%.1f"),
            "lA" to fmt(clampValue(draftLa, 0.02f, 0.30f), "%.2f"),
            "bH" to fmt(clampValue(draftBh, 0.05f, 1.0f), "%.2f"),
            "bL" to fmt(clampValue(draftBl, 0.0f, 0.5f), "%.2f"),
            "fS" to fmt(clampValue(draftFs, 50f, 1000f), "%.0f"),
            "sT" to fmt(clampValue(draftSt, 20f, 100f), "%.0f"),
            // Sensor calibrations go out at full precision: oP is volts (1 mV matters) and the
            // scales carry 4 decimals (0.6588 bar/V), so "%.2f" would quantise the calibration.
            "oP" to fmt(clampValue(draftOp, 0.1f, 4.5f), "%.3f"),
            "sP" to fmt(clampValue(draftSp, 0.1f, 2.0f), "%.4f"),
            "oE" to fmt(clampValue(draftOe, 0.1f, 4.5f), "%.3f"),
            "sE" to fmt(clampValue(draftSe, 0.1f, 2.0f), "%.4f"),
            "oV" to fmt(clampValue(draftOv, 0.1f, 3.5f), "%.2f"),
            "pR" to fmt(clampValue(draftPr, 0.5f, 8.0f), "%.1f"),
            "vP" to fmt(clampValue(draftVp, 1.0f, 40.0f), "%.2f")
        )
        val commands = buildList {
            if (viewModel.mtu.value >= 200) {
                add("SETALL:" + pairs.joinToString(";") { "${it.first}=${it.second}" })
            } else {
                pairs.forEach { add("SET:${it.first}:${it.second}") }
            }
            add("SAVE")
        }
        val result = withTimeoutOrNull(2500) {
            coroutineScope {
                val ack = async { viewModel.ackEvents.first { it.cmd == "SAVE" || !it.ok } }
                viewModel.sendCommands(commands)
                ack.await()
            }
        }
        saveInFlight = false
        when {
            result == null -> Toast.makeText(context, "Нет ответа от ЭБУ — проверьте связь", Toast.LENGTH_SHORT).show()
            result.ok -> {
                justSaved = true
                Toast.makeText(context, "Настройки сохранены", Toast.LENGTH_SHORT).show()
            }
            else -> Toast.makeText(context, "Ошибка ЭБУ: ${result.error ?: result.cmd}", Toast.LENGTH_SHORT).show()
        }
    }

    // Button enables on whether real settings are present (targetBoost is never 0 once an "S"
    // packet arrived) — NOT on the one-shot isInitialized flag, which the connect effect resets.
    val isSettingsLoaded = settings.targetBoost != 0f

    val saveButtonColor by animateColorAsState(
        targetValue = when {
            justSaved -> StatusGreen
            saveInFlight -> AccentAmber
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

                Spacer(Modifier.height(14.dp))
                val pedalScaled = settings.pedalTargetScaling != 0
                Text(
                    if (pedalScaled) "Таргет от педали: ВКЛ" else "Таргет от педали: ВЫКЛ (режим AVC-R)",
                    fontSize = 13.sp, fontWeight = FontWeight.Bold,
                    color = if (pedalScaled) BoostBlue else AccentAmber
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    if (pedalScaled)
                        "Наддув дозируется педалью: 40% газа ≈ 0.60 от уставки, 60% ≈ 0.74, " +
                            "полный таргет с 80%. Ход педали работает как регулятор тяги."
                    else
                        "Таргет всегда полный, как на AVC-R: регулятор гребёт к уставке сразу, " +
                            "как только наддув физически возможен, независимо от хода педали.",
                    fontSize = 12.sp, color = TextGray, letterSpacing = 0.3.sp
                )
                Spacer(Modifier.height(6.dp))
                Button(
                    onClick = { viewModel.sendCommand("SET:pT:${if (pedalScaled) 0 else 1}") },
                    modifier = Modifier.fillMaxWidth().height(44.dp),
                    shape = RoundedCornerShape(10.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (pedalScaled) AccentAmber.copy(alpha = 0.85f) else BoostBlue.copy(alpha = 0.85f)
                    )
                ) {
                    Text(
                        if (pedalScaled) "ПЕРЕКЛЮЧИТЬ НА AVC-R" else "ВЕРНУТЬ ТАРГЕТ ОТ ПЕДАЛИ",
                        fontSize = 12.sp, fontWeight = FontWeight.Bold, color = NeonWhite
                    )
                }
            }
        }

        item {
            SettingsCard(title = "Защита от передува (Limp)") {
                Text(
                    "Пороги выводятся из целевого наддува автоматически и следуют за ним: " +
                        "soft = target + 0.15, hard = target + 0.20 бар. Считаются от УСТАВКИ, " +
                        "а не от текущего таргета, поэтому не опускаются на частичном газе. " +
                        "Выше soft дьюти гасится плавно (квадратично), выше hard — сразу в ноль.",
                    color = TextGray,
                    fontSize = 12.sp,
                    letterSpacing = 0.3.sp,
                    modifier = Modifier.padding(bottom = 8.dp)
                )
                // Values come from the ECU ("sL"/"hL" in the settings packet) rather than being
                // recomputed here: the +0.15/+0.20 offsets live in the firmware, and duplicating
                // them in the app means the screen lies the moment the firmware changes them.
                ReadOnlyRow("Soft limp — плавный сброс дьюти (бар)", fmt(settings.softLimpBar, "%.2f"))
                ReadOnlyRow("Hard limp — дьюти 0% (бар)", fmt(settings.hardLimpBar, "%.2f"))
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

                // The firmware keeps two sets of gains. Which one drives the loop depends on the
                // tuner switch, so spell it out — a slider that is merely a seed looks identical
                // to one that is live.
                Spacer(Modifier.height(12.dp))
                val autoOn = settings.autoTuneEnabled != 0
                val live = liveState.value
                Text(
                    if (autoOn) "Автотюн ПИД: ВКЛ" else "Автотюн ПИД: ВЫКЛ",
                    fontSize = 13.sp, fontWeight = FontWeight.Bold,
                    color = if (autoOn) BoostRed else TextGray
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    if (autoOn)
                        "В моторе работают АВТО-гейны: ${fmt(live.autoKp, "%.1f")} / " +
                            "${fmt(live.autoKi, "%.1f")} / ${fmt(live.autoKd, "%.1f")} " +
                            "· эпизодов: ${live.autoTuneEpisodes}. Ползунки выше — стартовая точка: " +
                            "правка любого сбрасывает тюнер на это значение."
                    else
                        "В моторе работают РУЧНЫЕ гейны с ползунков выше. Тюнер заморожен, " +
                            "его последние значения: ${fmt(live.autoKp, "%.1f")} / " +
                            "${fmt(live.autoKi, "%.1f")} / ${fmt(live.autoKd, "%.1f")} " +
                            "· эпизодов: ${live.autoTuneEpisodes}.",
                    fontSize = 12.sp, color = TextGray
                )
                Spacer(Modifier.height(6.dp))
                Button(
                    onClick = { viewModel.sendCommand("SET:aT:${if (autoOn) 0 else 1}") },
                    modifier = Modifier.fillMaxWidth().height(44.dp),
                    shape = RoundedCornerShape(10.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (autoOn) Color(0xFF2C2C2E) else BoostRed.copy(alpha = 0.85f)
                    )
                ) {
                    Text(
                        if (autoOn) "ВЫКЛЮЧИТЬ АВТОТЮН" else "ВКЛЮЧИТЬ АВТОТЮН",
                        fontSize = 12.sp, fontWeight = FontWeight.Bold,
                        color = if (autoOn) TextGray else NeonWhite
                    )
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
                Spacer(Modifier.height(8.dp))
                Text(
                    "Порог педали: ниже него раздув не включается вовсе. На частичном газе наддув " +
                        "ограничен дросселем, а не вестгейтом — открытый дьюти там бесполезен и греет соленоид.",
                    color = TextGray,
                    fontSize = 12.sp,
                    letterSpacing = 0.3.sp,
                    modifier = Modifier.padding(bottom = 4.dp)
                )
                TuneRow("Порог педали для раздува (%)", draftSt, 5f, "%.0f", 20f, 100f) { draftSt = it }
            }
        }

        item {
            SettingsCard(title = "Сигнал на ЭБУ двигателя") {
                val live = liveState.value
                val ecuScale = if (draftSe > 0.0001f) draftSe else 0.55f
                val ecuVolts = (minOf(live.boost, live.limitBoostBar) / ecuScale + draftOe)
                    .coerceIn(0.60f, 4.60f)
                val ecuKpa = 100f + (ecuVolts - draftOe) * ecuScale * 100f
                Text(
                    "Характеристика ШТАТНОГО датчика — в ней ЭБУ двигателя ждёт сигнал. " +
                        "Не путать с калибровкой 2-барового датчика ниже: та для измерения, эта для выхода. " +
                        "На атмосфере ЭБУ должен видеть ~100 кПа, иначе двигатель не заведётся.",
                    color = TextGray,
                    fontSize = 12.sp,
                    letterSpacing = 0.3.sp,
                    modifier = Modifier.padding(bottom = 8.dp)
                )
                TuneRow("Ноль ЭБУ (oE), В", draftOe, 0.005f, "%.3f", 0.1f, 4.5f) { draftOe = it }
                TuneRow("Множ. ЭБУ (sE), бар/В", draftSe, 0.001f, "%.4f", 0.1f, 2.0f) { draftSe = it }
                Spacer(Modifier.height(8.dp))
                ReadOnlyRow("Сейчас на выходе", "${fmt(ecuVolts, "%.3f")} В  ≈ ${fmt(ecuKpa, "%.0f")} кПа")
            }
        }

        item {
            SettingsCard(title = "Калибровки датчиков") {
                Text(
                    "Ноль MAP = напряжение 2-барового датчика на атмосфере, каким его видит АЦП. " +
                        "Взять из диагностики (строка ADC ch0) при заглушенном моторе. Авто-ноль отключён.",
                    color = TextGray,
                    fontSize = 12.sp,
                    letterSpacing = 0.3.sp,
                    modifier = Modifier.padding(bottom = 8.dp)
                )
                TuneRow("Ноль MAP (oP), В", draftOp, 0.005f, "%.3f", 0.1f, 4.5f) { draftOp = it }
                TuneRow("Множ. MAP (sP), бар/В", draftSp, 0.001f, "%.4f", 0.1f, 2.0f) { draftSp = it }
                TuneRow("Ноль TPS (oV)", draftOv, 0.01f, "%.2f", 0.1f, 3.5f) { draftOv = it }
                Spacer(Modifier.height(16.dp))
                Text("Датчики вращения:", color = TextGray, fontSize = 12.sp, letterSpacing = 0.5.sp)
                TuneRow("Зубья ДПКВ (pR)", draftPr, 0.5f, "%.1f", 0.5f, 8.0f) { draftPr = it }
                TuneRow("Имп. скорости (vP)", draftVp, 0.1f, "%.2f", 1.0f, 40.0f) { draftVp = it }
            }
        }

        if (protectionMode != 0) {
            item {
                val modeText = if (protectionMode == 1) "SOFT LIMP" else "HARD LIMP"
                val modeColor = if (protectionMode == 1) AccentAmber else BoostRed
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
                    if (saveInFlight) return@Button
                    if (!isSettingsLoaded) {
                        Toast.makeText(context, "Дождитесь загрузки основных настроек из ЭБУ", Toast.LENGTH_SHORT).show()
                        return@Button
                    }
                    saveTrigger++   // actual send + ACK handling lives in LaunchedEffect(saveTrigger)
                },
                modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp).height(60.dp),
                shape = RoundedCornerShape(14.dp),
                colors = ButtonDefaults.buttonColors(containerColor = saveButtonColor)
            ) {
                Text(
                    when {
                        justSaved -> "СОХРАНЕНО"
                        saveInFlight -> "СОХРАНЕНИЕ…"
                        else -> "СОХРАНИТЬ"
                    },
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 1.sp,
                    color = if (isSettingsLoaded || justSaved) NeonWhite else TextGray
                )
            }
        }

        item {
            SettingsCard(title = "Сервисные функции") {
                val live = liveState.value
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
                    enabled = live.speed < 2
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    "База: ${live.baseDuty}%  |  Выход: ${live.currentDuty}%",
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

// Read-only value row (label left, value right) for settings the ECU derives and the user can't edit.
@Composable
fun ReadOnlyRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 5.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(label, modifier = Modifier.weight(1f), fontSize = 14.sp, color = NeonWhite)
        Text(
            value,
            color = TextGray,
            fontSize = 14.sp,
            fontWeight = FontWeight.SemiBold,
            fontFamily = FontFamily.Monospace
        )
    }
}
