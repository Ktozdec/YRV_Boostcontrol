package com.example.booster.ui.screens

import androidx.compose.foundation.Canvas
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
import androidx.compose.foundation.layout.padding
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable

val DarkBg = Color(0xFF101010)
val NeonWhite = Color(0xFFFFFFFF)
val TrackBg = Color(0xFF222222)
val TextGray = Color(0xFF888888)
val StatusGreen = Color(0xFF4CAF50)
val BoostBlue = Color(0xFF00BFFF)
val BoostGreen = Color(0xFF00E676)
val BoostRed = Color(0xFFFF1744)

enum class MeasurerState { IDLE, MEASURING, DONE }

const val SoftLimpBoostBar = 1.05f
const val HardLimpBoostBar = 1.15f

@Composable
fun BoostCanvasGauge(
    boost: Float,
    targetBoost: Float
) {
    val minBoost = -1.0f
    val maxBoost = 1.5f
    val range = maxBoost - minBoost

    val progress = ((boost - minBoost) / range).coerceIn(0f, 1f)
    val zeroProgress = ((0f - minBoost) / range).coerceIn(0f, 1f)
    val targetProgress = ((targetBoost - minBoost) / range).coerceIn(0f, 1f)

    val startAngle = 145f
    val sweepAngle = 250f

    Canvas(modifier = Modifier.fillMaxSize()) {
        val strokeWidth = 22.dp.toPx()
        val padding = 30.dp.toPx()

        val arcSize = Size(size.width - padding * 2, size.width - padding * 2)
        val arcOffset = Offset(padding, (size.height - arcSize.height) / 2)

        drawArc(
            color = TrackBg,
            startAngle = startAngle,
            sweepAngle = sweepAngle,
            useCenter = false,
            style = Stroke(width = strokeWidth, cap = StrokeCap.Butt),
            size = arcSize,
            topLeft = arcOffset
        )

        val blueSweep = sweepAngle * minOf(progress, zeroProgress)
        if (blueSweep > 0) {
            drawArc(
                color = BoostBlue,
                startAngle = startAngle,
                sweepAngle = blueSweep,
                useCenter = false,
                style = Stroke(width = strokeWidth, cap = StrokeCap.Butt),
                size = arcSize,
                topLeft = arcOffset
            )
        }

        if (progress > zeroProgress) {
            val greenStart = startAngle + (sweepAngle * zeroProgress)
            val greenSweep = sweepAngle * (progress - zeroProgress)
            if (greenSweep > 0) {
                drawArc(
                    color = BoostGreen,
                    startAngle = greenStart,
                    sweepAngle = greenSweep,
                    useCenter = false,
                    style = Stroke(width = strokeWidth, cap = StrokeCap.Butt),
                    size = arcSize,
                    topLeft = arcOffset
                )
            }
        }

        val targetAngle = startAngle + sweepAngle * targetProgress
        val indicatorRadius = (arcSize.width / 2f) + 1.dp.toPx()
        val indicatorCenter = Offset(
            x = arcOffset.x + arcSize.width / 2f,
            y = arcOffset.y + arcSize.height / 2f
        )
        val targetRadians = Math.toRadians(targetAngle.toDouble())
        val targetPoint = Offset(
            x = indicatorCenter.x + (indicatorRadius * kotlin.math.cos(targetRadians)).toFloat(),
            y = indicatorCenter.y + (indicatorRadius * kotlin.math.sin(targetRadians)).toFloat()
        )
        drawCircle(
            color = NeonWhite,
            radius = 5.dp.toPx(),
            center = targetPoint
        )
    }
}

@Composable
fun MetricTile(
    title: String,
    value: String,
    modifier: Modifier = Modifier,
    valueColor: Color = NeonWhite,
    titleColor: Color = TextGray,
    accent: Color = TrackBg,
    shape: Shape = RoundedCornerShape(18.dp)
) {
    Column(
        modifier = modifier
            .background(Color(0xFF181818), shape)
            .border(width = 1.dp, color = accent, shape = shape)
            .padding(horizontal = 14.dp, vertical = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            text = title,
            color = titleColor,
            fontSize = 12.sp,
            fontWeight = FontWeight.Medium
        )
        Spacer(modifier = Modifier.height(6.dp))
        Text(
            text = value,
            color = valueColor,
            fontSize = 18.sp,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace
        )
    }
}

@Composable
fun StatusDot(connected: Boolean, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .background(
                color = if (connected) StatusGreen else BoostRed,
                shape = RoundedCornerShape(50)
            )
    )
}

@Composable
fun CompactProgressGauge(
    label: String,
    valueLabel: String,
    value: Float,
    maxValue: Float,
    modifier: Modifier = Modifier
) {
    val progress = (value / maxValue).coerceIn(0f, 1f)
    Column(modifier = modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = label,
                color = TextGray,
                fontSize = 12.sp,
                fontWeight = FontWeight.Medium
            )
            Text(
                text = valueLabel,
                color = NeonWhite,
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace
            )
        }
        Spacer(modifier = Modifier.height(8.dp))
        Box(modifier = Modifier.fillMaxWidth().height(16.dp)) {
            Canvas(modifier = Modifier.fillMaxSize()) {
                val cornerRadius = CornerRadius(12.dp.toPx(), 12.dp.toPx())
                drawRoundRect(
                    color = TrackBg,
                    size = Size(size.width, size.height),
                    cornerRadius = cornerRadius
                )
                if (progress > 0.01f) {
                    val fillGradient = Brush.horizontalGradient(
                        colors = listOf(BoostBlue.copy(alpha = 0.7f), BoostGreen)
                    )
                    drawRoundRect(
                        brush = fillGradient,
                        size = Size(size.width * progress, size.height),
                        cornerRadius = cornerRadius
                    )
                }
            }
        }
    }
}

@Composable
fun LinearGradientGauge(label: String, value: Float, maxValue: Float, labels: List<String>) {
    val progress = (value / maxValue).coerceIn(0f, 1f)
    Column(modifier = Modifier.fillMaxWidth()) {
        Box(modifier = Modifier.fillMaxWidth().height(28.dp)) {
            Canvas(modifier = Modifier.fillMaxSize()) {
                val cornerRadius = CornerRadius(12.dp.toPx(), 12.dp.toPx())
                drawRoundRect(
                    color = TrackBg,
                    size = Size(size.width, size.height),
                    cornerRadius = cornerRadius
                )
                if (progress > 0.01f) {
                    val fillGradient = Brush.horizontalGradient(
                        colors = listOf(NeonWhite.copy(alpha = 0.4f), NeonWhite)
                    )
                    drawRoundRect(
                        brush = fillGradient,
                        size = Size(size.width * progress, size.height),
                        cornerRadius = cornerRadius
                    )
                }
            }
        }
        Spacer(modifier = Modifier.height(8.dp))
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            labels.forEach { text ->
                Text(
                    text = text,
                    color = NeonWhite.copy(alpha = 0.9f),
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
            }
        }
        if (label.isNotEmpty()) {
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                text = label,
                color = TextGray,
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.align(Alignment.CenterHorizontally),
                letterSpacing = 1.5.sp
            )
        }
    }
}
