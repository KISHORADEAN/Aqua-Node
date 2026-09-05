/*
  AQUA NODE v4

  ESP32 + DS1302 + relay + Telegram
  Requires config.h from the v3 project plus UniversalTelegramBot's
  TelegramCertificate.h (included with current releases of the library).
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

#include "config.h"

#if defined(AQUA_NODE_SELF_TEST)
#include <assert.h>
#endif

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

ThreeWire rtcWire(RTC_DAT, RTC_CLK, RTC_RST);
RtcDS1302<ThreeWire> Rtc(rtcWire);
Preferences preferences;

enum PumpMode {
  PUMP_OFF,
  PUMP_MANUAL,
  PUMP_TIMER,
  PUMP_SCHEDULE
};

enum InputState {
  INPUT_NONE,
  INPUT_ADD_TIME,
  INPUT_ADD_DURATION,
  INPUT_EDIT_TIME,
  INPUT_EDIT_DURATION,
  INPUT_DELETE
};

struct Schedule {
  uint8_t hour;
  uint8_t minute;
  uint16_t duration;
  bool enabled;
};

bool systemEnabled = true;
bool autoMode = false;
bool pumpRunning = false;
bool wifiWasConnected = false;
bool startupAnnouncementPending = true;
bool rtcNeedsAttention = false;

PumpMode currentPumpMode = PUMP_OFF;
InputState inputState = INPUT_NONE;

unsigned long pumpStartTime = 0;
unsigned long pumpDuration = 0;
unsigned long lastTelegramCheck = 0;
unsigned long lastWifiAttempt = 0;

Schedule schedules[MAX_SCHEDULES];
bool scheduleExecuted[MAX_SCHEDULES];
bool scheduleDirty[MAX_SCHEDULES];
int scheduleCount = 0;
uint32_t lastExecutedDate = 0;
bool scheduleCountDirty = false;

String inputChatId;
uint8_t pendingHour = 0;
uint8_t pendingMinute = 0;
int editingScheduleIndex = -1;

inline unsigned long elapsedSince(unsigned long startTime) {
  return millis() - startTime;
}

bool isAuthorized(const String &chatId);
bool parseNumber(const String &text, int &value);
bool parseTime(
  const String &text,
  uint8_t &hour,
  uint8_t &minute
);
bool isValidDuration(int duration);
bool isValidSchedule(const Schedule &schedule);
bool inputLockedFor(const String &chatId);
bool startPump(PumpMode mode, unsigned long durationMs);

#if defined(AQUA_NODE_SELF_TEST)
void runSelfTest();
#endif

void pumpOFF(const char *reason);
void startTimedPump(uint16_t durationMinutes, const String &chatId);
void checkPumpTimer();
void checkPumpFailsafe();
void checkAutomaticSchedule();
void resetScheduleExecution(const RtcDateTime &now);

void clearInput();
void saveSchedules();
void loadSchedules();
void addSchedule(
  uint8_t hour,
  uint8_t minute,
  uint16_t duration
);
void deleteSchedule(int index);
bool updateSchedule(
  int index,
  uint8_t hour,
  uint8_t minute,
  uint16_t duration
);

String getTimeString();
String getDateString();
String getPumpModeString();
String getStatus();
String getScheduleList();
String getMainKeyboard();
String getManualKeyboard();
String getAutoKeyboard();
String getScheduleKeyboard();

void sendToAllAuthorized(const String &message);
void sendStartupAnnouncement();
void handleCallback(
  const String &chatId,
  const String &data
);
void handleMessage(int messageCount);
void checkTelegram();
void connectWiFi();
void checkWiFiWatchdog();
void setupRTC();


// ============================================================
// AUTHORIZATION
// ============================================================

bool isAuthorized(const String &chatId) {
  for (int i = 0; i < NUMBER_OF_ALLOWED_CHATS; ++i) {
    if (chatId == ALLOWED_CHAT_IDS[i]) {
      return true;
    }
  }

  return false;
}


// ============================================================
// PARSING
// ============================================================

bool parseNumber(const String &text, int &value) {
  if (text.length() == 0 || text.length() > 5) {
    return false;
  }

  int parsed = 0;

  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];

    if (c < '0' || c > '9') {
      return false;
    }

    parsed = (parsed * 10) + (c - '0');
  }

  value = parsed;
  return true;
}

bool parseTime(
  const String &text,
  uint8_t &hour,
  uint8_t &minute
) {
  if (text.length() != 5 || text[2] != ':') {
    return false;
  }

  for (int i = 0; i < 5; ++i) {
    if (i != 2 &&
        (text[i] < '0' || text[i] > '9')) {
      return false;
    }
  }

  const int parsedHour =
    (text[0] - '0') * 10 +
    (text[1] - '0');

  const int parsedMinute =
    (text[3] - '0') * 10 +
    (text[4] - '0');

  if (parsedHour > 23 || parsedMinute > 59) {
    return false;
  }

  hour = parsedHour;
  minute = parsedMinute;

  return true;
}

unsigned long maxSafeDurationMinutes() {
  const unsigned long failsafeMinutes =
    PUMP_MAX_RUNTIME_MS / 60000UL;

  return MAX_DURATION_MIN < failsafeMinutes
    ? MAX_DURATION_MIN
    : failsafeMinutes;
}

bool isValidDuration(int duration) {
  return duration > 0 &&
         static_cast<unsigned long>(duration) <=
           maxSafeDurationMinutes();
}

bool isValidSchedule(const Schedule &schedule) {
  return schedule.hour < 24 &&
         schedule.minute < 60 &&
         isValidDuration(schedule.duration);
}


// ============================================================
// SELF TEST
// ============================================================

#if defined(AQUA_NODE_SELF_TEST)

void runSelfTest() {
  uint8_t hour = 0;
  uint8_t minute = 0;
  int number = 0;

  assert(
    parseTime("06:30", hour, minute) &&
    hour == 6 &&
    minute == 30
  );

  assert(!parseTime("6:30", hour, minute));

  assert(
    parseNumber("12", number) &&
    number == 12
  );

  assert(!parseNumber("12min", number));

  assert(!isValidDuration(0));
}

#endif


// ============================================================
// INPUT STATE
// ============================================================

bool inputLockedFor(const String &chatId) {
  return inputState != INPUT_NONE &&
         inputChatId != chatId;
}

void clearInput() {
  inputState = INPUT_NONE;
  inputChatId = "";
  editingScheduleIndex = -1;
  pendingHour = 0;
  pendingMinute = 0;
}


// ============================================================
// RTC / TIME
// ============================================================

String getTimeString() {
  RtcDateTime now = Rtc.GetDateTime();

  char buffer[16];

  snprintf(
    buffer,
    sizeof(buffer),
    "%02u:%02u:%02u",
    now.Hour(),
    now.Minute(),
    now.Second()
  );

  return String(buffer);
}

String getDateString() {
  RtcDateTime now = Rtc.GetDateTime();

  char buffer[20];

  snprintf(
    buffer,
    sizeof(buffer),
    "%02u/%02u/%04u",
    now.Day(),
    now.Month(),
    now.Year()
  );

  return String(buffer);
}


// ============================================================
// PUMP
// ============================================================

String getPumpModeString() {
  switch (currentPumpMode) {

    case PUMP_MANUAL:
      return "MANUAL";

    case PUMP_TIMER:
      return "TIMER";

    case PUMP_SCHEDULE:
      return "SCHEDULE";

    default:
      return "OFF";
  }
}

bool startPump(
  PumpMode mode,
  unsigned long durationMs
) {
  if (!systemEnabled) {
    Serial.println("[PUMP] Blocked, system OFF");
    return false;
  }

  if (pumpRunning) {
    Serial.println(
      "[PUMP] Start ignored, already running"
    );
    return false;
  }

  if (
    (mode == PUMP_TIMER ||
     mode == PUMP_SCHEDULE) &&
    durationMs == 0
  ) {
    Serial.println(
      "[PUMP] Start blocked, invalid timed duration"
    );
    return false;
  }

  currentPumpMode = mode;
  pumpDuration = durationMs;

  digitalWrite(RELAY_PIN, RELAY_ON);

  pumpStartTime = millis();
  pumpRunning = true;

  Serial.println(
    "[PUMP] ON, mode=" +
    getPumpModeString()
  );

  return true;
}

void pumpOFF(const char *reason) {
  digitalWrite(RELAY_PIN, RELAY_OFF);

  const bool wasRunning = pumpRunning;

  pumpRunning = false;
  pumpDuration = 0;
  currentPumpMode = PUMP_OFF;

  if (wasRunning) {
    Serial.print("[PUMP] OFF, reason=");
    Serial.println(reason);
  }
}

void startTimedPump(
  uint16_t durationMinutes,
  const String &chatId
) {
  if (!systemEnabled) {
    bot.sendMessage(
      chatId,
      "SYSTEM OFF. The pump cannot be turned on.",
      ""
    );
    return;
  }

  if (pumpRunning) {
    bot.sendMessage(
      chatId,
      "The pump is already running.",
      ""
    );
    return;
  }

  if (!isValidDuration(durationMinutes)) {
    bot.sendMessage(
      chatId,
      "Invalid duration. Maximum safe duration is " +
      String(maxSafeDurationMinutes()) +
      " min.",
      ""
    );
    return;
  }

  if (
    !startPump(
      PUMP_TIMER,
      durationMinutes * 60000UL
    )
  ) {
    return;
  }

  bot.sendMessage(
    chatId,
    "PUMP RUNNING\n\n"
    "Duration: " +
    String(durationMinutes) +
    " min\n"
    "Mode: TIMER\n"
    "Time: " +
    getTimeString(),
    ""
  );
}

void checkPumpTimer() {
  if (!pumpRunning) {
    return;
  }

  if (
    currentPumpMode != PUMP_TIMER &&
    currentPumpMode != PUMP_SCHEDULE
  ) {
    return;
  }

  if (
    elapsedSince(pumpStartTime) >=
    pumpDuration
  ) {
    pumpOFF("timer_complete");

    sendToAllAuthorized(
      "PUMP OFF\n\n"
      "Watering duration complete."
    );
  }
}

void checkPumpFailsafe() {
  if (!pumpRunning) {
    return;
  }

  if (
    elapsedSince(pumpStartTime) >=
    PUMP_MAX_RUNTIME_MS
  ) {
    pumpOFF("failsafe_max_runtime");

    sendToAllAuthorized(
      "FAILSAFE TRIGGERED\n\n"
      "The pump was force-stopped after " +
      String(
        PUMP_MAX_RUNTIME_MS / 60000UL
      ) +
      " min. Check the system."
    );
  }
}


// ============================================================
// STATUS
// ============================================================

String getStatus() {
  String message = "AQUA NODE STATUS\n\n";

  message += "System: ";
  message += systemEnabled
    ? "ON\n"
    : "OFF\n";

  message += "Pump: ";
  message += pumpRunning
    ? "ON\n"
    : "OFF\n";

  message += "Mode: ";
  message += getPumpModeString();
  message += "\n";

  message += "Auto: ";
  message += autoMode
    ? "ON\n"
    : "OFF\n";

  if (pumpRunning) {

    const unsigned long elapsed =
      elapsedSince(pumpStartTime);

    const unsigned long failsafeRemaining =
      elapsed < PUMP_MAX_RUNTIME_MS
        ? PUMP_MAX_RUNTIME_MS - elapsed
        : 0;

    message +=
      "Failsafe left: " +
      String(failsafeRemaining / 1000UL) +
      " sec\n";

    if (
      currentPumpMode == PUMP_TIMER ||
      currentPumpMode == PUMP_SCHEDULE
    ) {

      const unsigned long timerRemaining =
        elapsed < pumpDuration
          ? pumpDuration - elapsed
          : 0;

      message +=
        "Timer left: " +
        String(timerRemaining / 1000UL) +
        " sec\n";
    }
  }

  message += "WiFi: ";
  message += WiFi.status() == WL_CONNECTED
    ? "Connected\n"
    : "Disconnected\n";

  message +=
    "Time: " +
    getTimeString();

  message +=
    "\nDate: " +
    getDateString();

  if (rtcNeedsAttention) {
    message +=
      "\nRTC warning: clock was reset "
      "from build time at boot.";
  }

  return message;
}


// ============================================================
// TELEGRAM KEYBOARDS
// ============================================================

String getMainKeyboard() {
  return
    "["
      "["
        "{\"text\":\"🔵 MANUAL\",\"callback_data\":\"MANUAL\"},"
        "{\"text\":\"⚙️ AUTO\",\"callback_data\":\"AUTO\"}"
      "],"
      "["
        "{\"text\":\"💧 PUMP ON\",\"callback_data\":\"PUMP_ON\"},"
        "{\"text\":\"⛔ PUMP OFF\",\"callback_data\":\"PUMP_OFF\"}"
      "],"
      "["
        "{\"text\":\"🔌 SYSTEM ON\",\"callback_data\":\"SYSTEM_ON\"},"
        "{\"text\":\"🔴 SYSTEM OFF\",\"callback_data\":\"SYSTEM_OFF\"}"
      "],"
      "["
        "{\"text\":\"📊 STATUS\",\"callback_data\":\"STATUS\"}"
      "]"
    "]";
}

String getManualKeyboard() {
  return
    "["
      "["
        "{\"text\":\"🟢 PUMP ON\",\"callback_data\":\"PUMP_ON\"},"
        "{\"text\":\"🔴 PUMP OFF\",\"callback_data\":\"PUMP_OFF\"}"
      "],"
      "["
        "{\"text\":\"💧 1 MIN\",\"callback_data\":\"TIMER_1\"},"
        "{\"text\":\"💧 2 MIN\",\"callback_data\":\"TIMER_2\"}"
      "],"
      "["
        "{\"text\":\"💧 3 MIN\",\"callback_data\":\"TIMER_3\"}"
      "],"
      "["
        "{\"text\":\"⬅️ MENU\",\"callback_data\":\"MENU\"}"
      "]"
    "]";
}

String getAutoKeyboard() {
  return
    "["
      "["
        "{\"text\":\"🟢 AUTO ON\",\"callback_data\":\"AUTO_ON\"},"
        "{\"text\":\"🔴 AUTO OFF\",\"callback_data\":\"AUTO_OFF\"}"
      "],"
      "["
        "{\"text\":\"⏰ SCHEDULE\",\"callback_data\":\"SCHEDULE\"}"
      "],"
      "["
        "{\"text\":\"⬅️ MENU\",\"callback_data\":\"MENU\"}"
      "]"
    "]";
}

String getScheduleKeyboard() {
  return
    "["
      "["
        "{\"text\":\"📋 LIST\",\"callback_data\":\"SCHEDULE_LIST\"}"
      "],"
      "["
        "{\"text\":\"➕ ADD\",\"callback_data\":\"SCHEDULE_ADD\"}"
      "],"
      "["
        "{\"text\":\"✏️ EDIT\",\"callback_data\":\"SCHEDULE_EDIT\"}"
      "],"
      "["
        "{\"text\":\"🗑️ DELETE\",\"callback_data\":\"SCHEDULE_DELETE\"}"
      "],"
      "["
        "{\"text\":\"⬅️ AUTO\",\"callback_data\":\"AUTO\"}"
      "]"
    "]";
}


// ============================================================
// SCHEDULE
// ============================================================

String getScheduleList() {
  String message = "SCHEDULE LIST\n\n";

  if (scheduleCount == 0) {
    return message + "No schedules yet.";
  }

  for (int i = 0; i < scheduleCount; ++i) {

    char timeBuffer[6];

    snprintf(
      timeBuffer,
      sizeof(timeBuffer),
      "%02u:%02u",
      schedules[i].hour,
      schedules[i].minute
    );

    message +=
      String(i + 1) +
      ". " +
      String(timeBuffer) +
      " - " +
      String(schedules[i].duration) +
      " min ";

    message += schedules[i].enabled
      ? "ON\n"
      : "OFF\n";
  }

  return message;
}

void saveSchedules() {
  if (!preferences.begin("aquanode", false)) {
    Serial.println(
      "[NVS] Could not open schedule storage"
    );
    return;
  }

  if (
    scheduleCountDirty &&
    preferences.putInt(
      "count",
      scheduleCount
    ) > 0
  ) {
    scheduleCountDirty = false;
  }

  for (int i = 0; i < MAX_SCHEDULES; ++i) {

    if (!scheduleDirty[i]) {
      continue;
    }

    const String prefix =
      "s" + String(i);

    const bool saved =
      preferences.putUChar(
        (prefix + "h").c_str(),
        schedules[i].hour
      ) > 0 &&

      preferences.putUChar(
        (prefix + "m").c_str(),
        schedules[i].minute
      ) > 0 &&

      preferences.putUShort(
        (prefix + "d").c_str(),
        schedules[i].duration
      ) > 0 &&

      preferences.putBool(
        (prefix + "e").c_str(),
        schedules[i].enabled
      ) > 0;

    if (saved) {
      scheduleDirty[i] = false;
    }
  }

  preferences.end();
}

void loadSchedules() {

  for (int i = 0; i < MAX_SCHEDULES; ++i) {

    schedules[i] = {
      0,
      0,
      0,
      false
    };

    scheduleExecuted[i] = false;
    scheduleDirty[i] = false;
  }

  if (!preferences.begin("aquanode", true)) {
    Serial.println(
      "[NVS] Could not read schedule storage"
    );
    return;
  }

  int storedCount =
    preferences.getInt("count", 0);

  bool repaired = false;

  if (
    storedCount < 0 ||
    storedCount > MAX_SCHEDULES
  ) {
    storedCount = 0;
    repaired = true;
  }

  int validCount = 0;

  for (int i = 0; i < storedCount; ++i) {

    const String prefix =
      "s" + String(i);

    Schedule loaded = {
      preferences.getUChar(
        (prefix + "h").c_str(),
        0
      ),

      preferences.getUChar(
        (prefix + "m").c_str(),
        0
      ),

      preferences.getUShort(
        (prefix + "d").c_str(),
        0
      ),

      preferences.getBool(
        (prefix + "e").c_str(),
        false
      )
    };

    if (isValidSchedule(loaded)) {

      schedules[validCount++] =
        loaded;

    } else {

      repaired = true;

      Serial.println(
        "[NVS] Ignored invalid schedule slot"
      );
    }
  }

  preferences.end();

  scheduleCount = validCount;

  if (repaired) {

    scheduleCountDirty = true;

    for (
      int i = 0;
      i < scheduleCount;
      ++i
    ) {
      scheduleDirty[i] = true;
    }

    saveSchedules();
  }
}

void addSchedule(
  uint8_t hour,
  uint8_t minute,
  uint16_t duration
) {
  if (scheduleCount >= MAX_SCHEDULES) {
    return;
  }

  schedules[scheduleCount] = {
    hour,
    minute,
    duration,
    true
  };

  scheduleExecuted[scheduleCount] = false;
  scheduleDirty[scheduleCount] = true;

  ++scheduleCount;

  scheduleCountDirty = true;

  saveSchedules();
}

void deleteSchedule(int index) {
  if (
    index < 0 ||
    index >= scheduleCount
  ) {
    return;
  }

  for (
    int i = index;
    i < scheduleCount - 1;
    ++i
  ) {

    schedules[i] =
      schedules[i + 1];

    scheduleExecuted[i] =
      scheduleExecuted[i + 1];

    scheduleDirty[i] = true;
  }

  --scheduleCount;

  scheduleExecuted[scheduleCount] =
    false;

  scheduleCountDirty = true;

  saveSchedules();
}

bool updateSchedule(
  int index,
  uint8_t hour,
  uint8_t minute,
  uint16_t duration
) {
  if (
    index < 0 ||
    index >= scheduleCount
  ) {
    return false;
  }

  schedules[index] = {
    hour,
    minute,
    duration,
    true
  };

  scheduleExecuted[index] = false;
  scheduleDirty[index] = true;

  saveSchedules();

  return true;
}


// ============================================================
// AUTOMATIC SCHEDULE ENGINE
// ============================================================

uint32_t dateKey(
  const RtcDateTime &now
) {
  return
    (static_cast<uint32_t>(now.Year()) << 9) |
    (static_cast<uint32_t>(now.Month()) << 5) |
    now.Day();
}

void resetScheduleExecution(
  const RtcDateTime &now
) {
  const uint32_t today =
    dateKey(now);

  if (lastExecutedDate == today) {
    return;
  }

  for (int i = 0; i < MAX_SCHEDULES; ++i) {
    scheduleExecuted[i] = false;
  }

  lastExecutedDate = today;
}

void checkAutomaticSchedule() {

  if (
    !systemEnabled ||
    !autoMode
  ) {
    return;
  }

  const RtcDateTime now =
    Rtc.GetDateTime();

  resetScheduleExecution(now);

  for (
    int i = 0;
    i < scheduleCount;
    ++i
  ) {

    if (
      !schedules[i].enabled ||
      scheduleExecuted[i]
    ) {
      continue;
    }

    if (!isValidSchedule(schedules[i])) {

      scheduleExecuted[i] = true;

      Serial.println(
        "[SCHEDULE] Ignored invalid schedule"
      );

      continue;
    }

    if (
      now.Hour() != schedules[i].hour ||
      now.Minute() != schedules[i].minute
    ) {
      continue;
    }

    scheduleExecuted[i] = true;

    if (
      !pumpRunning &&
      startPump(
        PUMP_SCHEDULE,
        schedules[i].duration * 60000UL
      )
    ) {

      char timeBuffer[6];

      snprintf(
        timeBuffer,
        sizeof(timeBuffer),
        "%02u:%02u",
        schedules[i].hour,
        schedules[i].minute
      );

      sendToAllAuthorized(
        "AUTO SCHEDULE\n\n"
        "Time: " +
        String(timeBuffer) +
        "\nDuration: " +
        String(schedules[i].duration) +
        " min\nPump ON"
      );

    } else if (pumpRunning) {

      Serial.println(
        "[SCHEDULE] Skipped, pump is busy"
      );
    }
  }
}


// ============================================================
// TELEGRAM NOTIFICATIONS
// ============================================================

void sendToAllAuthorized(
  const String &message
) {
  if (WiFi.status() != WL_CONNECTED) {

    Serial.println(
      "[TELEGRAM] Notification skipped, WiFi down"
    );

    return;
  }

  for (
    int i = 0;
    i < NUMBER_OF_ALLOWED_CHATS;
    ++i
  ) {

    bot.sendMessage(
      ALLOWED_CHAT_IDS[i],
      message,
      ""
    );
  }
}

void sendStartupAnnouncement() {
  sendToAllAuthorized(
    "AQUA NODE v4 ONLINE\n\n"
    "System: ON\n"
    "Pump: OFF\n"
    "Auto: OFF\n\n"
    "Use /menu to open the control panel."
  );
}


// ============================================================
// CALLBACK HANDLER
// ============================================================

void handleCallback(
  const String &chatId,
  const String &data
) {

  if (inputLockedFor(chatId)) {

    bot.sendMessage(
      chatId,
      "Another authorized chat is editing "
      "a schedule. Try again shortly.",
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // MENU
  // ----------------------------------------------------------

  if (data == "MENU") {

    bot.sendMessageWithInlineKeyboard(
      chatId,
      "AQUA NODE\n\nControl Panel",
      "",
      getMainKeyboard()
    );
  }


  // ----------------------------------------------------------
  // MANUAL
  // ----------------------------------------------------------

  else if (data == "MANUAL") {

    bot.sendMessageWithInlineKeyboard(
      chatId,
      "MANUAL CONTROL\n\n"
      "Choose a pump control:",
      "",
      getManualKeyboard()
    );
  }


  // ----------------------------------------------------------
  // PUMP ON
  // ----------------------------------------------------------

  else if (data == "PUMP_ON") {

    if (!systemEnabled) {

      bot.sendMessage(
        chatId,
        "SYSTEM OFF. Turn SYSTEM ON first.",
        ""
      );

    } else if (pumpRunning) {

      bot.sendMessage(
        chatId,
        "The pump is already running.",
        ""
      );

    } else if (
      startPump(PUMP_MANUAL, 0)
    ) {

      bot.sendMessage(
        chatId,
        "PUMP ON\n\n"
        "Mode: MANUAL\n"
        "Failsafe limit: " +
        String(
          PUMP_MAX_RUNTIME_MS / 60000UL
        ) +
        " min.",
        ""
      );
    }
  }


  // ----------------------------------------------------------
  // PUMP OFF
  // ----------------------------------------------------------

  else if (data == "PUMP_OFF") {

    pumpOFF("manual_off");

    bot.sendMessage(
      chatId,
      "PUMP OFF",
      ""
    );
  }


  // ----------------------------------------------------------
  // TIMERS
  // ----------------------------------------------------------

  else if (data == "TIMER_1") {
    startTimedPump(1, chatId);
  }

  else if (data == "TIMER_2") {
    startTimedPump(2, chatId);
  }

  else if (data == "TIMER_3") {
    startTimedPump(3, chatId);
  }


  // ----------------------------------------------------------
  // SYSTEM
  // ----------------------------------------------------------

  else if (data == "SYSTEM_ON") {

    systemEnabled = true;

    bot.sendMessage(
      chatId,
      "SYSTEM ON. Aqua Node is ready.",
      ""
    );
  }

  else if (data == "SYSTEM_OFF") {

    systemEnabled = false;

    pumpOFF("system_off");

    bot.sendMessage(
      chatId,
      "SYSTEM OFF. Pump stopped and automation blocked.",
      ""
    );
  }


  // ----------------------------------------------------------
  // AUTO
  // ----------------------------------------------------------

  else if (data == "AUTO") {

    bot.sendMessageWithInlineKeyboard(
      chatId,
      "AUTOMATIC MODE\n\n"
      "Manage Aqua Node automation.",
      "",
      getAutoKeyboard()
    );
  }

  else if (data == "AUTO_ON") {

    autoMode = true;

    String message =
      "AUTO MODE ON\n\n" +
      getScheduleList();

    if (rtcNeedsAttention) {

      message +=
        "\n\nWARNING: RTC was reset "
        "from build time at boot.";
    }

    bot.sendMessage(
      chatId,
      message,
      ""
    );
  }

  else if (data == "AUTO_OFF") {

    autoMode = false;

    bot.sendMessage(
      chatId,
      "AUTO MODE OFF. Schedules will not "
      "run the pump.",
      ""
    );
  }


  // ----------------------------------------------------------
  // SCHEDULE MENU
  // ----------------------------------------------------------

  else if (
    data == "SCHEDULE" ||
    data == "SCHEDULE_LIST"
  ) {

    bot.sendMessageWithInlineKeyboard(
      chatId,
      getScheduleList(),
      "",
      getScheduleKeyboard()
    );
  }


  // ----------------------------------------------------------
  // ADD SCHEDULE
  // ----------------------------------------------------------

  else if (data == "SCHEDULE_ADD") {

    if (
      scheduleCount >= MAX_SCHEDULES
    ) {

      bot.sendMessage(
        chatId,
        "Schedule list is full.",
        ""
      );

      return;
    }

    clearInput();

    inputState = INPUT_ADD_TIME;
    inputChatId = chatId;

    bot.sendMessage(
      chatId,
      "ADD SCHEDULE\n\n"
      "Send the time exactly as HH:MM\n"
      "Example: 06:30\n\n"
      "/cancel to stop.",
      ""
    );
  }


  // ----------------------------------------------------------
  // EDIT SCHEDULE
  // ----------------------------------------------------------

  else if (data == "SCHEDULE_EDIT") {

    if (scheduleCount == 0) {

      bot.sendMessage(
        chatId,
        "No schedules yet.",
        ""
      );

      return;
    }

    clearInput();

    inputState = INPUT_EDIT_TIME;
    inputChatId = chatId;

    bot.sendMessage(
      chatId,
      getScheduleList() +
      "\nEDIT SCHEDULE\n\n"
      "Send schedule number and new time.\n"
      "Example: 1 06:30\n\n"
      "/cancel to stop.",
      ""
    );
  }


  // ----------------------------------------------------------
  // DELETE SCHEDULE
  // ----------------------------------------------------------

  else if (data == "SCHEDULE_DELETE") {

    if (scheduleCount == 0) {

      bot.sendMessage(
        chatId,
        "No schedules yet.",
        ""
      );

      return;
    }

    clearInput();

    inputState = INPUT_DELETE;
    inputChatId = chatId;

    bot.sendMessage(
      chatId,
      getScheduleList() +
      "\nDELETE SCHEDULE\n\n"
      "Send the schedule number.\n"
      "Example: 2\n\n"
      "/cancel to stop.",
      ""
    );
  }


  // ----------------------------------------------------------
  // STATUS
  // ----------------------------------------------------------

  else if (data == "STATUS") {

    bot.sendMessageWithInlineKeyboard(
      chatId,
      getStatus(),
      "",
      getMainKeyboard()
    );
  }
}


// ============================================================
// TELEGRAM MESSAGE HANDLER
// ============================================================

void handleMessage(int messageCount) {

  for (
    int i = 0;
    i < messageCount;
    ++i
  ) {

    const String chatId =
      bot.messages[i].chat_id;

    const String text =
      bot.messages[i].text;

    const String type =
      bot.messages[i].type;


    // --------------------------------------------------------
    // AUTHORIZATION
    // --------------------------------------------------------

    if (!isAuthorized(chatId)) {

      Serial.println(
        "[TELEGRAM] Ignored unauthorized chat"
      );

      continue;
    }


    // --------------------------------------------------------
    // CALLBACK QUERY
    // --------------------------------------------------------

    if (type == "callback_query") {

      handleCallback(
        chatId,
        text
      );

      continue;
    }


    // --------------------------------------------------------
    // INPUT LOCK
    // --------------------------------------------------------

    if (inputLockedFor(chatId)) {

      bot.sendMessage(
        chatId,
        "Another authorized chat is editing "
        "a schedule. Try again shortly.",
        ""
      );

      continue;
    }


    // --------------------------------------------------------
    // CANCEL
    // --------------------------------------------------------

    if (text == "/cancel") {

      if (inputState == INPUT_NONE) {

        bot.sendMessage(
          chatId,
          "Nothing to cancel.",
          ""
        );

      } else {

        clearInput();

        bot.sendMessage(
          chatId,
          "Schedule edit cancelled.",
          ""
        );
      }

      continue;
    }


    // --------------------------------------------------------
    // ADD TIME
    // --------------------------------------------------------

    if (
      inputState == INPUT_ADD_TIME
    ) {

      if (
        !parseTime(
          text,
          pendingHour,
          pendingMinute
        )
      ) {

        bot.sendMessage(
          chatId,
          "Wrong format. Use exactly HH:MM, "
          "for example 06:30.",
          ""
        );

        continue;
      }

      inputState =
        INPUT_ADD_DURATION;

      bot.sendMessage(
        chatId,
        "Time: " +
        text +
        "\n\nSend duration in minutes (max " +
        String(maxSafeDurationMinutes()) +
        ").",
        ""
      );

      continue;
    }


    // --------------------------------------------------------
    // ADD DURATION
    // --------------------------------------------------------

    if (
      inputState == INPUT_ADD_DURATION
    ) {

      int duration = 0;

      if (
        !parseNumber(text, duration) ||
        !isValidDuration(duration)
      ) {

        bot.sendMessage(
          chatId,
          "Invalid duration. Use a whole "
          "number from 1 to " +
          String(maxSafeDurationMinutes()) +
          ".",
          ""
        );

        continue;
      }

      addSchedule(
        pendingHour,
        pendingMinute,
        duration
      );

      clearInput();

      bot.sendMessageWithInlineKeyboard(
        chatId,
        "SCHEDULE ADDED\n\n" +
        getScheduleList(),
        "",
        getScheduleKeyboard()
      );

      continue;
    }


    // --------------------------------------------------------
    // EDIT TIME
    // --------------------------------------------------------

    if (
      inputState == INPUT_EDIT_TIME
    ) {

      const int space =
        text.indexOf(' ');

      int requestedNumber = 0;

      uint8_t hour = 0;
      uint8_t minute = 0;

      if (
        space <= 0 ||
        text.indexOf(
          ' ',
          space + 1
        ) >= 0 ||
        !parseNumber(
          text.substring(0, space),
          requestedNumber
        ) ||
        !parseTime(
          text.substring(space + 1),
          hour,
          minute
        ) ||
        requestedNumber < 1 ||
        requestedNumber > scheduleCount
      ) {

        bot.sendMessage(
          chatId,
          "Wrong format. Use exactly: 1 06:30",
          ""
        );

        continue;
      }

      editingScheduleIndex =
        requestedNumber - 1;

      pendingHour = hour;
      pendingMinute = minute;

      inputState =
        INPUT_EDIT_DURATION;

      bot.sendMessage(
        chatId,
        "New time: " +
        text.substring(space + 1) +
        "\n\nSend duration in minutes (max " +
        String(maxSafeDurationMinutes()) +
        ").",
        ""
      );

      continue;
    }


    // --------------------------------------------------------
    // EDIT DURATION
    // --------------------------------------------------------

    if (
      inputState == INPUT_EDIT_DURATION
    ) {

      int duration = 0;

      if (
        !parseNumber(text, duration) ||
        !isValidDuration(duration)
      ) {

        bot.sendMessage(
          chatId,
          "Invalid duration. Use a whole "
          "number from 1 to " +
          String(maxSafeDurationMinutes()) +
          ".",
          ""
        );

        continue;
      }

      updateSchedule(
        editingScheduleIndex,
        pendingHour,
        pendingMinute,
        duration
      );

      clearInput();

      bot.sendMessageWithInlineKeyboard(
        chatId,
        "SCHEDULE UPDATED\n\n" +
        getScheduleList(),
        "",
        getScheduleKeyboard()
      );

      continue;
    }


    // --------------------------------------------------------
    // DELETE
    // --------------------------------------------------------

    if (
      inputState == INPUT_DELETE
    ) {

      int requestedNumber = 0;

      if (
        !parseNumber(
          text,
          requestedNumber
        ) ||
        requestedNumber < 1 ||
        requestedNumber > scheduleCount
      ) {

        bot.sendMessage(
          chatId,
          "Invalid schedule number.",
          ""
        );

        continue;
      }

      deleteSchedule(
        requestedNumber - 1
      );

      clearInput();

      bot.sendMessageWithInlineKeyboard(
        chatId,
        "SCHEDULE DELETED\n\n" +
        getScheduleList(),
        "",
        getScheduleKeyboard()
      );

      continue;
    }


    // --------------------------------------------------------
    // COMMANDS
    // --------------------------------------------------------

    if (
      text == "/start" ||
      text == "/menu"
    ) {

      bot.sendMessageWithInlineKeyboard(
        chatId,
        "AQUA NODE\n\nControl Panel",
        "",
        getMainKeyboard()
      );

    } else if (text == "/status") {

      bot.sendMessage(
        chatId,
        getStatus(),
        ""
      );

    } else {

      bot.sendMessageWithInlineKeyboard(
        chatId,
        "AQUA NODE\n\n"
        "Use /menu or the menu below.",
        "",
        getMainKeyboard()
      );
    }
  }
}


// ============================================================
// TELEGRAM POLLING
// ============================================================

void checkTelegram() {

  if (
    elapsedSince(lastTelegramCheck) <
    TELEGRAM_INTERVAL_MS
  ) {
    return;
  }

  if (
    WiFi.status() != WL_CONNECTED
  ) {
    return;
  }

  lastTelegramCheck = millis();

  const int messages =
    bot.getUpdates(
      bot.last_message_received + 1
    );

  if (messages < 0) {

    Serial.println(
      "[TELEGRAM] getUpdates failed"
    );

    return;
  }

  // One batch per loop keeps safety checks
  // responsive. Increase only after measuring
  // message backlog.

  if (messages > 0) {
    handleMessage(messages);
  }
}


// ============================================================
// WIFI
// ============================================================

void connectWiFi() {

  if (
    WiFi.status() == WL_CONNECTED
  ) {
    return;
  }

  Serial.println(
    "[WIFI] Starting connection attempt"
  );

  WiFi.disconnect();

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  lastWifiAttempt = millis();
}

void checkWiFiWatchdog() {

  if (
    WiFi.status() == WL_CONNECTED
  ) {

    if (!wifiWasConnected) {

      wifiWasConnected = true;

      Serial.print(
        "[WIFI] Connected, IP: "
      );

      Serial.println(
        WiFi.localIP()
      );


      if (startupAnnouncementPending) {

        startupAnnouncementPending = false;

        sendStartupAnnouncement();
      }
    }

    return;
  }


  if (wifiWasConnected) {

    wifiWasConnected = false;

    Serial.println(
      "[WIFI] Disconnected"
    );
  }


  if (
    elapsedSince(lastWifiAttempt) >=
    WIFI_WATCHDOG_INTERVAL_MS
  ) {

    connectWiFi();
  }
}


// ============================================================
// RTC SETUP
// ============================================================

void setupRTC() {

  Rtc.Begin();

  if (Rtc.GetIsWriteProtected()) {
    Rtc.SetIsWriteProtected(false);
  }

  if (!Rtc.IsDateTimeValid()) {

    rtcNeedsAttention = true;

    Serial.println(
      "[RTC] Invalid date/time, using build time"
    );

    Rtc.SetDateTime(
      RtcDateTime(
        __DATE__,
        __TIME__
      )
    );
  }

  if (!Rtc.GetIsRunning()) {
    Rtc.SetIsRunning(true);
  }
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);


  // Preload the inactive level before
  // the GPIO becomes an output.

  digitalWrite(
    RELAY_PIN,
    RELAY_OFF
  );

  pinMode(
    RELAY_PIN,
    OUTPUT
  );


#if defined(AQUA_NODE_SELF_TEST)

  runSelfTest();

#endif


  setupRTC();

  loadSchedules();


  client.setCACert(
    TELEGRAM_CERTIFICATE_ROOT
  );

  client.setTimeout(5000);


  WiFi.mode(WIFI_STA);

  WiFi.persistent(false);

  WiFi.setAutoReconnect(true);

  connectWiFi();


  Serial.println();

  Serial.println(
    "=============================="
  );

  Serial.println(
    "       AQUA NODE v4"
  );

  Serial.println(
    "=============================="
  );

  Serial.print("[RTC] ");

  Serial.println(
    getTimeString()
  );

  Serial.print("[SCHEDULE] Count: ");

  Serial.println(
    scheduleCount
  );
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

  // Stop decisions run before any
  // network work.

  checkPumpFailsafe();

  checkPumpTimer();

  checkAutomaticSchedule();

  checkWiFiWatchdog();

  checkTelegram();
}