# YRV_Boostcontrol

Проект бустконтроллера на базе ESP32 для Toyota YRV Turbo: Android-приложение для настройки и мониторинга по BLE и прошивка контроллера.

## Что внутри

- `app/` — Android-приложение на Kotlin + Jetpack Compose
- `firmware/boost_controller_refined/boost_controller_refined.ino` — прошивка ESP32
- `gradle/`, `gradlew`, `build.gradle.kts` — сборка Android-проекта

## Возможности

- BLE-подключение к ESP32
- Онлайн-телеметрия: буст, RPM, скорость, TPS, duty
- Настройка PID и калибровок
- Редактирование 3D-карты базового duty
- OTA-режим для обновления прошивки
- Сервисные экраны для обслуживания и калибровки

## Прошивка

Основная версия прошивки лежит в:

`firmware/boost_controller_refined/boost_controller_refined.ino`

В прошивке реализованы:

- watchdog и fail-safe режимы
- ограничение и валидация BLE-команд
- безопасное сохранение параметров
- карта базового duty с самообучением
- OTA через Wi-Fi AP

## Сборка Android

Открыть проект в Android Studio и собрать `app`.

Из консоли:

```powershell
./gradlew.bat assembleDebug
```

## Прошивка ESP32

Открыть файл `.ino` в Arduino IDE или PlatformIO, установить необходимые библиотеки и прошить плату ESP32.

Используемые основные библиотеки:

- `Adafruit ADS1X15`
- `Preferences`
- `BLEDevice / BLEServer`
- `WebServer`

## BLE-протокол

Примеры команд:

- `GET:SETTINGS`
- `TAB:0`
- `SET:tB:0.85`
- `SET:kP:25.0`
- `SET:M_0_0:60.0`
- `DUTY:30`
- `SAVE`
- `SAVE_MAP`
- `RESET`
- `OTA:ON`

## Статус

Проект подготовлен к совместной отладке приложения и прошивки. Android-часть и BLE-протокол приведены к общему формату с текущей версией ESP-кода.
