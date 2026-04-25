# ESP_Boost MITM Add-on Wiring

Ниже приведен практический вариант дочерней платы для `bench / test harness`, которая подключается к уже существующей `ESP_Boost`.

## 1. Соединения между ESP_Boost и дочерней платой

Подключения делать отдельным 10-жильным шлейфом.

### Питание и земля

- `ESP_Boost 5V` -> `MITM_ADDON 5V_IN`
- `ESP_Boost 3V3` -> `MITM_ADDON 3V3_IN`
- `ESP_Boost GND` -> `MITM_ADDON GND`

### Логические сигналы

- `ESP32 GPIO26` -> `MITM_ADDON ECM_RX_OUT`
- `ESP32 GPIO27` -> `MITM_ADDON AT_TX_IN`
- `ESP32 GPIO32` -> `MITM_ADDON AT_RX_OUT`
- `ESP32 GPIO33` -> `MITM_ADDON ECM_TX_IN`
- `ESP32 GPIO4` -> `MITM_ADDON RELAY_EN`
- `ESP32 GPIO5` -> `MITM_ADDON WD_STROBE`

### Рекомендуемые провода

- сигналы UART/MITM: `AWG26-AWG28`
- питание и земля: `AWG22-AWG24`
- длина шлейфа между платами: стараться держать `<= 20 см`

## 2. Внешние клеммы дочерней платы

На дочерней плате сделать отдельный клеммник/разъем:

- `ECM_ATTX_IN`
- `ECM_ATRX_OUT`
- `AT_COMI_OUT`
- `AT_SI_IN`
- `GND_REF`

Для bench harness этого достаточно.

## 3. Фиксированный BOM

### Логика и уровни

- `U1`: `SN74LVC1G17-Q1` — прием `ECM_ATTX_IN -> GPIO26`
- `U2`: `SN74LVC1G17-Q1` — прием `AT_SI_IN -> GPIO32`
- `U3`: `SN74AHCT125-Q1` — передача `GPIO27 -> AT_COMI_OUT` и `GPIO33 -> ECM_ATRX_OUT`

### Управление реле

- `U4`: `TPL5010` — внешний watchdog
- `Q1`: `AO3400A` — low-side ключ катушки реле
- `D1`: `1N4148WS` — flyback diode
- `R1`: `100R` — в gate MOSFET
- `R2`: `10k` — gate pulldown

### Коммутация

- `K1`: `Omron G6K-2P-Y DC5`
- `K2`: `Omron G6K-2P-Y DC5`

### Защита линий

- `D2`: low-cap ESD diode array для канала `ECM_ATTX_IN`
- `D3`: low-cap ESD diode array для канала `AT_SI_IN`
- `D4`: low-cap ESD diode array для канала `AT_COMI_OUT`
- `D5`: low-cap ESD diode array для канала `ECM_ATRX_OUT`
- `R3`: `47R` series resistor на входе `ECM_ATTX_IN`
- `R4`: `47R` series resistor на входе `AT_SI_IN`
- `R5`: `47R` series resistor на выходе `AT_COMI_OUT`
- `R6`: `47R` series resistor на выходе `ECM_ATRX_OUT`

### Питание и развязка

- `C1`: `100uF 10V low-ESR`
- `C2`: `10uF 10V`
- `C3-C8`: `100nF` около каждого IC

## 4. ASCII-схема по узлам

```text
ESP_Boost                           MITM daughterboard
-----------                         ------------------
5V -------------------------------> 5V_IN
3V3 ------------------------------> 3V3_IN
GND ------------------------------> GND

GPIO26 <-------------------------- U1.Y
GPIO27 --------------------------> U3.1A
GPIO32 <-------------------------- U2.Y
GPIO33 --------------------------> U3.2A
GPIO4  --------------------------> RELAY_EN logic
GPIO5  --------------------------> TPL5010 DONE/WD input logic


ECM_ATTX_IN --R3--+---> K1 NC/COM bypass path ---> AT_COMI_OUT
                  |
                  +---> U1.A (SN74LVC1G17-Q1, VCC=3V3)
                           U1.Y ---------------------> GPIO26

GPIO27 ----------> U3.1A (SN74AHCT125-Q1, VCC=5V)
                   U3.1OE tied active by RELAY_EN
                   U3.1Y --R5------------------------> K1 NO path -> AT_COMI_OUT


AT_SI_IN ---R4----+---> K2 NC/COM bypass path ------> ECM_ATRX_OUT
                  |
                  +---> U2.A (SN74LVC1G17-Q1, VCC=3V3)
                           U2.Y ---------------------> GPIO32

GPIO33 ----------> U3.2A (SN74AHCT125-Q1, VCC=5V)
                   U3.2OE tied active by RELAY_EN
                   U3.2Y --R6------------------------> K2 NO path -> ECM_ATRX_OUT


TPL5010 watchdog ----> enable gate
RELAY_EN + watchdog --> Q1 gate drive
Q1 drain ------------> relay coil low side
relay coil high side -> 5V_IN
D1 across coil
```

## 5. Упрощенная логика реле

- Реле обесточены:
  - `ECM_ATTX_IN -> AT_COMI_OUT`
  - `AT_SI_IN -> ECM_ATRX_OUT`
- Реле включены:
  - штатные линии разорваны
  - трафик идет через `U1/U2/U3` и `ESP32`

## 6. Что паять куда на дочерней плате

### От ESP_Boost

- провод `5V` паять в общую точку питания логики и катушек реле
- провод `3V3` паять только в шину логики `U1/U2`
- провод `GND` паять в единую землю дочерней платы
- `GPIO26` паять на выход `U1.Y`
- `GPIO27` паять на вход `U3.1A`
- `GPIO32` паять на выход `U2.Y`
- `GPIO33` паять на вход `U3.2A`
- `GPIO4` паять в цепь `RELAY_EN`
- `GPIO5` паять в цепь watchdog strobe

### На стороне внешнего интерфейса

- `ECM_ATTX_IN` паять на вход канала 1
- `AT_COMI_OUT` паять на выход канала 1
- `AT_SI_IN` паять на вход канала 2
- `ECM_ATRX_OUT` паять на выход канала 2

## 7. Практический порядок

1. Сначала собрать только прием:
   - `U1`, `U2`, `R3`, `R4`, защита, питание
   - без `K1`, `K2`, `U3`
2. Подтвердить на bench, что `GPIO26/32` стабильно читают сигналы.
3. Потом добавить `U3`.
4. Потом добавить реле и watchdog.
5. Только после прозрачного passthrough переходить к любым активным экспериментам.

## 8. Ограничение

Этот документ описывает интерфейс между `ESP_Boost` и дочерней MITM-платой для стендовой отладки и test harness. Перед подключением к реальному автомобилю нужно отдельно подтвердить уровни, направление и timing линий `ATTX / ATRX / COMI / SI`.
