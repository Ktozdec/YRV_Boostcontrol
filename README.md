# YRV_Boostcontrol

Проект бустконтроллера на базе ESP32 для Daihatsu YRV Turbo (K3-VET): Android-приложение для настройки и мониторинга по BLE и прошивка контроллера.

## Что внутри

- `app/` — Android-приложение на Kotlin + Jetpack Compose
- `firmware/boost_controller_refined/boost_controller_refined.ino` — прошивка ESP32
- `docs/` — документация по железу (схема платы, MITM-доработка для АКПП) и K-Line
- `gradle/`, `gradlew`, `build.gradle.kts` — сборка Android-проекта

## Возможности

- BLE-подключение к ESP32 (имя `YRV_Boost_BLE`, сервис Nordic UART) с автопереподключением
- Онлайн-телеметрия: буст, RPM, скорость, TPS, базовый и итоговый duty, режим, одометр
- Настройка PID, калибровок датчиков (MAP/TPS), параметров колёс/VSS и целевого буста
- Экраны приложения: Дашборд, Настройки, Сервис, Динамика, Диагностика
- Запись поездки в CSV с потоковым сохранением на диск и экспортом
- Диагностика K-Line: сниффер кадров и ручной мост HEX-запросов
- OTA-режим для обновления прошивки через Wi-Fi AP

## Прошивка

Основная версия прошивки лежит в:

`firmware/boost_controller_refined/boost_controller_refined.ino`

В прошивке реализованы:

- watchdog и fail-safe режимы (SOFT_LIMP / HARD_LIMP)
- 2D-карта базового duty (TPS × RPM) с адаптивным самообучением и метрикой доверия
- PID с gain scheduling, setpoint feed-forward и анти-windup
- «обманка по бусту»: ограничение MAP-сигнала на ECU через ЦАП MCP4725
- ограничение и валидация BLE-команд, безопасное сохранение параметров в NVS
- K-Line сниффер (listen-only) и ручная отправка HEX-команд
- одометр и моточасы
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

- `Adafruit ADS1X15` (MAP/TPS через ADS1115)
- `Adafruit MCP4725` (ЦАП для обманки буста)
- `Preferences` (NVS)
- `BLEDevice / BLEServer`
- `WiFi` + `WebServer` + `Update` (OTA)

## BLE-протокол

Команды от приложения к ESP32:

- `GET:SETTINGS` — запросить текущие настройки
- `SET:<key>:<value>` — изменить параметр (например `SET:tB:0.85`, `SET:kP:25.0`)
- `DUTY:30` — ручной тест соленоида (только на стоянке)
- `KREQ:8310F0210C01` — отправить HEX-запрос в K-Line
- `SAVE` — сохранить настройки и одометр в NVS
- `RESET` — сбросить накопленные максимумы
- `OTA:ON` — перезагрузка в OTA-режим (Wi-Fi AP `YRV_Boost_Pro`)

Поддерживаемые ключи `SET`: `tB`, `lB`, `kP`, `kI`, `kD`, `lA`, `oP`, `sP`, `oV`, `pR`, `vP`, `tW`, `tA`, `tR`, `oD`, `eH`.

ESP32 шлёт построчный JSON по BLE: телеметрия (`T`), настройки (`S`), статистика K-Line (`K`), ответ K-Line (`KRES`) и подтверждения команд (`ack`).

## Статус

Android-часть и BLE-протокол приведены к общему формату с текущей версией ESP-кода. Ведётся доработка диагностики и MITM-архитектуры для АКПП — детали в `docs/`.
