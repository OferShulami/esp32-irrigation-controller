/*
 * Irrigation Controller for ESP32-S3 - Telegram alerts + remote control
 * Files: Irrigation.ino (logic), config.h (settings), secrets.h (WiFi/Telegram, git-ignored),
 *        webpage.h (phone page), telegram.h (bot)
 * Tools: USB CDC On Boot = Enabled, PSRAM = Disabled, Partition = Default 4MB with spiffs
 * Serial 115200: s = status, l = last 20 events, x = test run in 2 minutes, r = restart
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <esp_sntp.h>
#include <esp_mac.h>
#include "config.h"
#include "webpage.h"
#include "telegram.h"

// ================= TYPES (append only - numbers are stored in the log) =================
enum EvType : uint8_t { EV_BOOT = 1, EV_PUMP_ON, EV_PUMP_OFF, EV_REJECT, EV_WIFI_UP, EV_WIFI_DOWN,
                        EV_TIME_SYNC, EV_LOG_CLEARED, EV_OFFLINE, EV_ONLINE, EV_SCHED, EV_SKIP,
                        EV_TIME_PHONE, EV_SCHED_END };
enum Src      : uint8_t { SRC_WEB = 1, SRC_BUTTON, SRC_SCHEDULE, SRC_TELEGRAM };
enum OffRsn   : uint8_t { OFF_DONE = 1, OFF_WEB, OFF_BUTTON, OFF_TIMEOUT, OFF_NO_WIFI, OFF_TELEGRAM };
enum RejRsn   : uint8_t { REJ_RUNNING = 1, REJ_BADDUR, REJ_OFFLINE };
enum SchedRes : uint8_t { SR_PENDING = 0, SR_DONE, SR_SKIPPED, SR_MISS_OFFLINE, SR_MISS_DOWN, SR_MISS_BUSY,
                          SR_PARTIAL, SR_RUNNING };

struct __attribute__((packed)) Event {
  uint32_t epoch; uint32_t uptime; uint16_t boot; uint8_t type; uint8_t a; uint16_t b; uint16_t reserved;
};
struct __attribute__((packed)) LogHeader { uint32_t magic; uint16_t head; uint16_t count; };

const char* SR_TEXT[] = { "ממתינה", "בוצעה", "דולגה לבקשתך", "לא בוצעה – אין חיבור לרשת!",
                          "לא בוצעה – הבקר היה כבוי או בלי שעה!", "לא בוצעה – המשאבה כבר פעלה",
                          "בוצעה חלקית – הופסקה באמצע!", "התחילה" };

const char*    LOG_PATH   = "/events.bin";
const uint32_t LOG_MAGIC  = 0x49525231;
const int32_t  SCHED_INIT = -2;

// ================= STATE =================
WebServer   server(80);
Preferences prefs;
LogHeader   hdr;
bool        logOk = false;
uint16_t    bootCount = 0;

bool     pumpOn = false;
uint8_t  pumpSrc = 0;
uint32_t pumpStartMs = 0, pumpDurMs = 0;

int      netIdx = 0;
uint32_t attemptStartMs = 0;
uint32_t lostSinceMs = 0;
bool     lostTiming = false;           // true while counting disconnected time
bool     offline = false;              // no network after trying all -> pump locked
volatile uint8_t lastDiscReason = 0;
volatile bool syncFlag = false;
bool     firstSyncLogged = false;

int32_t  schedDay = -1;                // day key of last handled schedule
uint8_t  schedRes = SR_PENDING;
int32_t  skipDay  = -1;                // day key to skip
int      testTargetMin = -1;           // test run set by 'x' (-1 = use config.h)

// ================= HELPERS =================
bool timeValid() { return time(nullptr) > 1700000000; }

int32_t dayKeyOf(const struct tm& lt) { return (lt.tm_year + 1900) * 1000 + lt.tm_yday; }

int32_t todayKey(struct tm* out = nullptr) {
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  if (out) *out = lt;
  return dayKeyOf(lt);
}

int schedTarget() { return testTargetMin >= 0 ? testTargetMin : SCHED_HOUR * 60 + SCHED_MIN; }

int32_t nextSchedDay() {
  struct tm lt;
  int32_t today = todayKey(&lt);
  int nowMin = lt.tm_hour * 60 + lt.tm_min;
  if (schedDay != today && nowMin <= schedTarget()) return today;
  time_t t2 = time(nullptr) + 86400;
  localtime_r(&t2, &lt);
  return dayKeyOf(lt);
}

String fmtEpoch(uint32_t ep) {
  time_t t = ep;
  struct tm lt;
  localtime_r(&t, &lt);
  char b[24];
  strftime(b, sizeof(b), "%d/%m/%Y %H:%M:%S", &lt);
  return String(b);
}

String fmtShort(uint32_t ep) {
  time_t t = ep;
  struct tm lt;
  localtime_r(&t, &lt);
  char b[16];
  strftime(b, sizeof(b), "%d/%m %H:%M", &lt);
  return String(b);
}

String fmtDur(uint32_t s) {
  char b[16];
  snprintf(b, sizeof(b), "%lu:%02lu", s / 60, s % 60);
  return String(b);
}

String fmtUptime(uint32_t s) {
  char b[32];
  snprintf(b, sizeof(b), "%lu ימים, %02lu:%02lu", s / 86400, (s % 86400) / 3600, (s % 3600) / 60);
  return String(b);
}

String schedTimeStr() {
  char b[6];
  int t = schedTarget();
  snprintf(b, sizeof(b), "%02d:%02d", t / 60, t % 60);
  return String(b);
}

String jsonEsc(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

String macStr() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

void saveSched() { prefs.begin("sys", false); prefs.putInt("sday", schedDay); prefs.putUChar("sres", schedRes); prefs.end(); }
void saveSkip()  { prefs.begin("sys", false); prefs.putInt("skip", skipDay); prefs.end(); }

// ================= EVENT TEXT =================
const char* resetText(uint8_t r, uint8_t& lvl) {
  switch (r) {
    case ESP_RST_POWERON:  return "הדלקת חשמל";
    case ESP_RST_SW:       return "אתחול יזום";
    case ESP_RST_EXT:      return "כפתור RST";
    case ESP_RST_PANIC:    lvl = 2; return "קריסת תוכנה!";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      lvl = 2; return "Watchdog – התוכנה נתקעה!";
    case ESP_RST_BROWNOUT: lvl = 2; return "נפילת מתח!";
    case ESP_RST_DEEPSLEEP:return "יציאה משינה";
    case 11:               return "צריבה / אתחול דרך USB";
    default: {
      static char b[40];
      snprintf(b, sizeof(b), "סיבה לא ידועה (קוד %d)", r);
      return b;
    }
  }
}

String eventText(const Event& e, uint8_t& lvl) {
  lvl = 0;
  const char* srcN[] = { "?", "טלפון", "כפתור", "תזמון", "טלגרם" };
  const char* offN[] = { "?", "הסתיים הזמן", "כיבוי מהטלפון", "כיבוי מהכפתור", "חריגת זמן מקסימלי!",
                         "ניתוק רשת – כיבוי בטיחות!", "כיבוי מטלגרם" };
  switch (e.type) {
    case EV_BOOT:
      return String("הבקר עלה – ") + resetText(e.a, lvl);
    case EV_PUMP_ON:
      return String("משאבה הודלקה (") + srcN[e.a <= 4 ? e.a : 0] + ") ל-" + fmtDur(e.b);
    case EV_PUMP_OFF:
      if (e.a == OFF_TIMEOUT || e.a == OFF_NO_WIFI) lvl = 2;
      return String("משאבה כובתה – ") + offN[e.a <= 6 ? e.a : 0] + " (רצה " + fmtDur(e.b) + ")";
    case EV_REJECT:
      lvl = 1;
      if (e.a == REJ_RUNNING) return String("הדלקה נדחתה – המשאבה כבר פועלת");
      if (e.a == REJ_OFFLINE) return String("הדלקה נדחתה – אין חיבור לרשת");
      return String("הדלקה נדחתה – משך לא חוקי (") + e.b + " שנ')";
    case EV_WIFI_UP:
      return String("WiFi התחבר ל-") + nets[e.a % NUM_NETS].ssid + " (" + String(-(int)e.b) + " dBm)";
    case EV_WIFI_DOWN:
      lvl = 1;
      return String("WiFi התנתק מ-") + nets[e.a % NUM_NETS].ssid;
    case EV_TIME_SYNC:   return String("השעה סונכרנה מהאינטרנט");
    case EV_TIME_PHONE:  return String("השעה סונכרנה מהטלפון");
    case EV_LOG_CLEARED: return String("היומן נוקה");
    case EV_OFFLINE:
      lvl = 2;
      return String("אין רשת אחרי ניסיון בכל הרשתות – המשאבה ננעלה");
    case EV_ONLINE:      return String("החיבור חזר – המשאבה שוחררה");
    case EV_SCHED:
      if (e.a == SR_SKIPPED || e.a == SR_MISS_BUSY) lvl = 1;
      if (e.a == SR_MISS_OFFLINE || e.a == SR_MISS_DOWN) lvl = 2;
      return String("השקיה מתוזמנת ") + schedTimeStr() + " – " + SR_TEXT[e.a <= 7 ? e.a : 0];
    case EV_SCHED_END:
      if (e.a == OFF_DONE) return String("השקיה מתוזמנת הסתיימה – ") + fmtDur(e.b) + " מלאות";
      lvl = 2;
      if (e.a == 0) return String("השקיה מתוזמנת הופסקה – הבקר אותחל באמצע!");
      return String("השקיה מתוזמנת הופסקה – רצה ") + fmtDur(e.b) + " מתוך " + fmtDur(SCHED_SEC) +
             " (" + offN[e.a <= 6 ? e.a : 0] + ")";
    case EV_SKIP:
      return e.a ? String("נקבע דילוג על ההשקיה הבאה") : String("הדילוג בוטל");
  }
  return String("אירוע לא ידוע");
}

String eventTime(const Event& e) {
  if (e.epoch) return fmtEpoch(e.epoch);
  return String("אתחול ") + e.boot + " +" + e.uptime + "s";
}

// ================= TELEGRAM ALERTS =================
void notifyEvent(const Event& e) {
  uint8_t lvl;
  String txt = eventText(e, lvl);
  const char* icon = nullptr;
  switch (e.type) {
    case EV_BOOT:      icon = lvl >= 2 ? "🔴" : "ℹ️"; break;
    case EV_SCHED:     if (e.a != SR_RUNNING) icon = lvl >= 2 ? "🔴" : "🟠"; break;   // start is silent
    case EV_SCHED_END: icon = e.a == OFF_DONE ? "✅" : "🔴"; break;                    // result at the end
    case EV_OFFLINE:   icon = "🔴"; break;
    case EV_ONLINE:    icon = "🟢"; break;
    case EV_PUMP_OFF:  if (e.a == OFF_TIMEOUT) icon = "🔴"; break;
  }
  if (icon) tgNotify(String(icon) + " " + txt + "\n🕒 " + eventTime(e));
}

// ================= EVENT LOG STORAGE =================
void writeHeader(File& f) { f.seek(0); f.write((uint8_t*)&hdr, sizeof(hdr)); }

void createLog() {
  File f = LittleFS.open(LOG_PATH, "w");
  hdr = { LOG_MAGIC, 0, 0 };
  f.write((uint8_t*)&hdr, sizeof(hdr));
  uint8_t zeros[64] = { 0 };
  for (uint32_t n = 0; n < (uint32_t)LOG_CAP * sizeof(Event); n += sizeof(zeros)) f.write(zeros, sizeof(zeros));
  f.close();
}

void logInit() {
  if (!LittleFS.begin(true)) { Serial.println("!! שגיאה: מערכת הקבצים לא עלתה"); return; }
  const size_t expected = sizeof(LogHeader) + (size_t)LOG_CAP * sizeof(Event);
  bool fresh = true;
  if (LittleFS.exists(LOG_PATH)) {
    File f = LittleFS.open(LOG_PATH, "r");
    if (f.size() == expected) {
      f.read((uint8_t*)&hdr, sizeof(hdr));
      fresh = (hdr.magic != LOG_MAGIC || hdr.head >= LOG_CAP || hdr.count > LOG_CAP);
    }
    f.close();
  }
  if (fresh) { Serial.println(">> יוצר יומן חדש"); createLog(); }
  logOk = true;
  Serial.printf(">> יומן: %u אירועים שמורים. שטח פנוי: %u KB\n",
                hdr.count, (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
}

void logEvent(uint8_t type, uint8_t a = 0, uint16_t b = 0) {
  Event e = { timeValid() ? (uint32_t)time(nullptr) : 0, millis() / 1000, bootCount, type, a, b, 0 };
  uint8_t lvl;
  Serial.printf("[אירוע] %s  %s\n", eventTime(e).c_str(), eventText(e, lvl).c_str());
  if (logOk) {
    File f = LittleFS.open(LOG_PATH, "r+");
    if (f) {
      f.seek(sizeof(LogHeader) + (uint32_t)hdr.head * sizeof(Event));
      f.write((uint8_t*)&e, sizeof(e));
      hdr.head = (hdr.head + 1) % LOG_CAP;
      if (hdr.count < LOG_CAP) hdr.count++;
      writeHeader(f);
      f.close();
    }
  }
  notifyEvent(e);
}

bool readEvent(File& f, uint16_t i, Event& e) {   // i = 0 is newest
  if (i >= hdr.count) return false;
  uint16_t idx = (hdr.head + LOG_CAP - 1 - i) % LOG_CAP;
  f.seek(sizeof(LogHeader) + (uint32_t)idx * sizeof(Event));
  return f.read((uint8_t*)&e, sizeof(e)) == sizeof(e);
}

void backfillTimes() {   // give real time to this boot's events logged before sync
  if (!logOk || !timeValid()) return;
  uint32_t nowEp = time(nullptr), nowUp = millis() / 1000;
  File f = LittleFS.open(LOG_PATH, "r+");
  if (!f) return;
  Event e;
  int fixed = 0;
  for (uint16_t i = 0; i < hdr.count; i++) {
    uint16_t idx = (hdr.head + LOG_CAP - 1 - i) % LOG_CAP;
    uint32_t pos = sizeof(LogHeader) + (uint32_t)idx * sizeof(Event);
    f.seek(pos);
    f.read((uint8_t*)&e, sizeof(e));
    if (e.boot != bootCount) break;
    if (e.epoch == 0) {
      e.epoch = nowEp - (nowUp - e.uptime);
      f.seek(pos);
      f.write((uint8_t*)&e, sizeof(e));
      fixed++;
    }
  }
  f.close();
  if (fixed) Serial.printf(">> הושלמה שעה ל-%d אירועים\n", fixed);
}

// ================= PUMP =================
void setOutputs() {
  digitalWrite(PUMP_PIN, pumpOn ? RELAY_ON : RELAY_OFF);
#ifdef RGB_BUILTIN
  rgbLedWrite(RGB_BUILTIN, 0, pumpOn ? 40 : 0, 0);
#endif
}

bool startPump(uint32_t sec, uint8_t src, String* why = nullptr) {
  if (offline) { logEvent(EV_REJECT, REJ_OFFLINE); if (why) *why = "אין חיבור לרשת – המשאבה נעולה"; return false; }
  if (pumpOn)  { logEvent(EV_REJECT, REJ_RUNNING); if (why) *why = "המשאבה כבר פועלת"; return false; }
  if (sec < 1 || sec > MAX_RUN_SEC) {
    logEvent(EV_REJECT, REJ_BADDUR, sec > 65535 ? 65535 : sec);
    if (why) *why = "משך חייב להיות בין שנייה ל-10 דקות";
    return false;
  }
  pumpOn = true;
  pumpSrc = src;
  pumpStartMs = millis();
  pumpDurMs = sec * 1000UL;
  setOutputs();
  logEvent(EV_PUMP_ON, src, sec);
  return true;
}

void stopPump(uint8_t reason) {
  if (!pumpOn) return;
  pumpOn = false;
  setOutputs();
  uint32_t ran = (millis() - pumpStartMs) / 1000;
  logEvent(EV_PUMP_OFF, reason, ran);
  if (pumpSrc == SRC_SCHEDULE) {                    // report the real outcome of the scheduled run
    schedRes = (reason == OFF_DONE) ? SR_DONE : SR_PARTIAL;
    saveSched();
    logEvent(EV_SCHED_END, reason, ran);
  }
}

void pumpSupervisor() {
  digitalWrite(PUMP_PIN, pumpOn ? RELAY_ON : RELAY_OFF);   // continuous enforcement
  if (!pumpOn) return;
  uint32_t el = millis() - pumpStartMs;
  if (el >= pumpDurMs) stopPump(OFF_DONE);
  else if (el >= MAX_RUN_SEC * 1000UL) stopPump(OFF_TIMEOUT);
}

void handleButton() {
  static int lastRaw = HIGH, stable = HIGH;
  static uint32_t lastChange = 0;
  int raw = digitalRead(BTN_PIN);
  if (raw != lastRaw) { lastRaw = raw; lastChange = millis(); }
  if (millis() - lastChange >= DEBOUNCE_MS && raw != stable) {
    stable = raw;
    if (stable == LOW) {
      if (pumpOn) stopPump(OFF_BUTTON);          // stop always works
      else startPump(BTN_RUN_SEC, SRC_BUTTON);
    }
  }
}

// ================= SCHEDULE =================
void scheduler() {
  static uint32_t last = 0;
  static bool waitedOffline = false;
  if (millis() - last < 1000) return;
  last = millis();
  if (!timeValid()) return;

  struct tm lt;
  int32_t today = todayKey(&lt);
  int nowMin = lt.tm_hour * 60 + lt.tm_min;
  int target = schedTarget();

  if (schedDay == SCHED_INIT) {            // first run ever: don't report today as missed
    schedDay = (nowMin > target) ? today : -1;
    schedRes = SR_PENDING;
    saveSched();
    return;
  }
  if (schedDay == today) return;           // already handled today
  if (nowMin < target) { waitedOffline = false; return; }

  uint8_t res;
  if (nowMin == target) {
    if (skipDay == today) { res = SR_SKIPPED; skipDay = -1; saveSkip(); }       // skip is used once
    else if (WiFi.status() != WL_CONNECTED) { waitedOffline = true; return; }   // keep trying this minute
    else if (pumpOn) res = SR_MISS_BUSY;
    else res = startPump(SCHED_SEC, SRC_SCHEDULE) ? SR_RUNNING : SR_MISS_BUSY;
  } else {
    res = waitedOffline ? SR_MISS_OFFLINE : SR_MISS_DOWN;   // the minute passed without a run
  }
  waitedOffline = false;
  schedDay = today;
  schedRes = res;
  saveSched();
  logEvent(EV_SCHED, res);
}

// ================= WIFI =================
void onTimeSync(struct timeval*) { syncFlag = true; }

void onWifiEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    if (r != 8) lastDiscReason = r;    // 8 = our own disconnect, ignore
  }
}

const char* reasonText(uint8_t r) {
  switch (r) {
    case 0:   return "אין מידע";
    case 201: return "הרשת לא נראית באוויר – ה-Hotspot לא משדר";
    case 15:
    case 202:
    case 204: return "סיסמה שגויה או כשל אימות";
    case 203: return "הרשת סירבה לחיבור";
    case 200: return "האות נעלם";
    default:  return "סיבה אחרת";
  }
}

void tryNetwork(int i) {
  netIdx = i;
  attemptStartMs = millis();
  Serial.printf(">> מנסה להתחבר ל-%s...\n", nets[i].ssid);
  WiFi.disconnect(false, false);
  if (nets[i].fixedIp) WiFi.config(FIXED_IP, FIXED_GW, FIXED_MASK, FIXED_GW, DNS_BACKUP);
  else                 WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.begin(nets[i].ssid, nets[i].pass);
}

void wifiSupervisor() {
  static bool was = false;
  bool now = (WiFi.status() == WL_CONNECTED);

  if (now && !was) {
    logEvent(EV_WIFI_UP, netIdx, (uint16_t)(-WiFi.RSSI()));
    Serial.printf(">> פתח בדפדפן:  http://%s\n", WiFi.localIP().toString().c_str());
    configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");   // ask for time now that we have network
  }
  if (!now && was) { logEvent(EV_WIFI_DOWN, netIdx); attemptStartMs = millis(); }
  was = now;

  if (now) {
    lostTiming = false;
    if (offline) { offline = false; logEvent(EV_ONLINE); }
    return;
  }

  if (!lostTiming) { lostTiming = true; lostSinceMs = millis(); }
  if (!offline && millis() - lostSinceMs >= OFFLINE_AFTER_MS) {
    offline = true;
    stopPump(OFF_NO_WIFI);
    logEvent(EV_OFFLINE);
  }
  if (millis() - attemptStartMs >= NET_TRY_MS) {
    Serial.printf(">> %s נכשל: %s (קוד %u)\n", nets[netIdx].ssid, reasonText(lastDiscReason), lastDiscReason);
    lastDiscReason = 0;
    tryNetwork((netIdx + 1) % NUM_NETS);
  }
}

// ================= TELEGRAM COMMANDS (run in main loop) =================
String statusText() {
  String s = "💧 מצב הבקר\n\n";
  if (pumpOn) {
    uint32_t el = millis() - pumpStartMs;
    s += "משאבה: פועלת (נותרו " + fmtDur(pumpDurMs > el ? (pumpDurMs - el) / 1000 : 0) + ")\n";
  } else {
    s += "משאבה: כבויה\n";
  }
  s += "רשת: " + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)\n";
  s += "שעה: " + (timeValid() ? fmtEpoch(time(nullptr)) : String("לא מסונכרנת")) + "\n";
  s += "השקיה יומית: " + schedTimeStr() + " למשך " + fmtDur(SCHED_SEC) + "\n";
  if (timeValid()) {
    bool handled = (schedDay == todayKey());
    s += String("היום: ") + (handled ? SR_TEXT[schedRes <= 7 ? schedRes : 0] : "ממתינה") + "\n";
    if (skipDay == nextSchedDay()) s += "⏭ ההשקיה הבאה תדולג\n";
  }
  s += "פועל ברציפות: " + fmtUptime(millis() / 1000) + "\n";
  s += "\nאירועים אחרונים:\n";
  if (logOk) {
    File f = LittleFS.open(LOG_PATH, "r");
    Event e;
    uint8_t lvl;
    for (int i = 0; i < 5 && readEvent(f, i, e); i++)
      s += "• " + (e.epoch ? fmtShort(e.epoch) : String("לפני סנכרון")) + "  " + eventText(e, lvl) + "\n";
    f.close();
  }
  return s;
}

String helpText() {
  String s = "🤖 בקר ההשקיה\n\n/status – מצב ואירועים אחרונים\n";
#if TG_REMOTE_CONTROL
  s += "/on 5 – הדלקה ל-5 דקות (עד 10)\n/off – כיבוי\n";
  s += "/skip – דילוג על ההשקיה הבאה\n/unskip – ביטול הדילוג\n";
#else
  s += "\nשליטה מרחוק כבויה בהגדרות.";
#endif
  return s;
}

void processTgCommands() {
  if (!tgCmdQ) return;
  TgCmd c;
  while (xQueueReceive(tgCmdQ, &c, 0) == pdTRUE) {
    if (c.type == CMD_STATUS) tgNotify(statusText());
#if TG_REMOTE_CONTROL
    else if (c.type == CMD_ON) {
      String why;
      if (startPump(c.sec, SRC_TELEGRAM, &why)) tgNotify("💧 המשאבה הודלקה ל-" + fmtDur(c.sec));
      else tgNotify("⛔ " + why);
    }
    else if (c.type == CMD_OFF) {
      bool was = pumpOn;
      stopPump(OFF_TELEGRAM);
      tgNotify(was ? "⏹ המשאבה כובתה" : "המשאבה כבר כבויה");
    }
    else if (c.type == CMD_SKIP || c.type == CMD_UNSKIP) {
      if (!timeValid()) { tgNotify("⛔ השעה עדיין לא סונכרנה"); continue; }
      bool on = (c.type == CMD_SKIP);
      skipDay = on ? nextSchedDay() : -1;
      saveSkip();
      logEvent(EV_SKIP, on ? 1 : 0);
      tgNotify(on ? "⏭ ההשקיה הבאה תדולג" : "▶️ הדילוג בוטל");
    }
#endif
    else tgNotify(helpText());
  }
}

// ================= WEB API =================
void sendText(int code, const String& t) { server.send(code, "text/plain; charset=utf-8", t); }

void setupRoutes() {
  server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", INDEX_HTML); });

  server.on("/api/status", HTTP_GET, []() {
    uint32_t el = pumpOn ? millis() - pumpStartMs : 0;
    bool c = WiFi.status() == WL_CONNECTED;
    bool tv = timeValid();
    bool handled = tv && schedDay == todayKey();
    bool skipNext = tv && skipDay == nextSchedDay();

    String j = "{\"pump\":";
    j += pumpOn ? "true" : "false";
    j += ",\"src\":" + String(pumpSrc);
    j += ",\"remaining\":" + String(pumpOn && pumpDurMs > el ? (pumpDurMs - el) / 1000 : 0);
    j += ",\"timeValid\":"; j += tv ? "true" : "false";
    j += ",\"epoch\":" + String((uint32_t)time(nullptr));
    j += ",\"time\":\"" + (tv ? fmtEpoch(time(nullptr)) : String("לא מסונכרנת")) + "\"";
    j += ",\"ssid\":\"" + jsonEsc(c ? WiFi.SSID() : String("לא מחובר")) + "\"";
    j += ",\"ip\":\"" + (c ? WiFi.localIP().toString() : String("-")) + "\"";
    j += ",\"rssi\":" + String(c ? WiFi.RSSI() : 0);
    j += ",\"uptime\":" + String(millis() / 1000);
    j += ",\"boot\":" + String(bootCount);
    j += ",\"logCount\":" + String(hdr.count) + ",\"logCap\":" + String(LOG_CAP);
    j += ",\"logHead\":" + String(hdr.head);
    j += ",\"sched\":{\"time\":\"" + schedTimeStr() + "\",\"dur\":" + String(SCHED_SEC);
    j += ",\"handled\":"; j += handled ? "true" : "false";
    j += ",\"res\":" + String(handled ? schedRes : 0);
    j += ",\"skip\":"; j += skipNext ? "true" : "false";
    j += "}}";
    server.send(200, "application/json; charset=utf-8", j);
  });

  server.on("/api/pump", HTTP_POST, []() {
    String a = server.arg("action");
    if (a == "off") { stopPump(OFF_WEB); sendText(200, "OK"); return; }
    if (a == "on") {
      String why;
      if (startPump(server.arg("sec").toInt(), SRC_WEB, &why)) sendText(200, "OK");
      else sendText(409, why);
      return;
    }
    sendText(400, "פעולה לא חוקית");
  });

  server.on("/api/skip", HTTP_POST, []() {
    if (!timeValid()) { sendText(409, "השעה עדיין לא סונכרנה"); return; }
    bool on = server.arg("on").toInt() != 0;
    skipDay = on ? nextSchedDay() : -1;
    saveSkip();
    logEvent(EV_SKIP, on ? 1 : 0);
    sendText(200, "OK");
  });

  server.on("/api/time", HTTP_POST, []() {
    long long ep = atoll(server.arg("epoch").c_str());
    if (ep < 1700000000LL) { sendText(400, "זמן לא חוקי"); return; }
    bool wasValid = timeValid();
    long long diff = ep - (long long)time(nullptr);
    if (wasValid && diff > -30 && diff < 30) { sendText(200, "OK"); return; }   // already close enough
    struct timeval tv = { (time_t)ep, 0 };
    settimeofday(&tv, nullptr);
    if (!wasValid) backfillTimes();
    logEvent(EV_TIME_PHONE);
    sendText(200, "OK");
  });

  server.on("/api/events", HTTP_GET, []() {
    int off = server.arg("offset").toInt();
    int lim = server.arg("limit").toInt();
    if (off < 0) off = 0;
    if (lim <= 0 || lim > 100) lim = 50;
    String j;
    j.reserve(lim * 120 + 40);
    j = "{\"total\":" + String(hdr.count) + ",\"events\":[";
    if (logOk) {
      File f = LittleFS.open(LOG_PATH, "r");
      Event e;
      uint8_t lvl;
      for (int i = off; i < off + lim && readEvent(f, i, e); i++) {
        if (i > off) j += ",";
        String txt = eventText(e, lvl);
        j += "{\"t\":\"" + jsonEsc(eventTime(e)) + "\",\"x\":\"" + jsonEsc(txt) + "\",\"l\":" + String(lvl) + "}";
      }
      f.close();
    }
    j += "]}";
    server.send(200, "application/json; charset=utf-8", j);
  });

  server.on("/api/clear", HTTP_POST, []() {
    if (logOk) { createLog(); logEvent(EV_LOG_CLEARED); }
    sendText(200, "OK");
  });

  server.onNotFound([]() { sendText(404, "לא נמצא"); });
}

// ================= SERIAL =================
void printLast(int n) {
  if (!logOk) return;
  File f = LittleFS.open(LOG_PATH, "r");
  Event e;
  uint8_t lvl;
  Serial.printf("----- %d אירועים אחרונים (מתוך %u) -----\n", n, hdr.count);
  for (int i = n - 1; i >= 0; i--)
    if (readEvent(f, i, e)) Serial.printf("%s  %s\n", eventTime(e).c_str(), eventText(e, lvl).c_str());
  f.close();
}

void handleSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 's') Serial.printf("משאבה: %s | רשת: %s | נעול: %s | אירועים: %u\n",
                              pumpOn ? "פועלת" : "כבויה",
                              WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "לא מחובר",
                              offline ? "כן" : "לא", hdr.count);
  if (c == 'l') printLast(20);
  if (c == 'x') {
    if (!timeValid()) { Serial.println(">> אין שעה עדיין – אי אפשר לקבוע בדיקה"); return; }
    struct tm lt;
    todayKey(&lt);
    testTargetMin = (lt.tm_hour * 60 + lt.tm_min + 2) % 1440;
    schedDay = -1;                 // RAM only - a reboot restores the real state
    schedRes = SR_PENDING;
    Serial.printf(">> בדיקה: השקיה ב-%s (בעוד 2 דק'). אתחול מחזיר ל-%02u:%02u\n",
                  schedTimeStr().c_str(), SCHED_HOUR, SCHED_MIN);
  }
  if (c == 'r') { stopPump(OFF_WEB); delay(200); ESP.restart(); }
}

// ================= SETUP / LOOP =================
void setup() {
  digitalWrite(PUMP_PIN, RELAY_OFF);   // pump OFF before anything else
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, RELAY_OFF);
  pinMode(BTN_PIN, INPUT_PULLUP);
#ifdef RGB_BUILTIN
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
#endif

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  delay(300);
  Serial.println("\n\n===== בקר השקיה: התראות ושליטה מרחוק בטלגרם =====");
  Serial.printf("כתובת MAC: %s\n", macStr().c_str());

  setenv("TZ", TZ_INFO, 1);   // timezone before any logging
  tzset();

  prefs.begin("sys", false);
  bootCount = prefs.getUShort("boots", 0) + 1;
  prefs.putUShort("boots", bootCount);
  schedDay = prefs.isKey("sday") ? prefs.getInt("sday", -1) : SCHED_INIT;
  schedRes = prefs.getUChar("sres", SR_PENDING);
  skipDay  = prefs.getInt("skip", -1);
  prefs.end();

  tgBegin();                  // before the first event, so the boot alert is queued
  logInit();
  logEvent(EV_BOOT, (uint8_t)esp_reset_reason());
  if (schedRes == SR_RUNNING) {          // power was lost in the middle of a scheduled run
    schedRes = SR_PARTIAL;
    saveSched();
    logEvent(EV_SCHED_END, 0, 0);
  }

  WiFi.setHostname(DEVICE_NAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);       // keep radio awake - more stable with iPhone hotspot
  WiFi.onEvent(onWifiEvent);
  WiFi.setAutoReconnect(true);
  tryNetwork(0);
  sntp_set_time_sync_notification_cb(onTimeSync);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  MDNS.begin(DEVICE_NAME);
  MDNS.addService("http", "tcp", 80);
  setupRoutes();
  server.begin();
  Serial.printf(">> השקיה יומית: %s למשך %s\n", schedTimeStr().c_str(), fmtDur(SCHED_SEC).c_str());
}

void loop() {
  pumpSupervisor();      // safety first
  wifiSupervisor();      // may lock the pump
  handleButton();
  scheduler();
  processTgCommands();
  server.handleClient();

  if (syncFlag) {
    syncFlag = false;
    if (!firstSyncLogged) { firstSyncLogged = true; backfillTimes(); logEvent(EV_TIME_SYNC); }
  }
  handleSerial();
}
