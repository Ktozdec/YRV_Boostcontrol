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
import androidx.compose.foundation.border
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.draw.clip
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
            Toast.makeText(context, "Геолокация понадобится для GPS-калибровки скорости", Toast.LENGTH_SHORT).show()
        }
    }

    val blePermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) {
        if (hasBlePermissions(context)) {
            viewModel.connect()
        } else {
            Toast.makeText(context, "Для подключения к ESP32 нужны разрешения Bluetooth", Toast.LENGTH_SHORT).show()
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
        1 -> AccentAmber
        2 -> BoostRed
        else -> TextGray
    }

    val textGlowStyle = TextStyle(
        shadow = Shadow(color = NeonWhite.copy(alpha = 0.4f), blurRadius = 12f),
        fontFamily = FontFamily.Monospace
    )
    val telemetryAgeMs = if (data.telemetryUpdatedAtMillis == 0L) Long.MAX_VALUE
                         else freshnessNow - data.telemetryUpdatedAtMillis
    val isTelemetryStale = isConnected && telemetryAgeMs > 700L

    LaunchedEffect(data.telemetryUpdatedAtMillis) {
        if (data.telemetryUpdatedAtMillis == 0L) return@LaunchedEffect
        filteredBoost = (data.boost * 0.6f) + (filteredBoost * 0.4f)
        filteredSpeed = if (data.speed < 2f) data.speed.toFloat()
                        else (data.speed * 0.25f) + (filteredSpeed * 0.75f)   // midpoint of original 0.1 and 0.4
        filteredRpm = if (data.rpm < 10f) data.rpm.toFloat()
                      else (data.rpm * 0.275f) + (filteredRpm * 0.725f)   // midpoint of original 0.2 and 0.35
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
            .padding(start = 18.dp, end = 18.dp, top = 16.dp, bottom = 16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // ── Row 1: mode alert (only when not normal) + status dot ────────────
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            if (data.mode != 0) {
                StatusChip(label = modeLabel, color = modeColor)
            }
            Spacer(modifier = Modifier.weight(1f))
            if (isTelemetryStale) {
                StatusChip(label = "LAG", color = AccentAmber)
            }
            StatusDot(connected = isConnected, modifier = Modifier.size(11.dp))
        }

        Spacer(modifier = Modifier.height(6.dp))

        // ── Row 2: target / FCD / current PWM ────────────────────────────────
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            InfoPair(label = "TARGET", value = String.format(Locale.US, "%.2f", data.targetBoost))
            InfoPair(label = "FCD", value = String.format(Locale.US, "%.2f", data.limitBoostBar))
            InfoPair(label = "ШИМ", value = "${data.currentDuty.toInt()}%")
        }

        Spacer(modifier = Modifier.height(24.dp))

        // ── Boost gauge ───────────────────────────────────────────────────────
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier
                .fillMaxWidth()
                .height(240.dp)
        ) {
            BoostCanvasGauge(boost = animatedBoost, targetBoost = data.targetBoost)
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier.offset(y = (-8).dp)
            ) {
                Text(
                    text = String.format(Locale.US, "%.2f", animatedBoost),
                    fontSize = 76.sp,
                    fontWeight = FontWeight.Normal,
                    color = NeonWhite,
                    style = textGlowStyle
                )
                Text(text = "Bar", fontSize = 18.sp, color = NeonWhite, fontWeight = FontWeight.Medium)
            }
        }

        // ── Min / Max row (now outside Box — no offset hack) ──────────────────
        Spacer(modifier = Modifier.height(4.dp))
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 28.dp),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    String.format(Locale.US, "%.2f", data.minBoost),
                    fontSize = 18.sp,
                    color = NeonWhite,
                    style = textGlowStyle
                )
                Text("МИН", fontSize = 11.sp, color = TextGray, letterSpacing = 1.sp)
            }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    String.format(Locale.US, "%.2f", data.maxBoost),
                    fontSize = 18.sp,
                    color = NeonWhite,
                    style = textGlowStyle
                )
                Text("МАКС", fontSize = 11.sp, color = TextGray, letterSpacing = 1.sp)
            }
        }

        Spacer(modifier = Modifier.height(20.dp))

        // ── Speed / RPM ───────────────────────────────────────────────────────
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.Bottom
        ) {
            Column(modifier = Modifier.weight(1f), horizontalAlignment = Alignment.Start) {
                Text("Скорость", color = TextGray, fontSize = 11.sp, fontWeight = FontWeight.Medium, letterSpacing = 0.5.sp)
                Text(
                    text = "$displaySpeed",
                    color = NeonWhite,
                    fontSize = 64.sp,
                    fontWeight = FontWeight.Normal,
                    style = textGlowStyle,
                    maxLines = 1
                )
                Text("км/ч", color = NeonWhite, fontSize = 14.sp, fontWeight = FontWeight.Medium)
            }
            Spacer(modifier = Modifier.size(18.dp))
            Column(modifier = Modifier.weight(1f), horizontalAlignment = Alignment.End) {
                Text("Обороты", color = TextGray, fontSize = 11.sp, fontWeight = FontWeight.Medium, letterSpacing = 0.5.sp)
                Text(
                    text = "$displayRpm",
                    color = NeonWhite,
                    fontSize = 50.sp,
                    fontWeight = FontWeight.Normal,
                    style = textGlowStyle,
                    maxLines = 1
                )
                Text("об/мин", color = TextGray, fontSize = 14.sp, fontWeight = FontWeight.Medium)
            }
        }

        Spacer(modifier = Modifier.height(20.dp))

        // ── TPS progress ──────────────────────────────────────────────────────
        CompactProgressGauge(
            label = "Педаль газа",
            valueLabel = "${data.tps}%",
            value = animatedTps,
            maxValue = 100f,
            modifier = Modifier.fillMaxWidth()
        )

        Spacer(modifier = Modifier.height(24.dp))

        OutlinedButton(
            onClick = { viewModel.sendCommand("RESET") },
            modifier = Modifier.fillMaxWidth().height(52.dp),
            colors = ButtonDefaults.outlinedButtonColors(containerColor = DarkBg),
            border = BorderStroke(1.dp, Color(0xFF2C2C2E)),
            shape = RoundedCornerShape(12.dp)
        ) {
            Text(
                "СБРОС ПИКОВ",
                fontSize = 14.sp,
                fontWeight = FontWeight.SemiBold,
                color = TextGray,
                letterSpacing = 1.5.sp
            )
        }
        Spacer(modifier = Modifier.height(12.dp))
    }
}

@Composable
private fun InfoPair(label: String, value: String) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        Text(
            text = label,
            color = TextGray,
            fontSize = 10.sp,
            fontWeight = FontWeight.Medium,
            letterSpacing = 0.8.sp
        )
        Text(
            text = value,
            color = NeonWhite.copy(alpha = 0.85f),
            fontSize = 12.sp,
            fontWeight = FontWeight.SemiBold,
            fontFamily = FontFamily.Monospace
        )
    }
}

@Composable
private fun StatusChip(label: String, color: Color) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(20.dp))
            .background(color.copy(alpha = 0.10f))
            .border(1.dp, color.copy(alpha = 0.30f), RoundedCornerShape(20.dp))
            .padding(horizontal = 9.dp, vertical = 4.dp)
    ) {
        Text(
            text = label,
            color = color,
            fontSize = 11.sp,
            fontWeight = FontWeight.SemiBold,
            letterSpacing = 0.3.sp
        )
    }
}
