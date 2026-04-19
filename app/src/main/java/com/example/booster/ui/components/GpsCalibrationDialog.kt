package com.example.booster.ui.components

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.os.Looper
import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.Row
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.example.booster.data.TelemetryData
import com.example.booster.ui.screens.NeonWhite
import com.example.booster.ui.screens.StatusGreen
import com.example.booster.ui.screens.TextGray
import com.example.booster.ui.screens.TrackBg
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority
import java.util.Locale

@Composable
fun GpsCalibrationDialog(
    context: Context,
    telemetry: TelemetryData,
    onApply: (Float) -> Unit,
    onDismiss: () -> Unit
) {
    var gpsSpeed by remember { mutableFloatStateOf(0f) }

    DisposableEffect(Unit) {
        val fusedLocationClient = LocationServices.getFusedLocationProviderClient(context)
        var lastLocation: Location? = null
        var lastTimeMs = 0L

        val locationRequest = LocationRequest.Builder(Priority.PRIORITY_HIGH_ACCURACY, 500L)
            .setMinUpdateIntervalMillis(250L)
            .build()

        val locationCallback = object : com.google.android.gms.location.LocationCallback() {
            override fun onLocationResult(locationResult: com.google.android.gms.location.LocationResult) {
                for (location in locationResult.locations) {
                    if (location.accuracy > 25f) continue

                    gpsSpeed = when {
                        location.hasSpeed() -> location.speed * 3.6f
                        lastLocation != null -> {
                            val dt = (location.time - lastTimeMs) / 1000f
                            if (dt > 0f) {
                                (location.distanceTo(lastLocation!!) / dt) * 3.6f
                            } else {
                                gpsSpeed
                            }
                        }
                        else -> gpsSpeed
                    }

                    lastLocation = location
                    lastTimeMs = location.time
                }
            }
        }

        if (ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED) {
            fusedLocationClient.requestLocationUpdates(locationRequest, locationCallback, Looper.getMainLooper())
        } else {
            Toast.makeText(context, "Нет доступа к геолокации", Toast.LENGTH_SHORT).show()
        }

        onDispose { fusedLocationClient.removeLocationUpdates(locationCallback) }
    }

    val ratio = if (gpsSpeed > 5f && telemetry.speed > 0) {
        telemetry.speed.toFloat() / gpsSpeed
    } else {
        1f
    }
    val newGpsVp = telemetry.vssPulses * ratio

    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = TrackBg,
        title = { Text("GPS Калибровка", color = NeonWhite, fontWeight = FontWeight.Bold) },
        text = {
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(
                    "Держите ровную скорость > 40 км/ч",
                    color = TextGray,
                    fontSize = 12.sp,
                    textAlign = TextAlign.Center
                )
                Spacer(modifier = Modifier.height(24.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceAround
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text("ЭБУ", color = TextGray)
                        Text(
                            "${telemetry.speed}",
                            color = NeonWhite,
                            fontSize = 40.sp,
                            fontWeight = FontWeight.Bold
                        )
                    }
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text("GPS", color = StatusGreen)
                        Text(
                            String.format(Locale.US, "%.0f", gpsSpeed),
                            color = StatusGreen,
                            fontSize = 40.sp,
                            fontWeight = FontWeight.Bold
                        )
                    }
                }
                Spacer(modifier = Modifier.height(24.dp))
                Text(
                    "Текущий vP: ${String.format(Locale.US, "%.2f", telemetry.vssPulses)}",
                    color = TextGray
                )
                Text(
                    "Расчетный vP: ${String.format(Locale.US, "%.2f", newGpsVp)}",
                    color = if (gpsSpeed > 5f) NeonWhite else TextGray,
                    fontWeight = FontWeight.Bold
                )
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onApply(newGpsVp) },
                enabled = telemetry.vssPulses > 0f && gpsSpeed > 5f
            ) {
                Text(
                    "ПРИМЕНИТЬ",
                    color = if (gpsSpeed > 5f) StatusGreen else TextGray,
                    fontWeight = FontWeight.Bold
                )
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text("ОТМЕНА", color = TextGray)
            }
        }
    )
}
