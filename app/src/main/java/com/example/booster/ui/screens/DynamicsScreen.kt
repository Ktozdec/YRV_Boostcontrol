package com.example.booster.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Divider
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.booster.data.AppPreferencesRepository
import com.example.booster.viewmodel.BoosterViewModel
import kotlinx.coroutines.delay
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private data class DragRun(val date: String, val t60: Long?, val t100: Long?, val t402: Long?)

@Composable
fun DynamicsScreen(viewModel: BoosterViewModel) {
    val data by viewModel.telemetry.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val prefs = remember { AppPreferencesRepository(context) }.dynamicsPrefs

    fun loadHistory(): List<DragRun> {
        val str = prefs.getString("drag_history_json", null) ?: return emptyList()
        val list = mutableListOf<DragRun>()
        try {
            val array = JSONArray(str)
            for (i in 0 until array.length()) {
                val obj = array.getJSONObject(i)
                list.add(
                    DragRun(
                        date = obj.getString("date"),
                        t60 = if (obj.has("t60")) obj.getLong("t60") else null,
                        t100 = if (obj.has("t100")) obj.getLong("t100") else null,
                        t402 = if (obj.has("t402")) obj.getLong("t402") else null
                    )
                )
            }
        } catch (_: Exception) {
        }
        return list
    }

    fun saveHistory(list: List<DragRun>) {
        val jsonArray = JSONArray()
        list.forEach { run ->
            val obj = JSONObject()
            obj.put("date", run.date)
            run.t60?.let { obj.put("t60", it) }
            run.t100?.let { obj.put("t100", it) }
            run.t402?.let { obj.put("t402", it) }
            jsonArray.put(obj)
        }
        prefs.edit().putString("drag_history_json", jsonArray.toString()).apply()
    }

    var runHistory by remember { mutableStateOf(loadHistory()) }
    var showHistory by remember { mutableStateOf(false) }
    var isReadyToStart by remember { mutableStateOf(data.speed == 0) }
    var isRunning by remember { mutableStateOf(false) }
    var startTime by remember { mutableLongStateOf(0L) }
    var time0to60 by remember { mutableStateOf<Long?>(null) }
    var time0to100 by remember { mutableStateOf<Long?>(null) }
    var time402m by remember { mutableStateOf<Long?>(null) }
    var runDistance by remember { mutableFloatStateOf(0f) }
    var liveTime by remember { mutableLongStateOf(0L) }

    DisposableEffect(Unit) {
        onDispose { isRunning = false }
    }

    LaunchedEffect(data.speed) {
        val currentSpeed = data.speed
        val now = System.currentTimeMillis()

        if (currentSpeed == 0) {
            isReadyToStart = true
            if (isRunning) {
                isRunning = false
                if (time0to60 != null || time0to100 != null || time402m != null) {
                    val df = SimpleDateFormat("dd.MM HH:mm", Locale.getDefault())
                    val newRun = DragRun(df.format(Date()), time0to60, time0to100, time402m)
                    runHistory = (listOf(newRun) + runHistory).take(30)
                    saveHistory(runHistory)
                }
            }
        } else if (isReadyToStart && !isRunning) {
            isRunning = true
            isReadyToStart = false
            startTime = now
            runDistance = 0f
            time0to60 = null
            time0to100 = null
            time402m = null
            liveTime = 0L
        } else if (isRunning) {
            val currentRunTime = now - startTime
            if (currentSpeed >= 60 && time0to60 == null) time0to60 = currentRunTime
            if (currentSpeed >= 100 && time0to100 == null) time0to100 = currentRunTime
        }
    }

    LaunchedEffect(isRunning, data.speed) {
        var lastTickTime = System.currentTimeMillis()
        while (isRunning) {
            val now = System.currentTimeMillis()
            val dt = (now - lastTickTime) / 1000f
            lastTickTime = now

            val speedMs = data.speed / 3.6f
            runDistance += speedMs * dt
            liveTime = now - startTime

            if (runDistance >= 402f && time402m == null) {
                time402m = liveTime
            }
            delay(16)
        }
    }

    fun formatTime(ms: Long?): String {
        if (ms == null) return "--.--"
        val sec = ms / 1000
        val millis = (ms % 1000) / 10
        return String.format(Locale.US, "%d.%02d", sec, millis)
    }

    val textGlowStyle = TextStyle(
        shadow = Shadow(color = NeonWhite.copy(alpha = 0.5f), blurRadius = 8f),
        fontFamily = FontFamily.Monospace
    )

    Column(
        modifier = Modifier.fillMaxSize().background(DarkBg).padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(modifier = Modifier.height(16.dp))

        val statusText = when {
            isRunning -> "ЗАМЕР АКТИВЕН... (ДО ОСТАНОВКИ)"
            isReadyToStart -> "ОСТАНОВКА. ГОТОВ К СТАРТУ"
            else -> "СБРОС СКОРОСТИ ДО 0 ДЛЯ СТАРТА"
        }
        Text(
            text = statusText,
            color = if (isReadyToStart) StatusGreen else Color(0xFFFFC107),
            fontWeight = FontWeight.Bold,
            letterSpacing = 1.sp
        )

        Spacer(modifier = Modifier.height(32.dp))

        Text(text = "${data.speed}", fontSize = 100.sp, fontWeight = FontWeight.Normal, color = NeonWhite, style = textGlowStyle)
        Text("Км/ч", fontSize = 24.sp, color = NeonWhite, fontWeight = FontWeight.Bold)

        if (isRunning) {
            Text("Дистанция: ${runDistance.toInt()} м", color = TextGray, fontSize = 14.sp)
        } else {
            Spacer(modifier = Modifier.height(20.dp))
        }

        Spacer(modifier = Modifier.height(32.dp))
        DynamicsRow("0 - 60 км/ч", time0to60, isRunning, liveTime, ::formatTime)
        Spacer(modifier = Modifier.height(16.dp))
        DynamicsRow("0 - 100 км/ч", time0to100, isRunning, liveTime, ::formatTime)
        Spacer(modifier = Modifier.height(16.dp))
        DynamicsRow("402 метра", time402m, isRunning, liveTime, ::formatTime)

        Spacer(modifier = Modifier.weight(1f))

        OutlinedButton(
            onClick = { showHistory = true },
            modifier = Modifier.fillMaxWidth().height(56.dp),
            shape = RoundedCornerShape(12.dp),
            colors = ButtonDefaults.outlinedButtonColors(containerColor = TrackBg),
            border = BorderStroke(1.dp, Color(0xFF444444))
        ) {
            Text("ИСТОРИЯ ЗАМЕРОВ", fontSize = 16.sp, fontWeight = FontWeight.Bold, color = NeonWhite)
        }
    }

    if (showHistory) {
        AlertDialog(
            onDismissRequest = { showHistory = false },
            containerColor = TrackBg,
            title = { Text("История разгонов", color = NeonWhite, fontWeight = FontWeight.Bold) },
            text = {
                LazyColumn {
                    items(runHistory.size) { index ->
                        val run = runHistory[index]
                        Column(modifier = Modifier.padding(vertical = 8.dp).fillMaxWidth()) {
                            Text(run.date, color = TextGray, fontSize = 12.sp)
                            Spacer(Modifier.height(4.dp))
                            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                                Text("0-60: ${formatTime(run.t60)}", color = NeonWhite)
                                Text("0-100: ${formatTime(run.t100)}", color = NeonWhite)
                            }
                            Text("402м: ${formatTime(run.t402)}", color = StatusGreen, fontWeight = FontWeight.Bold)
                            Divider(color = Color(0xFF333333), modifier = Modifier.padding(top = 8.dp))
                        }
                    }
                    if (runHistory.isEmpty()) {
                        item { Text("Нет сохраненных замеров", color = TextGray) }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showHistory = false }) {
                    Text("ЗАКРЫТЬ", color = TextGray)
                }
            },
            dismissButton = {
                TextButton(onClick = {
                    runHistory = emptyList()
                    saveHistory(emptyList())
                }) {
                    Text("ОЧИСТИТЬ", color = BoostRed)
                }
            }
        )
    }
}

@Composable
fun DynamicsRow(title: String, finalTime: Long?, isRunning: Boolean, liveTime: Long, formatFunc: (Long?) -> String) {
    val state = when {
        finalTime != null -> MeasurerState.DONE
        isRunning -> MeasurerState.MEASURING
        else -> MeasurerState.IDLE
    }

    Card(
        modifier = Modifier.fillMaxWidth().height(80.dp),
        colors = CardDefaults.cardColors(containerColor = TrackBg),
        shape = RoundedCornerShape(16.dp),
        border = BorderStroke(1.dp, if (state == MeasurerState.DONE) StatusGreen else Color.Transparent)
    ) {
        Row(
            modifier = Modifier.fillMaxSize().padding(horizontal = 24.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(text = title, color = TextGray, fontSize = 20.sp, fontWeight = FontWeight.Bold)
            val timeText = when (state) {
                MeasurerState.IDLE -> "--.-- сек"
                MeasurerState.MEASURING -> "${formatFunc(liveTime)} сек"
                MeasurerState.DONE -> "${formatFunc(finalTime)} сек"
            }
            val timeColor = if (state == MeasurerState.DONE) StatusGreen else NeonWhite
            Text(text = timeText, color = timeColor, fontSize = 24.sp, fontWeight = FontWeight.Bold, fontFamily = FontFamily.Monospace)
        }
    }
}
