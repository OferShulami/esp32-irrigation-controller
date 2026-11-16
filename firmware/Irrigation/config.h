#pragma once
#include <Arduino.h>
#include <IPAddress.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing secrets.h - copy secrets.example.h to secrets.h and fill in your WiFi and Telegram values"
#endif

// ================= NETWORKS (2.4GHz) - tried in order =================
struct Net { const char* ssid; const char* pass; bool fixedIp; };
static Net nets[] = {
  { WIFI1_SSID, WIFI1_PASS, true },   // iPhone hotspot -> fixed address
  { WIFI2_SSID, WIFI2_PASS, true },
};
static const int      NUM_NETS         = sizeof(nets) / sizeof(nets[0]);
static const uint32_t NET_TRY_MS       = 15000;                         // per attempt
static const uint32_t OFFLINE_AFTER_MS = NET_TRY_MS * (NUM_NETS + 1);   // ~45s -> pump lock

// Fixed address inside any iPhone hotspot
static const IPAddress FIXED_IP(172, 20, 10, 14);
static const IPAddress FIXED_GW(172, 20, 10, 1);
static const IPAddress FIXED_MASK(255, 255, 255, 240);
static const IPAddress DNS_BACKUP(8, 8, 8, 8);

// ================= DEVICE =================
static const char* DEVICE_NAME = "irrigation";
static const char* TZ_INFO     = "IST-2IDT,M3.4.4/26,M10.5.0";   // Israel incl. DST

// ================= HARDWARE =================
#define PUMP_PIN   4
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH
#define BTN_PIN    0          // onboard BOOT button for now

// ================= SAFETY =================
static const uint32_t MAX_RUN_SEC = 600;   // hard 10-minute cap
static const uint32_t BTN_RUN_SEC = 300;
static const uint32_t DEBOUNCE_MS = 250;

// ================= DAILY SCHEDULE =================
static const uint8_t  SCHED_HOUR = 7;
static const uint8_t  SCHED_MIN  = 30;
static const uint16_t SCHED_SEC  = 300;    // 5 minutes

// ================= EVENT LOG =================
static const uint16_t LOG_CAP = 500;

// ================= TELEGRAM =================
#define TG_ENABLED        1
#define TG_REMOTE_CONTROL 1                // 1 = allow /on, /off, /skip from Telegram (owner chat only)
static const char*    TG_TOKEN    = SECRET_TG_TOKEN;
static const char*    TG_CHAT_ID  = SECRET_TG_CHAT_ID;
static const uint16_t TG_POLL_SEC = 30;    // how long each "any new commands?" request waits
