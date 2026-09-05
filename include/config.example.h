/*
=========================================================
                  AQUA NODE v3 - CONFIG
=========================================================
This file holds all configuration constants.
Kept separate from the logic so that:
1. It's easy to change the WiFi/Bot Token without scrolling
   through hundreds of lines
2. It can be .gitignore'd if you want to commit the project
   to a public Git repo (keep a config.h.example without real
   data, and keep the real config.h local on your machine)
=========================================================
*/

#pragma once

// =====================================================
//                    WIFI
// =====================================================

#define WIFI_SSID       "YOUR WIFI NAME"
#define WIFI_PASSWORD   "YOUR WIFI PASSWORD"

// How long to wait before a WiFi connection attempt is considered failed (ms)
#define WIFI_CONNECT_TIMEOUT_MS   20000

// Interval to check whether WiFi is still alive (ms)
#define WIFI_WATCHDOG_INTERVAL_MS 30000


// =====================================================
//                  TELEGRAM
// =====================================================

#define BOT_TOKEN       "YOUR BOT TOKEN"

// Polling interval — don't set this too low, the Telegram API has rate limits
#define TELEGRAM_INTERVAL_MS   1000

// Delay before retrying if getUpdates() fails, e.g. due to a brief WiFi drop (ms)
#define TELEGRAM_RETRY_DELAY_MS 3000


// =====================================================
//                AUTHORIZED USERS
// =====================================================

// Add the chat IDs allowed to control Aqua Node.
// To get your chat_id: message @userinfobot on Telegram.
static const char* const ALLOWED_CHAT_IDS[] = {
  "USER 1 CHAT ID", //USER NAME
  "USER 2 CHAT ID", //USER NAME
};

#define NUMBER_OF_ALLOWED_CHATS \
  (sizeof(ALLOWED_CHAT_IDS) / sizeof(ALLOWED_CHAT_IDS[0]))


// =====================================================
//                     RELAY
// =====================================================

#define RELAY_PIN   26

// Most single-channel relay modules are active-LOW
// (LOW on the trigger pin = contact closed / pump ON)
#define RELAY_ON    LOW
#define RELAY_OFF   HIGH

// FAILSAFE: absolute limit on how long the pump may run continuously.
// This is independent of any mode (manual/timer/schedule) —
// if a logic bug elsewhere ever prevents the pump from receiving
// an OFF signal, this hardware timer will still force it off at
// this point. Do not remove.
#define PUMP_MAX_RUNTIME_MS   (15UL * 60UL * 1000UL)  // 15 minutes


// =====================================================
//                    RTC DS1302
// =====================================================

#define RTC_CLK   18
#define RTC_DAT   19
#define RTC_RST   21


// =====================================================
//                   SCHEDULE
// =====================================================

#define MAX_SCHEDULES     10
#define MAX_DURATION_MIN  1440   // reasonable cap on duration input (24 hours)
