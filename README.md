# Aqua-Node

ESP32 controller for a water pump with a Telegram remote control and RTC-based
schedules.

## Hardware

| Part | Connection |
| --- | --- |
| ESP32 DevKit v1 | — |
| DS1302 RTC | CLK → GPIO 18, DAT → GPIO 19, RST → GPIO 21 |
| Relay module | IN → GPIO 26 (active LOW) |

The relay pin, active level, RTC pins and safety limits are set in
`include/config.h`. Defaults live in `include/config.example.h`.

## Features

- Telegram control panel: `/menu`, `/status`, `/cancel`.
- Manual pump on/off with a 15-minute failsafe (`PUMP_MAX_RUNTIME_MS`).
- Up to 10 daily schedules (`MAX_SCHEDULES`), stored in NVS so they survive a
  reboot.
- Automatic mode plus a WiFi watchdog that reconnects without rebooting.
- Only chat IDs listed in `NUMBER_OF_ALLOWED_CHATS` may send commands.

## Setup

1. Install [PlatformIO](https://platformio.org/install).
2. Create the local config and fill in your WiFi and Telegram credentials:

   ```bash
   cp include/config.example.h include/config.h
   ```

   `include/config.h` is ignored by Git; never commit it.

3. Set `BOT_TOKEN` from [@BotFather](https://t.me/BotFather), and add your
   Telegram chat ID to the allowed list in the config.
4. Build and upload:

   ```bash
   pio run -t upload
   pio device monitor
   ```

The board target is `esp32doit-devkit-v1`; change `board` in `platformio.ini`
if you use a different ESP32 module.

## Configuration

Everything is a compile-time define in `include/config.h`:

| Setting | Purpose |
| --- | --- |
| `WIFI_SSID`, `WIFI_PASSWORD` | Network credentials |
| `BOT_TOKEN` | Telegram bot token |
| `NUMBER_OF_ALLOWED_CHATS` | Chat IDs allowed to control the pump |
| `RELAY_PIN`, `RELAY_ON`, `RELAY_OFF` | Relay wiring and active level |
| `RTC_CLK`, `RTC_DAT`, `RTC_RST` | DS1302 pins |
| `PUMP_MAX_RUNTIME_MS` | Failsafe cutoff for a manual run |
| `MAX_SCHEDULES`, `MAX_DURATION_MIN` | Schedule limits |

## Project layout

```text
include/config.example.h   template configuration
src/main.cpp               firmware
platformio.ini             board and library configuration
```
