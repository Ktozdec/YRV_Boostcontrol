package com.example.booster.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.booster.viewmodel.BoosterViewModel

@Composable
fun DiagnosticScreen(viewModel: BoosterViewModel) {
    val data by viewModel.telemetry.collectAsStateWithLifecycle()
    val log by viewModel.diagnosticLog.collectAsStateWithLifecycle()
    var command by remember { mutableStateOf("") }

    LazyColumn(
        modifier = Modifier.fillMaxSize().background(DarkBg).padding(horizontal = 12.dp),
        contentPadding = PaddingValues(top = 16.dp, bottom = 90.dp)
    ) {
        item {
            Text(
                "Диагностика",
                fontSize = 24.sp,
                fontWeight = FontWeight.Black,
                color = NeonWhite,
                modifier = Modifier.padding(bottom = 16.dp)
            )
        }

        item {
            Card(
                modifier = Modifier.fillMaxWidth().padding(bottom = 12.dp),
                colors = CardDefaults.cardColors(containerColor = TrackBg),
                shape = RoundedCornerShape(12.dp)
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text("K-Line сниффер", color = NeonWhite, fontSize = 16.sp, fontWeight = FontWeight.Bold)
                    Spacer(Modifier.height(10.dp))
                    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text("Байты ${data.klineBytes}", color = TextGray, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                        Text("Кадры ${data.klineFrames}", color = TextGray, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                        Text("Переп. ${data.klineOverflows}", color = TextGray, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                    }
                    Spacer(Modifier.height(12.dp))
                    Text(
                        if (data.klineLastHex.isBlank()) "Ожидание кадров от Launch или ЭБУ" else data.klineLastHex,
                        color = NeonWhite,
                        fontSize = 13.sp,
                        fontFamily = FontFamily.Monospace
                    )
                }
            }
        }

        item {
            Card(
                modifier = Modifier.fillMaxWidth().padding(bottom = 12.dp),
                colors = CardDefaults.cardColors(containerColor = TrackBg),
                shape = RoundedCornerShape(12.dp)
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text("Ручной запрос", color = NeonWhite, fontSize = 16.sp, fontWeight = FontWeight.Bold)
                    Spacer(Modifier.height(10.dp))
                    OutlinedTextField(
                        value = command,
                        onValueChange = { command = it },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true,
                        label = { Text("HEX команда", color = TextGray) },
                        placeholder = { Text("8310F0210C01", color = TextGray) },
                        keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Characters),
                        colors = OutlinedTextFieldDefaults.colors(
                            focusedTextColor = NeonWhite,
                            unfocusedTextColor = NeonWhite,
                            focusedBorderColor = BoostBlue,
                            unfocusedBorderColor = TextGray,
                            cursorColor = BoostBlue
                        )
                    )
                    Spacer(Modifier.height(12.dp))
                    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(
                            onClick = { viewModel.sendKLineHexCommand(command) },
                            modifier = Modifier.weight(1f).height(48.dp),
                            colors = ButtonDefaults.buttonColors(containerColor = BoostBlue)
                        ) {
                            Text("ОТПРАВИТЬ", color = NeonWhite, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                        }
                        OutlinedButton(
                            onClick = { viewModel.scanDiagnosticTroubleCodes() },
                            modifier = Modifier.weight(1f).height(48.dp),
                            border = BorderStroke(1.dp, BoostRed)
                        ) {
                            Text("СКАН ОШИБОК", color = BoostRed, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                        }
                    }
                }
            }
        }

        item {
            Text("Результат", color = TextGray, fontSize = 12.sp, fontWeight = FontWeight.Bold, modifier = Modifier.padding(top = 4.dp, bottom = 8.dp))
        }

        if (log.isEmpty()) {
            item {
                Text(
                    "Лог пуст. Для расшифровки PID включите Launch и смотрите пойманные кадры здесь.",
                    color = TextGray,
                    fontSize = 13.sp,
                    modifier = Modifier.padding(16.dp)
                )
            }
        } else {
            items(log.asReversed()) { line ->
                Text(
                    text = line,
                    color = NeonWhite,
                    fontSize = 12.sp,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp)
                )
            }
        }
    }
}
