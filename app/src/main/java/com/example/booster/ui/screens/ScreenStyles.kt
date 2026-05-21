package com.example.booster.ui.screens

import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// ─── Palette (AMOLED-optimised, Galaxy S25 Ultra) ────────────────────────────
val DarkBg      = Color(0xFF090909)   // true AMOLED black
val NeonWhite   = Color(0xFFEEEEEE)   // primary text (slightly off-white)
val TrackBg     = Color(0xFF1C1C1E)   // cards / elevated surfaces / inputs
val TextGray    = Color(0xFF8E8E93)   // secondary labels (iOS-style)
val StatusGreen = Color(0xFF30D158)   // OK / connected / normal mode
val BoostBlue   = Color(0xFF0A84FF)   // primary action / vacuum / idle boost
val BoostGreen  = Color(0xFF30D158)   // positive boost / speed
val BoostRed    = Color(0xFFFF453A)   // error / danger / hard limp
val AccentAmber = Color(0xFFFFD60A)   // warning / soft limp / stale data

enum class MeasurerState { IDLE, MEASURING, DONE }

const val SoftLimpBoostBar = 1.05f
const val HardLimpBoostBar = 1.15f

@Composable
fun BoostCanvasGauge(boost: Float, targetBoost: Float) {
    val minBoost = -1.0f
    val maxBoost = 1.5f
    val range = maxBoost - minBoost
    val progress = ((boost - minBoost) / range).coerceIn(0f, 1f)
    val zeroProgress = ((0f - minBoost) / range).coerceIn(0f, 1f)
    val targetProgress = ((targetBoost - minBoost) / range).coerceIn(0f, 1f)
    val startAngle = 145f
    val sweepAngle = 250f

    Canvas(modifier = Modifier.fillMaxSize()) {
        val strokeWidth = 16.dp.toPx()
        val padding = 32.dp.toPx()
        val arcSize = Size(size.width - padding * 2, size.width - padding * 2)
        val arcOffset = Offset(padding, (size.height - arcSize.height) / 2)

        // Track background
        drawArc(
            color = TrackBg,
            startAngle = startAngle,
            sweepAngle = sweepAngle,
            useCenter = false,
            style = Stroke(width = strokeWidth, cap = StrokeCap.Butt),
            size = arcSize,
            topLeft = arcOffset
        )

        // Negative boost — blue with glow
        val blueSweep = sweepAngle * minOf(progress, zeroProgress)
        if (blueSweep > 0.5f) {
            drawArc(
                color = BoostBlue.copy(alpha = 0.18f),
                startAngle = startAngle,
                sweepAngle = blueSweep,
                useCenter = false,
                style = Stroke(width = strokeWidth + 10.dp.toPx(), cap = StrokeCap.Butt),
                size = arcSize,
                topLeft = arcOffset
            )
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

        // Positive boost — green with glow
        if (progress > zeroProgress) {
            val greenStart = startAngle + (sweepAngle * zeroProgress)
            val greenSweep = sweepAngle * (progress - zeroProgress)
            if (greenSweep > 0.5f) {
                drawArc(
                    color = BoostGreen.copy(alpha = 0.20f),
                    startAngle = greenStart,
                    sweepAngle = greenSweep,
                    useCenter = false,
                    style = Stroke(width = strokeWidth + 10.dp.toPx(), cap = StrokeCap.Butt),
                    size = arcSize,
                    topLeft = arcOffset
                )
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

        // Target indicator — glow halo + crisp dot
        val targetAngle = startAngle + sweepAngle * targetProgress
        val indicatorRadius = arcSize.width / 2f
        val indicatorCenter = Offset(
            x = arcOffset.x + arcSize.width / 2f,
            y = arcOffset.y + arcSize.height / 2f
        )
        val targetRadians = Math.toRadians(targetAngle.toDouble())
        val targetPoint = Offset(
            x = indicatorCenter.x + (indicatorRadius * kotlin.math.cos(targetRadians)).toFloat(),
            y = indicatorCenter.y + (indicatorRadius * kotlin.math.sin(targetRadians)).toFloat()
        )
        drawCircle(color = NeonWhite.copy(alpha = 0.25f), radius = 7.dp.toPx(), center = targetPoint)
        drawCircle(color = NeonWhite, radius = 4.dp.toPx(), center = targetPoint)
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
    shape: Shape = RoundedCornerShape(16.dp)
) {
    Column(
        modifier = modifier
            .background(TrackBg, shape)
            .border(width = 1.dp, color = accent.copy(alpha = 0.4f), shape = shape)
            .padding(horizontal = 14.dp, vertical = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            text = title,
            color = titleColor,
            fontSize = 11.sp,
            fontWeight = FontWeight.Medium,
            letterSpacing = 0.5.sp
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
    val infiniteTransition = rememberInfiniteTransition(label = "dotPulse")
    val pulseAlpha by infiniteTransition.animateFloat(
        initialValue = 0.55f,
        targetValue = 1.0f,
        animationSpec = infiniteRepeatable(
            animation = tween(900, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Reverse
        ),
        label = "alpha"
    )
    Box(
        modifier = modifier.background(
            color = if (connected) StatusGreen.copy(alpha = pulseAlpha) else BoostRed,
            shape = CircleShape
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
                fontWeight = FontWeight.Medium,
                letterSpacing = 0.5.sp
            )
            Text(
                text = valueLabel,
                color = NeonWhite,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace
            )
        }
        Spacer(modifier = Modifier.height(8.dp))
        Box(modifier = Modifier.fillMaxWidth().height(5.dp)) {
            Canvas(modifier = Modifier.fillMaxSize()) {
                val cr = CornerRadius(3.dp.toPx())
                drawRoundRect(color = TrackBg, size = Size(size.width, size.height), cornerRadius = cr)
                if (progress > 0.01f) {
                    drawRoundRect(
                        brush = Brush.horizontalGradient(listOf(BoostBlue.copy(alpha = 0.8f), BoostGreen)),
                        size = Size(size.width * progress, size.height),
                        cornerRadius = cr
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
        Box(modifier = Modifier.fillMaxWidth().height(24.dp)) {
            Canvas(modifier = Modifier.fillMaxSize()) {
                val cr = CornerRadius(8.dp.toPx())
                drawRoundRect(color = TrackBg, size = Size(size.width, size.height), cornerRadius = cr)
                if (progress > 0.01f) {
                    drawRoundRect(
                        brush = Brush.horizontalGradient(listOf(NeonWhite.copy(alpha = 0.3f), NeonWhite)),
                        size = Size(size.width * progress, size.height),
                        cornerRadius = cr
                    )
                }
            }
        }
        Spacer(modifier = Modifier.height(8.dp))
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            labels.forEach { text ->
                Text(text = text, color = NeonWhite.copy(alpha = 0.9f), fontSize = 14.sp, fontWeight = FontWeight.Bold, fontFamily = FontFamily.Monospace)
            }
        }
        if (label.isNotEmpty()) {
            Spacer(modifier = Modifier.height(6.dp))
            Text(
                text = label,
                color = TextGray,
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.align(Alignment.CenterHorizontally),
                letterSpacing = 1.sp
            )
        }
    }
}
