package com.example.booster.data

import android.content.Context
import android.content.SharedPreferences

class AppPreferencesRepository(context: Context) {
    private val appContext = context.applicationContext

    val maintenancePrefs: SharedPreferences
        get() = appContext.getSharedPreferences(MAINTENANCE_PREFS, Context.MODE_PRIVATE)

    val dynamicsPrefs: SharedPreferences
        get() = appContext.getSharedPreferences(DYNAMICS_PREFS, Context.MODE_PRIVATE)

    fun hasRequestedLocationPermission(): Boolean {
        return appPrefs.getBoolean(KEY_LOCATION_PERMISSION_REQUESTED, false)
    }

    fun markLocationPermissionRequested() {
        appPrefs.edit().putBoolean(KEY_LOCATION_PERMISSION_REQUESTED, true).apply()
    }

    private val appPrefs: SharedPreferences
        get() = appContext.getSharedPreferences(APP_PREFS, Context.MODE_PRIVATE)

    private companion object {
        const val APP_PREFS = "AppPrefs"
        const val MAINTENANCE_PREFS = "MaintenancePrefs"
        const val DYNAMICS_PREFS = "DynamicsPrefs"
        const val KEY_LOCATION_PERMISSION_REQUESTED = "location_permission_requested_v2"
    }
}
