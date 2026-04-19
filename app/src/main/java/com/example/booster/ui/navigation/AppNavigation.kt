package com.example.booster.ui.navigation

import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.example.booster.ui.screens.*
import com.example.booster.viewmodel.BoosterViewModel
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material.icons.filled.Timeline
import androidx.compose.ui.graphics.vector.ImageVector

// Добавляем описание наших экранов
sealed class Screen(val route: String, val label: String, val icon: ImageVector) {
    object Dashboard : Screen("dashboard", "Дашборд", Icons.Filled.Speed)
    object Settings : Screen("settings", "Настройки", Icons.Filled.Settings)
    object Maintenance : Screen("maintenance", "Сервис", Icons.Filled.Build)
    object Dynamics : Screen("dynamics", "Динамика", Icons.Filled.Timeline)
}
@Composable
fun BoosterApp() {
    val navController = rememberNavController()

    // 1. СОЗДАЕМ ВЬЮМОДЕЛЬ ЗДЕСЬ
    // Теперь это один "мозг" на всё приложение
    val boosterViewModel: BoosterViewModel = viewModel()

    val items = listOf(
        Screen.Dashboard,
        Screen.Settings,
        Screen.Maintenance,
        Screen.Dynamics
    )

    Scaffold(
        bottomBar = {
            NavigationBar {
                val navBackStackEntry by navController.currentBackStackEntryAsState()
                val currentDestination = navBackStackEntry?.destination
                items.forEach { screen ->
                    NavigationBarItem(
                        icon = { Icon(screen.icon, contentDescription = null) },
                        label = { Text(screen.label) },
                        selected = currentDestination?.hierarchy?.any { it.route == screen.route } == true,
                        onClick = {
                            navController.navigate(screen.route) {
                                popUpTo(navController.graph.findStartDestination().id) {
                                    saveState = true
                                }
                                launchSingleTop = true
                                restoreState = true
                            }
                        }
                    )
                }
            }
        }
    ) { innerPadding ->
        // 2. ПЕРЕДАЕМ ВЬЮМОДЕЛЬ В КАЖДЫЙ ЭКРАН
        NavHost(
            navController,
            startDestination = Screen.Dashboard.route,
            Modifier.padding(innerPadding)
        ) {
            composable(Screen.Dashboard.route) {
                DashboardScreen(boosterViewModel)
            }
            composable(Screen.Settings.route) {
                SettingsScreen(boosterViewModel)
            }
            composable(Screen.Maintenance.route) {
                MaintenanceScreen(boosterViewModel)
            }
            composable(Screen.Dynamics.route) {
                DynamicsScreen(boosterViewModel)
            }
        }
    }
}