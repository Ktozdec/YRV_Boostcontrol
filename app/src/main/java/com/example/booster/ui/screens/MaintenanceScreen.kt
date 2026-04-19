package com.example.booster.ui.screens

import android.widget.Toast
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.booster.data.AppPreferencesRepository
import com.example.booster.ui.components.GpsCalibrationDialog
import com.example.booster.ui.permissions.hasLocationPermission
import com.example.booster.viewmodel.BoosterViewModel
import java.util.Locale
import java.util.concurrent.TimeUnit

private data class MaintItem(val id: String, val name: String, val defaultKm: Int, val defaultDays: Int)

@Composable
fun MaintenanceScreen(viewModel: BoosterViewModel) {
    val data by viewModel.telemetry.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val prefs = remember { AppPreferencesRepository(context).maintenancePrefs }

    var currentMileage by remember { mutableIntStateOf(data.totalDistance.toInt()) }
    var showMileageDialog by remember { mutableStateOf(false) }
    var editMileageInput by remember { mutableStateOf("") }

    var editingIntervalId by remember { mutableStateOf<String?>(null) }
    var editKmInput by remember { mutableStateOf("") }
    var editDaysInput by remember { mutableStateOf("") }

    var showTireCalcDialog by remember { mutableStateOf(false) }
    var showGpsDialog by remember { mutableStateOf(false) }

    var stockWidth by remember { mutableStateOf(prefs.getString("stockWidth", "175") ?: "175") }
    var stockProfile by remember { mutableStateOf(prefs.getString("stockProfile", "55") ?: "55") }
    var stockRadius by remember { mutableStateOf(prefs.getString("stockRadius", "15") ?: "15") }

    var tireWidth by remember(data.tireW) { mutableStateOf(if (data.tireW > 0) data.tireW.toString() else "195") }
    var tireProfile by remember(data.tireA) { mutableStateOf(if (data.tireA > 0) data.tireA.toString() else "50") }
    var tireRadius by remember(data.tireR) { mutableStateOf(if (data.tireR > 0) data.tireR.toString() else "15") }

    val maintItems = listOf(
        MaintItem("oil_engine", "Масло двигателя", 5000, 365),
        MaintItem("oil_gearbox", "Масло КПП (ATF)", 40000, 1095),
        MaintItem("filter_air", "Воздушный фильтр", 10000, 365),
        MaintItem("spark_plugs", "Свечи зажигания", 20000, 730),
        MaintItem("filter_fuel", "Топливный фильтр", 40000, 1095)
    )

    LaunchedEffect(data.totalDistance) {
        currentMileage = data.totalDistance.toInt()
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize().background(DarkBg).padding(horizontal = 16.dp),
        contentPadding = PaddingValues(top = 24.dp, bottom = 80.dp)
    ) {
        item {
            Card(
                modifier = Modifier.fillMaxWidth().padding(bottom = 24.dp),
                colors = CardDefaults.cardColors(containerColor = TrackBg),
                shape = RoundedCornerShape(16.dp)
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth().padding(24.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column {
                        Text("ОДОМЕТР", color = TextGray, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                        Spacer(modifier = Modifier.height(4.dp))
                        Row(verticalAlignment = Alignment.Bottom) {
                            Text(
                                text = "$currentMileage",
                                color = NeonWhite,
                                fontSize = 40.sp,
                                fontFamily = FontFamily.Monospace,
                                fontWeight = FontWeight.Bold
                            )
                            Text(" км", color = TextGray, fontSize = 16.sp, modifier = Modifier.padding(bottom = 6.dp, start = 4.dp))
                        }
                        Spacer(modifier = Modifier.height(6.dp))
                        Text(
                            "Моточасы: ${String.format(Locale.US, "%.1f", data.engineHours)} ч",
                            color = TextGray,
                            fontSize = 12.sp
                        )
                    }
                    androidx.compose.material3.IconButton(
                        onClick = {
                            editMileageInput = currentMileage.toString()
                            showMileageDialog = true
                        },
                        modifier = Modifier.background(DarkBg, RoundedCornerShape(12.dp))
                    ) {
                        androidx.compose.material3.Icon(Icons.Default.Edit, contentDescription = "Редактировать", tint = NeonWhite)
                    }
                }
            }
        }

        item {
            Text(
                "КАРТА ОБСЛУЖИВАНИЯ",
                color = NeonWhite,
                fontSize = 18.sp,
                fontWeight = FontWeight.Black,
                modifier = Modifier.padding(bottom = 16.dp)
            )
        }

        items(maintItems.size) { index ->
            val item = maintItems[index]
            val intervalKm = prefs.getInt("${item.id}_interval_km", prefs.getInt("${item.id}_interval", item.defaultKm))
            val intervalDays = prefs.getInt("${item.id}_interval_days", item.defaultDays)
            val lastKm = prefs.getInt("${item.id}_last_km", prefs.getInt("${item.id}_last", currentMileage))
            val lastDateMs = prefs.getLong("${item.id}_last_date", System.currentTimeMillis())

            val passedKm = (currentMileage - lastKm).coerceAtLeast(0)
            val passedDays = TimeUnit.MILLISECONDS.toDays(System.currentTimeMillis() - lastDateMs).toInt().coerceAtLeast(0)
            val remainingKm = (intervalKm - passedKm).coerceAtLeast(0)
            val remainingDays = (intervalDays - passedDays).coerceAtLeast(0)
            val kmProgress = (remainingKm.toFloat() / intervalKm.coerceAtLeast(1)).coerceIn(0f, 1f)
            val dayProgress = (remainingDays.toFloat() / intervalDays.coerceAtLeast(1)).coerceIn(0f, 1f)
            val progress = minOf(kmProgress, dayProgress)
            val progressColor = when {
                progress > 0.3f -> StatusGreen
                progress > 0.1f -> Color(0xFFFFC107)
                else -> BoostRed
            }

            Card(
                modifier = Modifier.fillMaxWidth().padding(bottom = 12.dp),
                colors = CardDefaults.cardColors(containerColor = Color.Transparent),
                border = BorderStroke(1.dp, TrackBg),
                shape = RoundedCornerShape(12.dp)
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(item.name, color = NeonWhite, fontSize = 16.sp, fontWeight = FontWeight.Bold)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                "ЛИМИТ",
                                color = TextGray,
                                fontSize = 10.sp,
                                fontWeight = FontWeight.Bold,
                                modifier = Modifier
                                    .clickable {
                                        editKmInput = intervalKm.toString()
                                        editDaysInput = intervalDays.toString()
                                        editingIntervalId = item.id
                                    }
                                    .background(TrackBg, RoundedCornerShape(6.dp))
                                    .padding(horizontal = 12.dp, vertical = 6.dp)
                            )
                            Spacer(modifier = Modifier.width(8.dp))
                            OutlinedButton(
                                onClick = {
                                    prefs.edit()
                                        .putInt("${item.id}_last_km", currentMileage)
                                        .putLong("${item.id}_last_date", System.currentTimeMillis())
                                        .apply()
                                    currentMileage += 1
                                    currentMileage -= 1
                                    Toast.makeText(context, "${item.name}: интервал сброшен", Toast.LENGTH_SHORT).show()
                                },
                                modifier = Modifier.height(32.dp),
                                contentPadding = PaddingValues(horizontal = 12.dp),
                                colors = ButtonDefaults.outlinedButtonColors(contentColor = NeonWhite),
                                border = BorderStroke(1.dp, NeonWhite.copy(alpha = 0.3f)),
                                shape = RoundedCornerShape(8.dp)
                            ) {
                                Text("СБРОС", fontSize = 10.sp, fontWeight = FontWeight.Bold)
                            }
                        }
                    }
                    Spacer(modifier = Modifier.height(12.dp))
                    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text("Осталось:", color = TextGray, fontSize = 12.sp)
                        Text(
                            "$remainingKm км / $remainingDays дн",
                            color = progressColor,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                    Spacer(modifier = Modifier.height(4.dp))
                    LinearProgressIndicator(
                        progress = { progress },
                        modifier = Modifier.fillMaxWidth().height(8.dp),
                        color = progressColor,
                        trackColor = DarkBg,
                        strokeCap = StrokeCap.Round
                    )
                }
            }
        }

        item {
            Spacer(modifier = Modifier.height(16.dp))
            Text(
                "ПАРАМЕТРЫ КОЛЕС",
                color = NeonWhite,
                fontSize = 18.sp,
                fontWeight = FontWeight.Black,
                modifier = Modifier.padding(bottom = 16.dp)
            )

            Card(
                modifier = Modifier.fillMaxWidth().padding(bottom = 16.dp),
                colors = CardDefaults.cardColors(containerColor = TrackBg),
                shape = RoundedCornerShape(16.dp)
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceEvenly,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        WheelField("Ширина", tireWidth) { tireWidth = it }
                        Text("/", color = TextGray, fontSize = 24.sp)
                        WheelField("Профиль", tireProfile) { tireProfile = it }
                        Text("R", color = NeonWhite, fontSize = 20.sp, fontWeight = FontWeight.Bold)
                        WheelField("Радиус", tireRadius) { tireRadius = it }
                    }
                    Spacer(modifier = Modifier.height(16.dp))
                    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(
                            onClick = { showTireCalcDialog = true },
                            modifier = Modifier.weight(1f).height(48.dp),
                            shape = RoundedCornerShape(8.dp),
                            colors = ButtonDefaults.outlinedButtonColors(containerColor = DarkBg),
                            border = BorderStroke(1.dp, Color(0xFF444444))
                        ) {
                            Text("ШИННЫЙ КАЛЬКУЛЯТОР", fontSize = 10.sp, fontWeight = FontWeight.Bold, color = TextGray)
                        }
                        OutlinedButton(
                            onClick = {
                                if (hasLocationPermission(context)) {
                                    showGpsDialog = true
                                } else {
                                    Toast.makeText(
                                        context,
                                        "Нет доступа к геолокации. При первом запуске приложение должно запросить его. Если этого не произошло, после переустановки появится системный запрос.",
                                        Toast.LENGTH_LONG
                                    ).show()
                                }
                            },
                            modifier = Modifier.weight(1f).height(48.dp),
                            shape = RoundedCornerShape(8.dp),
                            colors = ButtonDefaults.outlinedButtonColors(containerColor = DarkBg),
                            border = BorderStroke(1.dp, StatusGreen)
                        ) {
                            Text("GPS КАЛИБРОВКА", fontSize = 10.sp, fontWeight = FontWeight.Bold, color = StatusGreen)
                        }
                    }
                }
            }
        }
    }

    if (showMileageDialog) {
        AlertDialog(
            onDismissRequest = { showMileageDialog = false },
            containerColor = TrackBg,
            title = { Text("Корректировка одометра", color = NeonWhite, fontWeight = FontWeight.Bold) },
            text = {
                OutlinedTextField(
                    value = editMileageInput,
                    onValueChange = { editMileageInput = it },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    colors = OutlinedTextFieldDefaults.colors(focusedTextColor = NeonWhite, unfocusedTextColor = NeonWhite)
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    editMileageInput.toIntOrNull()?.let {
                        currentMileage = it
                        viewModel.sendCommand("SET:oD:${String.format(Locale.US, "%.1f", it.toFloat())}")
                        viewModel.sendCommand("SAVE")
                        Toast.makeText(context, "Одометр отправлен в ЭБУ", Toast.LENGTH_SHORT).show()
                    }
                    showMileageDialog = false
                }) {
                    Text("СОХРАНИТЬ", color = StatusGreen, fontWeight = FontWeight.Bold)
                }
            },
            dismissButton = {
                TextButton(onClick = { showMileageDialog = false }) {
                    Text("ОТМЕНА", color = TextGray)
                }
            }
        )
    }

    if (editingIntervalId != null) {
        AlertDialog(
            onDismissRequest = { editingIntervalId = null },
            containerColor = TrackBg,
            title = { Text("Настройка лимита", color = NeonWhite, fontWeight = FontWeight.Bold) },
            text = {
                Column {
                    OutlinedTextField(
                        value = editKmInput,
                        onValueChange = { editKmInput = it },
                        label = { Text("Километры", color = TextGray) },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        colors = OutlinedTextFieldDefaults.colors(focusedTextColor = NeonWhite, unfocusedTextColor = NeonWhite)
                    )
                    Spacer(Modifier.height(8.dp))
                    OutlinedTextField(
                        value = editDaysInput,
                        onValueChange = { editDaysInput = it },
                        label = { Text("Дни", color = TextGray) },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        colors = OutlinedTextFieldDefaults.colors(focusedTextColor = NeonWhite, unfocusedTextColor = NeonWhite)
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    val km = editKmInput.toIntOrNull() ?: 0
                    val days = editDaysInput.toIntOrNull() ?: 0
                    if (km > 0 && days > 0) {
                        prefs.edit()
                            .putInt("${editingIntervalId}_interval_km", km)
                            .putInt("${editingIntervalId}_interval_days", days)
                            .apply()
                        currentMileage += 1
                        currentMileage -= 1
                    }
                    editingIntervalId = null
                }) {
                    Text("СОХРАНИТЬ", color = StatusGreen, fontWeight = FontWeight.Bold)
                }
            },
            dismissButton = {
                TextButton(onClick = { editingIntervalId = null }) {
                    Text("ОТМЕНА", color = TextGray)
                }
            }
        )
    }

    if (showTireCalcDialog) {
        val w1 = stockWidth.toFloatOrNull() ?: 175f
        val p1 = stockProfile.toFloatOrNull() ?: 55f
        val r1 = stockRadius.toFloatOrNull() ?: 15f
        val w2 = tireWidth.toFloatOrNull() ?: 195f
        val p2 = tireProfile.toFloatOrNull() ?: 50f
        val r2 = tireRadius.toFloatOrNull() ?: 15f
        val d1 = (w1 * p1 / 100f * 2f) + (r1 * 25.4f)
        val d2 = (w2 * p2 / 100f * 2f) + (r2 * 25.4f)
        val ratio = if (d1 > 0) d2 / d1 else 1f
        val diffPercent = (ratio - 1f) * 100f
        val realSpeed = 100f * ratio

        AlertDialog(
            onDismissRequest = { showTireCalcDialog = false },
            containerColor = TrackBg,
            title = { Text("Шинный калькулятор", color = NeonWhite, fontWeight = FontWeight.Bold) },
            text = {
                Column {
                    Text("Заводской размер:", color = TextGray, fontSize = 12.sp)
                    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                        StockField(stockWidth) { stockWidth = it; prefs.edit().putString("stockWidth", it).apply() }
                        StockField(stockProfile) { stockProfile = it; prefs.edit().putString("stockProfile", it).apply() }
                        StockField(stockRadius) { stockRadius = it; prefs.edit().putString("stockRadius", it).apply() }
                    }
                    Spacer(modifier = Modifier.height(16.dp))
                    Text(
                        "Изменение диаметра: ${if (diffPercent > 0) "+" else ""}${String.format(Locale.US, "%.1f", diffPercent)}%",
                        color = if (diffPercent > 0) StatusGreen else BoostRed,
                        fontWeight = FontWeight.Bold
                    )
                    Text(
                        "При 100 км/ч реальная скорость: ${String.format(Locale.US, "%.1f", realSpeed)} км/ч",
                        color = NeonWhite
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.sendCommand("SET:tW:${w2.toInt()}")
                    viewModel.sendCommand("SET:tA:${p2.toInt()}")
                    viewModel.sendCommand("SET:tR:${r2.toInt()}")
                    viewModel.sendCommand("SAVE")
                    Toast.makeText(context, "Размер колес отправлен в ЭБУ", Toast.LENGTH_LONG).show()
                    showTireCalcDialog = false
                }) {
                    Text("В ЭБУ", color = StatusGreen, fontWeight = FontWeight.Bold)
                }
            },
            dismissButton = {
                TextButton(onClick = { showTireCalcDialog = false }) {
                    Text("ЗАКРЫТЬ", color = TextGray)
                }
            }
        )
    }

    if (showGpsDialog) {
        GpsCalibrationDialog(
            context = context,
            telemetry = data,
            onApply = { newGpsVp ->
                viewModel.sendCommand("SET:vP:${String.format(Locale.US, "%.2f", newGpsVp)}")
                viewModel.sendCommand("SAVE")
                Toast.makeText(context, "Калибровка по GPS применена", Toast.LENGTH_LONG).show()
                showGpsDialog = false
            },
            onDismiss = { showGpsDialog = false }
        )
    }
}

@Composable
private fun WheelField(label: String, value: String, onValueChange: (String) -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(label, color = TextGray, fontSize = 10.sp)
        BasicTextField(
            value = value,
            onValueChange = onValueChange,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            textStyle = TextStyle(
                color = NeonWhite,
                fontSize = 24.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center
            ),
            modifier = Modifier.width(60.dp).background(DarkBg, RoundedCornerShape(8.dp)).padding(4.dp)
        )
    }
}

@Composable
private fun StockField(value: String, onValueChange: (String) -> Unit) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        modifier = Modifier.width(88.dp),
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        colors = OutlinedTextFieldDefaults.colors(focusedTextColor = NeonWhite, unfocusedTextColor = NeonWhite)
    )
}
