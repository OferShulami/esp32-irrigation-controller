#pragma once
// Telegram transport: two tasks on core 0 (sender + receiver).
// Never touches the pump - only moves messages out and commands in.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "config.h"

enum TgCmdType : uint8_t { CMD_STATUS = 1, CMD_HELP, CMD_ON, CMD_OFF, CMD_SKIP, CMD_UNSKIP };
struct TgCmd { uint8_t type; uint16_t sec; };
struct TgMsg { char text[1500]; };

static QueueHandle_t tgOutQ = nullptr;   // messages waiting to be sent
static QueueHandle_t tgCmdQ = nullptr;   // commands waiting for the main loop
static SemaphoreHandle_t tgTlsLock = nullptr;   // TLS start-up one at a time (hardware AES init is not thread-safe)

static WiFiClientSecure* tgNewClient() {
  xSemaphoreTake(tgTlsLock, portMAX_DELAY);
  WiFiClientSecure* c = new WiFiClientSecure();
  c->setInsecure();                     // encrypted, but server certificate not verified
  xSemaphoreGive(tgTlsLock);
  return c;
}

static bool tgConfigured() {
  return TG_ENABLED && strncmp(TG_TOKEN, "PASTE", 5) != 0 && strncmp(TG_CHAT_ID, "PASTE", 5) != 0;
}

// Called from the main loop: queue a message (never blocks)
static void tgNotify(const String& s) {
  if (!tgOutQ) return;
  static TgMsg m;
  size_t n = s.length();
  if (n >= sizeof(m.text)) {                       // cut safely on a UTF-8 boundary
    n = sizeof(m.text) - 1;
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
  }
  memcpy(m.text, s.c_str(), n);
  m.text[n] = 0;
  if (xQueueSend(tgOutQ, &m, 0) != pdTRUE) Serial.println("!! תור הטלגרם מלא – הודעה נזרקה");
}

static String tgUrlEncode(const char* s) {
  String o;
  char h[4];
  for (; *s; s++) {
    uint8_t c = *s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { snprintf(h, sizeof(h), "%%%02X", c); o += h; }
  }
  return o;
}

static void tgHandleText(String t) {
  t.trim();
  TgCmd c = { CMD_HELP, 0 };
  if (t.startsWith("/status")) c.type = CMD_STATUS;
#if TG_REMOTE_CONTROL
  else if (t.startsWith("/on")) {
    int m = t.substring(3).toInt();
    if (m <= 0) m = 5;
    if (m > 60) m = 60;                            // main loop still enforces the 10-min cap
    c.type = CMD_ON;
    c.sec = m * 60;
  }
  else if (t.startsWith("/off"))    c.type = CMD_OFF;
  else if (t.startsWith("/skip"))   c.type = CMD_SKIP;
  else if (t.startsWith("/unskip")) c.type = CMD_UNSKIP;
#endif
  xQueueSend(tgCmdQ, &c, 0);
}

static void tgParse(const String& b, long& offset, bool ignore) {
  const char* key = "\"update_id\":";
  const int klen = strlen(key);
  int p = b.indexOf(key);
  while (p >= 0) {
    int start = p + klen;
    offset = atol(b.c_str() + start) + 1;
    int next = b.indexOf(key, start);
    if (!ignore) {
      String u = b.substring(start, next < 0 ? b.length() : next);
      int c = u.indexOf("\"chat\":{\"id\":");
      int t = u.indexOf("\"text\":\"");
      if (c >= 0 && t >= 0 && atoll(u.c_str() + c + 13) == atoll(TG_CHAT_ID)) {   // owner only
        int e = u.indexOf('"', t + 8);
        tgHandleText(u.substring(t + 8, e < 0 ? u.length() : e));
      }
    }
    p = next;
  }
}

// ---- Sender: sleeps until a message is queued, sends it immediately ----
static void tgSendTask(void*) {
  WiFiClientSecure* client = tgNewClient();
  HTTPClient http;
  http.setReuse(true);
  const String url = String("https://api.telegram.org/bot") + TG_TOKEN + "/sendMessage";
  static TgMsg m;
  bool warm = false;                    // first connection is made under the lock

  for (;;) {
    xQueueReceive(tgOutQ, &m, portMAX_DELAY);          // wait until there is something to send
    for (;;) {
      while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(1000));
      if (!warm) xSemaphoreTake(tgTlsLock, portMAX_DELAY);
      http.setTimeout(15000);
      http.begin(*client, url);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      int code = http.POST(String("chat_id=") + TG_CHAT_ID + "&text=" + tgUrlEncode(m.text));
      http.end();
      if (!warm) { warm = true; xSemaphoreGive(tgTlsLock); }
      if (code == 200) break;
      Serial.printf("!! טלגרם: שליחה נכשלה (קוד %d)\n", code);
      if (code >= 400 && code < 500 && code != 429) break;   // bad token/message: drop it
      vTaskDelay(pdMS_TO_TICKS(5000));                      // network problem: retry same message
    }
  }
}

// ---- Receiver: long polling - returns the instant a command arrives ----
static void tgPollTask(void*) {
  WiFiClientSecure* client = tgNewClient();
  HTTPClient http;
  http.setReuse(true);
  const String base = String("https://api.telegram.org/bot") + TG_TOKEN;
  long offset = -1;                     // first request: skip old messages
  bool first = true, warm = false;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    String url = base + "/getUpdates?timeout=" + String(first ? 0 : TG_POLL_SEC) + "&offset=" + String(offset);
    if (!warm) xSemaphoreTake(tgTlsLock, portMAX_DELAY);
    http.setTimeout((TG_POLL_SEC + 10) * 1000);
    http.begin(*client, url);
    int code = http.GET();
    String body = (code == 200) ? http.getString() : String();
    http.end();
    if (!warm) { warm = true; xSemaphoreGive(tgTlsLock); }
    if (code == 200) {
      tgParse(body, offset, first);
      if (first && offset < 0) offset = 0;
      first = false;
    } else {
      Serial.printf("!! טלגרם: קבלה נכשלה (קוד %d)%s\n", code, code == 401 ? " – ה-Token שגוי" : "");
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}

static void tgBegin() {
  if (!tgConfigured()) { Serial.println(">> טלגרם כבוי (חסר Token או Chat ID ב-config.h)"); return; }
  tgOutQ    = xQueueCreate(8, sizeof(TgMsg));
  tgCmdQ    = xQueueCreate(4, sizeof(TgCmd));
  tgTlsLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(tgSendTask, "tg_send", 16384, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(tgPollTask, "tg_poll", 16384, nullptr, 1, nullptr, 0);
  Serial.printf(">> טלגרם פעיל. שליטה מרחוק: %s\n", TG_REMOTE_CONTROL ? "פעילה" : "כבויה");
}
