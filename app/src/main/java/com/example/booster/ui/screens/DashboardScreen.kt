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
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
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

    val animatedBoost by animateFloatAsState(
        targetValue = data.boost,
        animationSpec = tween(100, easing = LinearEasing),
        label = "boostAnim"
    )
    val animatedTps by animateFloatAsState(
        targetValue = data.tps.toFloat(),
        animationSpec = tween(100, easing = LinearEasing),
        label = "tpsAnim"
    )
    val modeLabel = when (data.mode) {
        0 -> "NORMAL"
        1 -> "SOFT LIMP"
        2 -> "HARD LIMP"
        else -> "UNKNOWN"
    }
    val modeColor = when (data.mode) {
        0 -> StatusGreen
        1 -> Color(0xFFFFC107)
        2 -> BoostRed
        else -> TextGray
    }
    val textGlowStyle = TextStyle(
        shadow = Shadow(color = NeonWhite.copy(alpha = 0.5f), blurRadius = 8f),
        fontFamily = FontFamily.Monospace
    )

    Column(
        modifier = Modifier.fillMaxSize().background(DarkBg).padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
            if (status != "Подключено") {
                Button(
                    onClick = {
                        if (hasBlePermissions(context)) {
                            viewModel.connect()
                        } else {
                            blePermissionLauncher.launch(requiredBlePermissions())
                        }
                    },
                    colors = ButtonDefaults.buttonColors(containerColor = NeonWhite),
                    modifier = Modifier.height(32.dp)
                ) {
                    Text(
                        if (status == "Отключено" || status.contains("Обрыв")) "ПОДКЛЮЧИТЬСЯ" else "ПОИСК...",
                        color = DarkBg,
                        fontWeight = FontWeight.Black
                    )
                }
            } else {
                Text(text = "Подключено", fontSize = 14.sp, fontWeight = FontWeight.Bold, color = StatusGreen)
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("База ШИМ: ${data.baseDuty.toInt()}%", color = TextGray, fontSize = 14.sp, fontWeight = FontWeight.Medium)
            Text("Выход ШИМ: ${data.currentDuty.toInt()}%", color = BoostBlue, fontSize = 14.sp, fontWeight = FontWeight.Bold)
        }

        Spacer(modifier = Modifier.height(8.dp))

        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("Режим ЭБУ: $modeLabel", color = modeColor, fontSize = 13.sp, fontWeight = FontWeight.Bold)
            Text("Цель: ${String.format(Locale.US, "%.2f", data.targetBoost)} bar", color = TextGray, fontSize = 13.sp)
        }

        Spacer(modifier = Modifier.height(16.dp))

        Box(contentAlignment = Alignment.Center, modifier = Modifier.fillMaxWidth().height(260.dp)) {
            BoostCanvasGauge(boost = animatedBoost, targetBoost = data.targetBoost)
            Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.offset(y = (-10).dp)) {
                Text(
                    text = String.format(Locale.US, "%.2f", data.boost),
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

        Spacer(modifier = Modifier.height(32.dp))

        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(text = "${data.speed}", fontSize = 88.sp, fontWeight = FontWeight.Normal, color = NeonWhite, style = textGlowStyle)
                Text(text = "Км/ч", fontSize = 18.sp, color = NeonWhite, fontWeight = FontWeight.Medium)
            }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(text = "${data.rpm}", fontSize = 56.sp, fontWeight = FontWeight.Normal, color = NeonWhite, style = textGlowStyle)
                Text(text = "Об/мин", fontSize = 18.sp, color = TextGray, fontWeight = FontWeight.Medium)
            }
        }

        Spacer(modifier = Modifier.weight(1f))
        LinearGradientGauge(
            label = "ДРОССЕЛЬ: ${data.tps}%",
            value = animatedTps,
            maxValue = 100f,
            labels = listOf("0%", "50%", "100%")
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
    }
}
