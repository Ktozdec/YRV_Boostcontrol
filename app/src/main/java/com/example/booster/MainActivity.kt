package com.example.booster

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.example.booster.ui.theme.BoosterTheme
import com.example.booster.ui.navigation.BoosterApp

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Делаем интерфейс от края до края
        enableEdgeToEdge()

        setContent {
            // Оборачиваем всё в твою родную тему BOOSTer
            BoosterTheme {
                // Запускаем навигацию
                BoosterApp()
            }
        }
    }
}