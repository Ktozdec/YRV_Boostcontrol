package com.example.booster.ui.screens

import android.Manifest
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
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
import com.example.booster.ui.permissions.hasBlePermissions
import com.example.booster.ui.permissions.hasLocationPermission
import com.example.booster.ui.permissions.requiredBlePermissions
import com.example.booster.viewmodel.BoosterViewModel
import java.util.Locale

@Composable
fun DashboardScreen(viewModel: BoosterViewModel) {
    val status by viewModel.connectionStatus.collectAsStateWithLifecycle()
    val data by viewModel.telemetry.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val prefsRepository = remember { AppPreferencesRepository(context) }
    val shouldRequestLocationOnLaunch = remember {
        !prefsRepository.hasRequestedLocationPermission() && !hasLocationPermission(context)
    }

    val locationPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (!granted) {
            Toast.makeText(
                context,
                "Геолокация понадобится для GPS-калибровки скорости",
                Toast.LENGTH_SHORT
            ).show()
        }
    }

    val blePermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) {
        if (hasBlePermissions(context)) {
            viewModel.connect()
        } else {
            Toast.makeText(
                context,
                "Для подключения к ESP32 нужны разрешения Bluetooth",
                Toast.LENGTH_SHORT
            ).show()
        }

        if (shouldRequestLocationOnLaunch && !prefsRepository.hasRequestedLocationPermission()) {
            prefsRepository.markLocationPermissionRequested()
            locationPermissionLauncher.launch(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }

    LaunchedEffect(Unit) {
        if (hasBlePermissions(context)) {
            viewModel.connect()
            if (shouldRequestLocationOnLaunch) {
                prefsRepository.markLocationPermissionRequested()
                locationPermissionLauncher.launch(Manifest.permission.ACCESS_FINE_LOCATION)
            }
        } else {
            blePermissionLauncher.launch(requiredBlePermissions())
        }
    }

    val animatedTps by animateFloatAsState(
        targetValue = data.tps.toFloat(),
        animationSpec = tween(100, easing = LinearEasing),
        label = "tpsAnim"
    )
    var filteredBoost by remember { mutableFloatStateOf(data.boost) }
    var filteredSpeed by remember { mutableFloatStateOf(data.speed.toFloat()) }
    var filteredRpm by remember { mutableFloatStateOf(data.rpm.toFloat()) }
    val scrollState = rememberScrollState()
    val isConnected = status == "Подключено"
    var freshnessNow by remember { mutableLongStateOf(System.currentTimeMillis()) }
    val modeLabel = when (data.mode) {
        0 -> "NORMAL"
        1 -> "SOFT"
        2 -> "HARD"
        else -> "UNKNOWN"
    }
    val modeColor = when (data.mode) {
        0 -> StatusGreen
        1 -> BoostBlue
        2 -> NeonWhite
        else -> TextGray
    }
    val textGlowStyle = TextStyle(
        shadow = Shadow(color = NeonWhite.copy(alpha = 0.5f), blurRadius = 8f),
        fontFamily = FontFamily.Monospace
    )
    val telemetryAgeMs = if (data.telemetryUpdatedAtMillis == 0L) Long.MAX_VALUE else freshnessNow - data.telemetryUpdatedAtMillis
    val isTelemetryStale = isConnected && telemetryAgeMs > 700L

    LaunchedEffect(data.telemetryUpdatedAtMillis) {
        if (data.telemetryUpdatedAtMillis == 0L) return@LaunchedEffect

        val rawBoost = data.boost
        val rawSpeed = data.speed.toFloat()
        val rawRpm = data.rpm.toFloat()

        filteredBoost = (rawBoost * 0.6f) + (filteredBoost * 0.4f)
        filteredSpeed = if (rawSpeed < 2f) {
            rawSpeed
        } else {
            (rawSpeed * 0.1f) + (filteredSpeed * 0.9f)
        }
        filteredRpm = if (rawRpm < 10f) {
            rawRpm
        } else {
            (rawRpm * 0.2f) + (filteredRpm * 0.8f)
        }
    }

    val animatedBoost by animateFloatAsState(
        targetValue = filteredBoost,
        animationSpec = tween(120, easing = LinearEasing),
        label = "boostAnim"
    )
    val displaySpeed = filteredSpeed.toInt()
    val displayRpm = filteredRpm.toInt()

    LaunchedEffect(isConnected, data.telemetryUpdatedAtMillis) {
        while (isConnected) {
            freshnessNow = System.currentTimeMillis()
            kotlinx.coroutines.delay(250)
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(DarkBg)
            .verticalScroll(scrollState)
            .statusBarsPadding()
            .navigationBarsPadding()
            .padding(start = 18.dp, end = 18.dp, top = 18.dp, bottom = 16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = "База ШИМ ${data.baseDuty.toInt()}%",
                color = TextGray.copy(alpha = 0.58f),
                fontSize = 14.sp,
                fontWeight = FontWeight.Medium
            )
            Text(
                text = "ШИМ ${data.currentDuty.toInt()}%",
                color = BoostBlue.copy(alpha = 0.72f),
                fontSize = 14.sp,
                fontWeight = FontWeight.SemiBold
            )
            Spacer(modifier = Modifier.weight(1f))
            StatusDot(
                connected = isConnected,
                modifier = Modifier.size(10.dp)
            )
        }

        if (isTelemetryStale) {
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                text = "ТЕЛЕМЕТРИЯ ЗАДЕРЖАЛАСЬ",
                color = Color(0xFFFFC107),
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold
            )
        }

        Spacer(modifier = Modifier.height(14.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(14.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = "Режим",
                color = TextGray.copy(alpha = 0.58f),
                fontSize = 12.sp,
                fontWeight = FontWeight.Medium
            )
            Text(
                text = modeLabel,
                color = modeColor.copy(alpha = 0.78f),
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold
            )
            Spacer(modifier = Modifier.widthIn(min = 6.dp))
            Text(
                text = "Target",
                color = TextGray.copy(alpha = 0.58f),
                fontSize = 12.sp,
                fontWeight = FontWeight.Medium
            )
            Text(
                text = "${String.format(Locale.US, "%.2f", data.targetBoost)} bar",
                color = NeonWhite.copy(alpha = 0.74f),
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold
            )
        }

        Spacer(modifier = Modifier.height(28.dp))

        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier
                .fillMaxWidth()
                .height(260.dp)
        ) {
            BoostCanvasGauge(boost = animatedBoost, targetBoost = data.targetBoost)
            Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.offset(y = (-10).dp)) {
                Text(
                    text = String.format(Locale.US, "%.2f", animatedBoost),
                    fontSize = 76.sp,
                    fontWeight = FontWeight.Normal,
                    color = NeonWhite,
                    style = textGlowStyle
                )
                Text(text = "Bar", fontSize = 18.sp, color = NeonWhite, fontWeight = FontWeight.Medium)
            }
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 24.dp).align(Alignment.BottomCenter).offset(y = 30.dp),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(String.format(Locale.US, "%.1f", data.minBoost), fontSize = 18.sp, color = NeonWhite, style = textGlowStyle)
                    Text("МИН", fontSize = 12.sp, color = TextGray)
                }
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(String.format(Locale.US, "%.1f", data.maxBoost), fontSize = 18.sp, color = NeonWhite, style = textGlowStyle)
                    Text("МАКС", fontSize = 12.sp, color = TextGray)
                }
            }
        }

        Spacer(modifier = Modifier.height(18.dp))

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.Bottom
        ) {
            Column(
                modifier = Modifier.weight(1f),
                horizontalAlignment = Alignment.Start
            ) {
                Text(
                    text = "Скорость",
                    color = TextGray,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Medium
                )
                Text(
                    text = "$displaySpeed",
                    color = NeonWhite,
                    fontSize = 64.sp,
                    fontWeight = FontWeight.Normal,
                    style = textGlowStyle,
                    maxLines = 1
                )
                Text(
                    text = "км/ч",
                    color = NeonWhite,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Medium
                )
            }
            Spacer(modifier = Modifier.size(18.dp))
            Column(
                modifier = Modifier.weight(1f),
                horizontalAlignment = Alignment.End
            ) {
                Text(
                    text = "Обороты",
                    color = TextGray,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Medium
                )
                Text(
                    text = "$displayRpm",
                    color = NeonWhite,
                    fontSize = 50.sp,
                    fontWeight = FontWeight.Normal,
                    style = textGlowStyle,
                    maxLines = 1
                )
                Text(
                    text = "об/мин",
                    color = TextGray,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Medium
                )
            }
        }

        Spacer(modifier = Modifier.height(18.dp))

        CompactProgressGauge(
            label = "Педаль",
            valueLabel = "${data.tps}%",
            value = animatedTps,
            maxValue = 100f,
            modifier = Modifier.fillMaxWidth()
        )
        Spacer(modifier = Modifier.height(24.dp))

        OutlinedButton(
            onClick = { viewModel.sendCommand("RESET") },
            modifier = Modifier.fillMaxWidth().height(56.dp),
            colors = ButtonDefaults.outlinedButtonColors(containerColor = DarkBg),
            border = BorderStroke(1.dp, Color(0xFF444444))
        ) {
            Text("СБРОС ПИКОВ", fontSize = 16.sp, fontWeight = FontWeight.Bold, color = TextGray, letterSpacing = 1.sp)
        }
        Spacer(modifier = Modifier.height(12.dp))
    }
}
