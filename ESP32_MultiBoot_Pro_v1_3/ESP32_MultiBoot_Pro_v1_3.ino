/*
 * ESP32 MultiBoot Manager
 * Dual-slot persistent boot manager — fully offline, WiFi Access Point only.
 *
 * Lives in the FACTORY partition. Manages two OTA slots (A and B), each
 * holding one application binary. A .bin file is uploaded directly from a
 * phone/PC connected to this device's own AP and streamed straight into the
 * target slot's flash as it arrives. The manager always boots first; it
 * checks NVS for a boot target and either stays as manager or chains into
 * the selected slot. Hold GPIO 0 LOW at power-on to force manager mode.
 *
 * Partition table (partitions_multiboot.csv):
 *   nvs,     data, nvs,     0x9000,   0x5000
 *   otadata, data, ota,     0xe000,   0x2000
 *   manager, app,  factory, 0x10000,  0x100000
 *   slot_a,  app,  ota_0,   0x110000, 0x130000
 *   slot_b,  app,  ota_1,   0x240000, 0x130000
 *   spiffs,  data, spiffs,  0x370000, 0x28000
 *
 * Board: ESP32 Dev Module | Partition Scheme: Custom | Flash: 4MB
 */


// ═══════════════════════════════════════════════════════════════════════════
//   INCLUDES
// ═══════════════════════════════════════════════════════════════════════════
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <ctype.h>
#include <string.h>
#include <stdarg.h>

// Version guard for Core 2.x / Core 3.x API differences
#ifndef ESP_ARDUINO_VERSION
  #define ESP_ARDUINO_VERSION 0
#endif
#ifndef ESP_ARDUINO_VERSION_VAL
  #define ESP_ARDUINO_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))
#endif

// ═══════════════════════════════════════════════════════════════════════════
//   FORWARD TYPE DEFINITIONS
//   NOTE: Arduino's ctags-based prototype generator hoists a forward
//   declaration for every function to a point immediately after this
//   include block — BEFORE any of our own code further down. Any function
//   whose signature (param or return type) uses a custom struct/enum will
//   fail to compile with "'X' does not name a type" unless that type is
//   already defined up here, ahead of the hoisted prototypes. Keep these
//   type definitions here — do NOT move them back down next to their
//   usage sites.
// ═══════════════════════════════════════════════════════════════════════════
struct LoginAttempt {
  uint32_t ip        = 0;
  uint8_t  fails     = 0;
  uint32_t lockUntil = 0;   // millis() timestamp, 0 = not locked
};

enum SlotStatus { SLOT_EMPTY, SLOT_VALID };

// ═══════════════════════════════════════════════════════════════════════════
//   CONFIG — Edit these
// ═══════════════════════════════════════════════════════════════════════════
// V1.5: fully offline. This device NEVER joins another WiFi network — it
// always runs its own Access Point, so it works anywhere with no router/
// internet dependency. SSID/password below are only the FIRST-BOOT defaults;
// once changed from the Settings panel, the values actually used live in
// NVS and these consts are never read again.
const char* AP_SSID_DEFAULT = "ESP32-BootManager";
const char* AP_PASS_DEFAULT = "bootmgr-ap-2026";

// Web UI Basic Auth credentials (dashboard login)
const char* AUTH_USER       = "admin";
const char* AUTH_PASS       = "esp32boot";

// V1.5: separate password specifically gating the "Update Manager Firmware"
// action in Settings — deliberately independent of AUTH_PASS/AP_PASS so
// compromising the dashboard login alone can't be used to overwrite the
// manager itself. This is only the first-boot default; the Settings panel's
// "Change OTA Password" flow (old → new → confirm) moves the real value
// into NVS from then on.
const char* OTA_PASS_DEFAULT = "changeme-ota-2026";

// ═══════════════════════════════════════════════════════════════════════════
//   PIN DEFINITIONS
// ═══════════════════════════════════════════════════════════════════════════
// Newer esp32-hal.h in the Arduino core already #defines BOOT_PIN (to the
// same GPIO 0) — guard so we don't trigger a harmless-but-noisy redefinition
// warning on cores that provide it.
#ifndef BOOT_PIN
#define BOOT_PIN 0   // Hold LOW at power-on to force manager mode
#endif

// ═══════════════════════════════════════════════════════════════════════════
//   CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════
#define PREF_NS          "mbmgr"
#define PREF_BOOT_TARGET "boot_tgt"
#define PREF_SLOT_A_NAME "slot_a_nm"
#define PREF_SLOT_B_NAME "slot_b_nm"
// Same key ReturnToManager.h's rtm_bootSafetyCheck() writes/reads in
// the "mbmgr" namespace — manager resets it to 0 on any manually-requested
// boot so the slot app gets a fresh unconfirmed-boot budget.
#define PREF_BOOT_FAILS  "boot_fails"
#define PREF_AP_SSID     "ap_ssid"      // V1.5: user-configurable AP identity
#define PREF_AP_PASS     "ap_pass"
#define PREF_OTA_PASS    "ota_pass"     // V1.5: manager-self-update gate password
// V1.5: two-stage safe manager self-update (see checkPendingPromotion()).
// NVS key names are capped at 15 chars by the Preferences/NVS API — both
// of these are well under that limit.
#define PREF_PEND_PROMOTE "pend_promo"  // uint8: 1 = a promotion copy is due on this boot
#define PREF_PEND_LEN     "pend_len"    // uint32: exact byte length to copy
#define OTA_MAGIC_BYTE   0xE9        // Valid ESP32 image first byte
#define DNS_PORT         53          // Captive-portal DNS (AP mode only)
#define MIN_FW_SIZE      65536       // Reject anything smaller as "not real firmware"

// Boot targets stored in NVS
#define TARGET_MANAGER "manager"
#define TARGET_SLOT_A  "slot_a"
#define TARGET_SLOT_B  "slot_b"

// ═══════════════════════════════════════════════════════════════════════════
//   GLOBALS
// ═══════════════════════════════════════════════════════════════════════════
WebServer   server(80);
DNSServer   dnsServer;   // Captive portal: only started/serviced in AP mode
Preferences prefs;
bool        apMode         = false;
String      slotAName      = "";   // Name of binary currently in slot A
String      slotBName      = "";   // Name of binary currently in slot B
String      apSsid;                // Loaded from NVS in setup() (falls back to AP_SSID_DEFAULT)
String      apPass;                // Loaded from NVS in setup() (falls back to AP_PASS_DEFAULT)
String      otaUpdatePass;         // Loaded from NVS in setup() (falls back to OTA_PASS_DEFAULT)

// ─── Shared mutex — guards every prefs.* access so the upload/flash ───────
// handler and the synchronous WebServer handlers in loop() never touch NVS
// at the same instant.
SemaphoreHandle_t ioMutex = NULL;
bool lockIO(uint32_t timeoutMs = 3000) {
  if (!ioMutex) return true;  // not yet created (very early boot) — no contention possible
  return xSemaphoreTake(ioMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}
void unlockIO() {
  if (ioMutex) xSemaphoreGive(ioMutex);
}

// ─── Guards against a second upload/flash request while one is already ───
// is already streaming into an OTA partition.
volatile bool flashInProgress = false;

// ─── Same pattern as flashInProgress, for the async erase task ───────────
volatile bool eraseInProgress = false;

// ─── Lightweight per-IP brute-force lockout for Basic Auth ───────────────
// (struct LoginAttempt is defined near the top of the file — see the
// "FORWARD TYPE DEFINITIONS" block — so it's already known before Arduino's
// hoisted function prototypes need it.)
#define LOGIN_TRACK_SLOTS   8
#define LOGIN_MAX_FAILS     5
#define LOGIN_LOCK_MS       30000UL
LoginAttempt loginTrack[LOGIN_TRACK_SLOTS];

// Recycle a genuinely idle slot (never used, or used-but-clean)
// first. Only fall back to evicting a currently-locked slot — picking the
// one soonest to expire — as a last resort. The old version preferred
// evicting UNLOCKED slots outright, which let an attacker rotating more
// than LOGIN_TRACK_SLOTS distinct source IPs keep landing in fresh slots
// and reset their own fail counter for free, defeating the lockout.
LoginAttempt* findLoginSlot(uint32_t ip) {
  int idleIdx = -1, lockedIdx = -1;
  uint32_t soonestUnlock = 0xFFFFFFFF;
  for (int i = 0; i < LOGIN_TRACK_SLOTS; i++) {
    if (loginTrack[i].ip == ip) return &loginTrack[i];
    if (loginTrack[i].ip == 0 || (loginTrack[i].fails == 0 && loginTrack[i].lockUntil == 0)) {
      if (idleIdx < 0) idleIdx = i;
    } else if (loginTrack[i].lockUntil > 0 && loginTrack[i].lockUntil < soonestUnlock) {
      soonestUnlock = loginTrack[i].lockUntil;
      lockedIdx = i;
    }
  }
  int useIdx = (idleIdx >= 0) ? idleIdx : (lockedIdx >= 0 ? lockedIdx : 0);
  loginTrack[useIdx] = LoginAttempt();
  loginTrack[useIdx].ip = ip;
  return &loginTrack[useIdx];
}

// ─── Slot status ────────────────────────────────────────────────────────────
// There used to be a third state, SLOT_ACTIVE, for "this slot is the
// currently running partition." That state can never actually be observed:
// this manager binary lives permanently in the factory partition and is the
// only code that ever calls getSlotStatus() — esp_ota_get_running_partition()
// will therefore always be the factory partition while this function runs,
// never slot_a or slot_b. A slot only ever becomes "running" once its own
// app has booted and taken over — at which point this manager code, and this
// function, are not executing at all. Keeping SLOT_ACTIVE around implied the
// manager could see its own slots running, which it structurally cannot.
// (enum SlotStatus itself is defined near the top of the file — see the
// "FORWARD TYPE DEFINITIONS" block.)
SlotStatus getSlotStatus(const char* partName) {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, partName);
  if (!part) return SLOT_EMPTY;

  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(part, &state) != ESP_OK) return SLOT_EMPTY;

  if (state == ESP_OTA_IMG_VALID       ||
      state == ESP_OTA_IMG_PENDING_VERIFY ||
      state == ESP_OTA_IMG_UNDEFINED) {
    // Check magic byte to verify something is actually written
    uint8_t magic = 0;
    esp_partition_read(part, 0, &magic, 1);
    return (magic == OTA_MAGIC_BYTE) ? SLOT_VALID : SLOT_EMPTY;
  }
  return SLOT_EMPTY;
}

// ─── Auth helper — per-IP brute-force lockout ─────────────────────────────
bool checkAuth() {
  uint32_t ip = (uint32_t)server.client().remoteIP();
  LoginAttempt* slot = findLoginSlot(ip);

  uint32_t now = millis();
  if (slot->lockUntil != 0 && now < slot->lockUntil) {
    uint32_t remainSec = (slot->lockUntil - now) / 1000 + 1;
    server.send(429, "text/plain",
                "Too many failed login attempts. Try again in " + String(remainSec) + "s.");
    return false;
  }
  // Lock window expired — reset the counter
  if (slot->lockUntil != 0 && now >= slot->lockUntil) {
    slot->fails = 0;
    slot->lockUntil = 0;
  }

  if (!server.authenticate(AUTH_USER, AUTH_PASS)) {
    slot->fails++;
    if (slot->fails >= LOGIN_MAX_FAILS) {
      slot->lockUntil = now + LOGIN_LOCK_MS;
      Serial.printf("[AUTH] IP locked out for %lus after %u failed attempts\n",
                    LOGIN_LOCK_MS / 1000, slot->fails);
      server.send(429, "text/plain", "Too many failed login attempts. Try again in 30s.");
      return false;
    }
    server.requestAuthentication(BASIC_AUTH, "MultiBoot Manager", "Auth required");
    return false;
  }
  // Successful auth — clear any accumulated failures for this IP
  slot->fails = 0;
  slot->lockUntil = 0;
  return true;
}

// V1.5: constant-time string compare for the separate OTA-update password
// (dashboard Basic Auth already gets this from WebServer's own authenticate()
// internals; this covers the plain-string comparisons the new Settings
// routes below do against values stored in NVS).
bool secureEquals(const String& a, const String& b) {
  size_t lenA = a.length(), lenB = b.length();
  size_t maxLen = (lenA > lenB) ? lenA : lenB;
  uint8_t diff = (lenA == lenB) ? 0 : 1;
  for (size_t i = 0; i < maxLen; i++) {
    uint8_t ca = (i < lenA) ? (uint8_t)a[i] : 0;
    uint8_t cb = (i < lenB) ? (uint8_t)b[i] : 0;
    diff |= (ca ^ cb);
  }
  return diff == 0;
}

// ─── CSRF guard — exact host match, fail-closed ───────────────────────────
// Basic-Auth credentials, once cached by a browser, are auto-attached to any
// same-origin-looking simple POST request — including one fired from a page
// on a completely different site while the admin has this device open in
// another tab. Reject state-changing requests whose Origin/Referer host
// doesn't EXACTLY match this device's own current IP.
//
// Extracts just the "host[:port]" component from a URL, e.g.
//   "http://192.168.1.50/app/page?x=1"  ->  "192.168.1.50"
//   "http://192.168.1.50:80/x"          ->  "192.168.1.50:80"
// Deliberately does NOT use indexOf()/substring containment anywhere —
// an attacker-controlled Referer path/query like
// "http://evil.com/192.168.1.50/x.html" contains the device IP as a
// substring without actually being it.
String extractHost(const String& url) {
  int start = url.indexOf("://");
  start = (start < 0) ? 0 : start + 3;
  int pathStart  = url.indexOf('/', start);
  int queryStart = url.indexOf('?', start);
  int end = url.length();
  if (pathStart  >= 0 && pathStart  < end) end = pathStart;
  if (queryStart >= 0 && queryStart < end) end = queryStart;
  String host = url.substring(start, end);
  host.trim();
  return host;
}

bool checkOrigin() {
  String origin = server.header("Origin");
  bool fromReferer = false;
  if (origin.length() == 0) { origin = server.header("Referer"); fromReferer = true; }

  // Fail CLOSED. A browser making a real same-origin request to this
  // page always sends at least one of these headers. Absence of both on a
  // state-changing route is treated as suspicious rather than trusted.
  if (origin.length() == 0) {
    Serial.println("[CSRF] Rejected — no Origin or Referer header present (fail-closed)");
    server.send(403, "text/plain", "Cross-origin request rejected (missing Origin)");
    return false;
  }

  String host = extractHost(origin);
  String selfIp = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  bool match = (host == selfIp) || (host == selfIp + ":80");
  if (!match) {
    Serial.println("[CSRF] Rejected — " + String(fromReferer ? "Referer" : "Origin") +
                    " host mismatch: '" + host + "' != '" + selfIp + "'");
    server.send(403, "text/plain", "Cross-origin request rejected");
    return false;
  }
  return true;
}

// ─── Human-readable file size ───────────────────────────────────────────────
String fmtSize(size_t bytes) {
  if (bytes < 1024)     return String(bytes) + " B";
  if (bytes < 1048576)  return String(bytes / 1024.0f, 1) + " KB";
  return String(bytes / 1048576.0f, 2) + " MB";
}

// ─── Shared filename sanitizer ─────────────────────────────────────────────
// Strips path components (traversal) AND whitelists characters so a crafted
// filename can never break out of the onclick="..." JS string it's embedded
// into on the dashboard. Returns "" if nothing safe survives.
String sanitizeFilename(String fname) {
  if (fname.indexOf('/') >= 0)  fname = fname.substring(fname.lastIndexOf('/') + 1);
  if (fname.indexOf('\\') >= 0) fname = fname.substring(fname.lastIndexOf('\\') + 1);
  fname.replace("..", "");
  String clean = "";
  for (size_t i = 0; i < fname.length(); i++) {
    char c = fname.charAt(i);
    if (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') clean += c;
  }
  return clean;
}

// ═══════════════════════════════════════════════════════════════════════════
//   WEB UI HTML — Futuristic Dark Theme
// ═══════════════════════════════════════════════════════════════════════════
// Served from C string to avoid SPIFFS dependency for the UI itself.
// Uses template placeholders: __SLOT_A_NAME__, __SLOT_B_NAME__,
// __IP__, __WIFI_MODE__ (see the page.replace() calls in buildPage())

static const char HTML_PAGE[] PROGMEM = R"HTMLEOF(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,minimum-scale=0.5">
<title>ESP32 MultiBoot Manager</title>
<style>
:root{
  --bg:#0a0a0a;--bg2:#131313;--bg3:#1b1b1b;
  --line:#2c2c2c;--line2:#3a3a3a;
  --txt:#ededed;--dim:#8a8a8a;--dim2:#525252;
  --w:#ffffff;--k:#000000;
}
*{box-sizing:border-box;margin:0;padding:0}
html,body{background:var(--bg);color:var(--txt);font-family:-apple-system,'Segoe UI',system-ui,sans-serif;min-height:100vh}
body{padding:20px 12px 40px}
h1{font-size:1.3rem;font-weight:700;letter-spacing:2px;text-transform:uppercase;color:var(--w);margin-bottom:4px}
.sub{color:var(--dim);font-size:.78rem;letter-spacing:.5px;margin-bottom:20px}
.wrap{max-width:720px;margin:0 auto}
/* Status bar */
.sbar{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:18px;
      background:var(--bg2);border:1px solid var(--line);border-radius:8px;padding:10px 14px}
.sitem{font-size:.75rem;color:var(--dim);letter-spacing:.3px}
.sitem span{color:var(--txt);font-weight:600;margin-left:4px}
.sitem .on{color:var(--w)}.sitem .off{color:var(--dim)}.sitem .warn{color:var(--txt)}
/* Cards */
.card{background:var(--bg2);border:1px solid var(--line);border-radius:10px;
      padding:18px;margin-bottom:16px}
.card-title{font-size:.7rem;font-weight:700;letter-spacing:2px;text-transform:uppercase;
            color:var(--dim);margin-bottom:14px;display:flex;align-items:center;gap:8px}
/* Slot cards */
.slots{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:16px}
@media(max-width:540px){.slots{grid-template-columns:1fr}}
.slot{background:var(--bg3);border:1px solid var(--line);border-radius:8px;padding:14px;
      transition:border-color .15s ease-out}
.slot.valid{border-color:var(--line2)}
.slot.empty{border-color:var(--line);opacity:.65}
.slot-id{font-size:.6rem;letter-spacing:2px;text-transform:uppercase;color:var(--dim);margin-bottom:6px}
.slot-name{font-size:.9rem;font-weight:600;color:var(--txt);font-family:ui-monospace,monospace;
           word-break:break-all;margin-bottom:8px;min-height:20px}
.slot-tag{display:inline-block;font-size:.6rem;font-weight:700;letter-spacing:1px;
          padding:2px 8px;border-radius:20px;text-transform:uppercase;border:1px solid var(--line2)}
.tag-valid{background:var(--w);color:var(--k);border-color:var(--w)}
.tag-empty{background:transparent;color:var(--dim)}
.slot-btns{display:flex;gap:6px;margin-top:10px;flex-wrap:wrap}
/* Buttons — flat grayscale, no glow/gradient, cheap to render every frame */
.btn{border:1px solid var(--line2);border-radius:6px;padding:7px 13px;font-size:.75rem;font-weight:600;
     cursor:pointer;letter-spacing:.3px;transition:background-color .12s ease-out;text-transform:uppercase;
     background:var(--bg3);color:var(--txt)}
.btn:hover{background:var(--line)}
.btn:active{background:var(--line2)}
.btn:disabled{opacity:.35;cursor:not-allowed}
.btn-primary{background:var(--w);color:var(--k);border-color:var(--w)}
.btn-primary:hover{background:#d8d8d8}
.btn-danger{background:transparent;border:1px solid var(--dim2);color:var(--dim)}
.btn-danger:hover{border-color:var(--w);color:var(--w)}
.btn-lg{width:100%;padding:11px;font-size:.85rem}
/* Upload zone */
.dropzone{border:1px dashed var(--line2);border-radius:8px;padding:28px 20px;
          text-align:center;cursor:pointer;transition:border-color .15s ease-out;margin-bottom:12px}
.dropzone:hover,.dropzone.drag{border-color:var(--w);border-style:solid}
.dropzone input{display:none}
.dz-icon{font-size:1.6rem;margin-bottom:8px;color:var(--dim)}
.dz-label{color:var(--w);font-weight:600;cursor:pointer;font-size:.85rem;text-decoration:underline}
.dz-sub{color:var(--dim);font-size:.75rem;margin-top:6px}
.dz-fname{margin-top:8px;font-family:ui-monospace,monospace;font-size:.8rem;
          color:var(--txt);display:none}
/* Progress — flat fill, no gradient/animation cost */
.prog-wrap{display:none;margin-top:10px}
.prog-track{height:6px;background:var(--bg3);border-radius:3px;overflow:hidden;
            border:1px solid var(--line)}
.prog-fill{height:100%;width:0%;background:var(--w);border-radius:3px;transition:width .15s linear}
.prog-txt{font-size:.72rem;color:var(--dim);margin-top:5px;text-align:center}
/* Boot/erase modal */
.modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.92);z-index:999;
       flex-direction:column;align-items:center;justify-content:center;gap:16px}
.modal.show{display:flex}
.spinner{width:36px;height:36px;border:3px solid var(--line2);border-top-color:var(--w);
         border-radius:50%;animation:spin .7s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
.modal-title{font-size:1rem;font-weight:700;color:var(--txt);letter-spacing:.5px}
.modal-sub{font-size:.8rem;color:var(--dim);text-align:center;max-width:320px}
/* Misc */
.tip{font-size:.72rem;color:var(--dim);line-height:1.6;margin-top:10px;
     padding:8px 12px;background:var(--bg3);border-radius:6px;border-left:2px solid var(--line2)}
/* Top bar + settings */
.topbar{display:flex;justify-content:space-between;align-items:flex-start;gap:12px}
.gear-btn{background:transparent;border:1px solid var(--line2);color:var(--txt);
          width:34px;height:34px;flex-shrink:0;border-radius:8px;font-size:1rem;
          cursor:pointer;transition:background-color .12s ease-out;line-height:1}
.gear-btn:hover{background:var(--line)}
.field-label{font-size:.68rem;color:var(--dim);display:block;margin-bottom:4px;
             letter-spacing:.3px;text-transform:uppercase}
.field{width:100%;background:var(--bg3);border:1px solid var(--line2);border-radius:6px;
       color:var(--txt);padding:9px 11px;font-size:.82rem;margin-bottom:10px;
       font-family:inherit}
.field:focus{outline:none;border-color:var(--w)}
.settings-sheet{max-width:480px;width:100%;background:var(--bg2);border:1px solid var(--line);
                 border-radius:10px;padding:20px;position:relative;margin:auto}
.close-x{position:absolute;top:14px;right:14px;background:none;border:none;color:var(--dim);
         font-size:1.1rem;cursor:pointer;line-height:1;padding:4px}
.close-x:hover{color:var(--w)}
</style>
</head>
<body>
<div class="wrap">

<div class="topbar">
  <div>
    <h1>MultiBoot Manager</h1>
    <div class="sub">Fully Offline · ESP32 Dual-Slot Persistent Boot System</div>
  </div>
  <button class="gear-btn" onclick="openSettings()" aria-label="Settings" title="Settings">&#9881;</button>
</div>

<!-- Status Bar -->
<div class="sbar">
  <div class="sitem">WiFi<span id="wifiMode" class="__WIFI_CLASS__">__WIFI_MODE__</span></div>
  <div class="sitem">IP<span id="espIp">__IP__</span></div>
  <div class="sitem">Uptime<span id="uptime">--</span></div>
</div>

<!-- Slot Cards -->
<div class="slots">
  <div class="slot __SLOT_A_CLASS__" id="slotACard">
    <div class="slot-id">◈ SLOT A — Persistent</div>
    <div class="slot-name" id="slotAName">__SLOT_A_NAME__</div>
    <span class="slot-tag __SLOT_A_TAG_CLASS__" id="slotATag">__SLOT_A_TAG__</span>
    <div class="slot-btns" id="slotABtns">__SLOT_A_BTNS__</div>
  </div>
  <div class="slot __SLOT_B_CLASS__" id="slotBCard">
    <div class="slot-id">◈ SLOT B — Persistent</div>
    <div class="slot-name" id="slotBName">__SLOT_B_NAME__</div>
    <span class="slot-tag __SLOT_B_TAG_CLASS__" id="slotBTag">__SLOT_B_TAG__</span>
    <div class="slot-btns" id="slotBBtns">__SLOT_B_BTNS__</div>
  </div>
</div>

<!-- Upload -->
<div class="card">
  <div class="card-title">⬆ Upload &amp; Flash Binary</div>
  <div class="dropzone" id="dropZone">
    <div class="dz-icon">+</div>
    <div>Drop .bin file here or <label class="dz-label" for="fileInput">browse</label></div>
    <input type="file" id="fileInput" accept=".bin">
    <div class="dz-sub">Only valid ESP32 firmware .bin files accepted</div>
    <div class="dz-fname" id="dzFname"></div>
  </div>
  <div class="prog-wrap" id="progWrap">
    <div class="prog-track"><div class="prog-fill" id="progFill"></div></div>
    <div class="prog-txt" id="progTxt">Uploading...</div>
  </div>
  <div style="display:flex;gap:.6rem;margin-top:12px">
    <button class="btn btn-primary btn-lg" id="btnUploadFlashA" onclick="uploadFile('a')" disabled style="flex:1">
      → Flash Slot A
    </button>
    <button class="btn btn-primary btn-lg" id="btnUploadFlashB" onclick="uploadFile('b')" disabled style="flex:1">
      → Flash Slot B
    </button>
  </div>
  <div class="tip">
    ℹ Picks a target slot, streams the file straight over WiFi into that slot's flash — no SD card, no separate step — and reboots into it automatically when done. Works from any connected device, phone included.
  </div>
</div>

<!-- Return to manager tip -->
<div class="card">
  <div class="card-title">↩ Return to Manager</div>
  <div style="font-size:.8rem;color:var(--dim);line-height:1.8">
    While another binary is running, return here by:<br>
    <strong style="color:var(--txt)">①</strong> Hold <code style="color:var(--txt)">GPIO 0</code> LOW and press Reset button<br>
    <strong style="color:var(--txt)">②</strong> From your app: include <code style="color:var(--txt)">ReturnToManager.h</code> and call <code style="color:var(--txt)">returnToManager()</code> — it sets the boot partition back to factory and restarts
  </div>
</div>

</div><!-- .wrap -->

<!-- Boot / erase modal -->
<div class="modal" id="flashModal">
  <div class="spinner"></div>
  <div class="modal-title" id="modalTitle">Working...</div>
  <div class="modal-sub" id="modalSub">Do not power off. ESP32 will reboot automatically.</div>
</div>

<!-- Settings panel -->
<div class="modal" id="settingsModal" style="align-items:flex-start;overflow-y:auto;padding:30px 12px">
  <div class="settings-sheet">
    <button class="close-x" onclick="closeSettings()" aria-label="Close">&#10005;</button>
    <div class="card-title" style="margin-bottom:16px">Settings</div>

    <div class="card">
      <div class="card-title">Access Point</div>
      <label class="field-label" for="apSsidInput">SSID</label>
      <input type="text" id="apSsidInput" class="field" maxlength="31">
      <label class="field-label" for="apPassInput">Password (min 8 characters)</label>
      <input type="password" id="apPassInput" class="field" maxlength="63" placeholder="Leave to keep current">
      <button class="btn btn-primary btn-lg" onclick="saveAP()">Save Access Point</button>
      <div class="tip" id="apMsg" style="display:none"></div>
    </div>

    <div class="card">
      <div class="card-title">Change OTA Password</div>
      <label class="field-label" for="otaOldPass">Current OTA Password</label>
      <input type="password" id="otaOldPass" class="field">
      <label class="field-label" for="otaNewPass">New OTA Password</label>
      <input type="password" id="otaNewPass" class="field">
      <label class="field-label" for="otaConfirmPass">Confirm New Password</label>
      <input type="password" id="otaConfirmPass" class="field">
      <button class="btn btn-primary btn-lg" onclick="saveOtaPass()">Change Password</button>
      <div class="tip" id="otaPassMsg" style="display:none"></div>
    </div>

    <div class="card" style="margin-bottom:0">
      <div class="card-title">Update Manager Firmware</div>
      <div class="tip">Uses Slot B as temporary staging — its current content is overwritten during the update. The manager restarts automatically once done.</div>
      <label class="field-label" style="margin-top:10px" for="mgrFileInput">Manager .bin File</label>
      <input type="file" id="mgrFileInput" class="field" accept=".bin">
      <label class="field-label" for="mgrOtaPass">OTA Update Password</label>
      <input type="password" id="mgrOtaPass" class="field">
      <div class="prog-wrap" id="mgrProgWrap">
        <div class="prog-track"><div class="prog-fill" id="mgrProgFill"></div></div>
        <div class="prog-txt" id="mgrProgTxt"></div>
      </div>
      <button class="btn btn-primary btn-lg" id="mgrUpdateBtn" onclick="updateManager()">Upload &amp; Update Manager</button>
    </div>
  </div>
</div>

<script>
// ── Uptime counter ────────────────────────────────────────────────────────
function refreshUptime(){
  fetch('/status').then(function(r){ return r.json(); }).then(function(d){
    var s = d.uptime_s || 0;
    var h = Math.floor(s/3600), m = Math.floor((s%3600)/60), sec = s%60;
    document.getElementById('uptime').textContent =
      (h?h+'h ':'') + (m?m+'m ':'') + sec + 's';
  }).catch(function(){ /* ignore transient poll errors */ });
}
refreshUptime();
setInterval(refreshUptime, 5000);

// ── Drop zone ─────────────────────────────────────────────────────────────
var dz = document.getElementById('dropZone');
var fi = document.getElementById('fileInput');
var dzFname = document.getElementById('dzFname');
var btnUploadFlashA = document.getElementById('btnUploadFlashA');
var btnUploadFlashB = document.getElementById('btnUploadFlashB');

function setFile(file){
  if(!file) return;
  if(!file.name.toLowerCase().endsWith('.bin')){
    alert('Only .bin files are accepted.');
    return;
  }
  fi._file = file;
  dzFname.textContent = file.name + '  (' + (file.size/1024).toFixed(1) + ' KB)';
  dzFname.style.display = 'block';
  btnUploadFlashA.disabled = false;
  btnUploadFlashB.disabled = false;
}
fi.addEventListener('change', function(){ setFile(fi.files[0]); });
dz.addEventListener('click', function(){ fi.click(); });
dz.addEventListener('dragover', function(e){ e.preventDefault(); dz.classList.add('drag'); });
dz.addEventListener('dragleave', function(){ dz.classList.remove('drag'); });
dz.addEventListener('drop', function(e){
  e.preventDefault(); dz.classList.remove('drag');
  setFile(e.dataTransfer.files[0]);
});

function resetUploadButtons(){
  btnUploadFlashA.disabled = false;
  btnUploadFlashB.disabled = false;
}

// ── Upload → streams straight into the chosen slot's flash partition ──────
// No SD card, no intermediate save step. The XHR upload progress IS the
// flash progress (the server writes each chunk via esp_ota_write as it
// arrives), so no separate polling/modal is needed — the browser's own
// upload progress bar already reflects real write progress. The short gap
// between "100% sent" and the server's final response is esp_ota_end()
// verifying the image; that's shown as "Verifying..." below.
function uploadFile(slot){
  var file = fi._file;
  if(!file){ alert('Select a .bin file first.'); return; }
  var pw = document.getElementById('progWrap');
  var pf = document.getElementById('progFill');
  var pt = document.getElementById('progTxt');
  pw.style.display = 'block';
  btnUploadFlashA.disabled = true;
  btnUploadFlashB.disabled = true;
  var fd = new FormData();
  fd.append('file', file, file.name);
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/upload?slot=' + slot);
  // No explicit Authorization header — the browser already cached
  // Basic-Auth credentials from loading "/" and auto-attaches them here.
  xhr.upload.onprogress = function(e){
    if(!e.lengthComputable) return;
    var pct = Math.round(e.loaded/e.total*100);
    pf.style.width = pct + '%';
    if(pct < 100){
      pt.textContent = pct + '% — ' + (e.loaded/1024).toFixed(1)+' / '+(e.total/1024).toFixed(1)+' KB';
    } else {
      pt.textContent = 'Verifying and rebooting into Slot ' + slot.toUpperCase() + '...';
    }
  };
  xhr.onload = function(){
    if(xhr.status === 200){
      pt.textContent = 'Done. Rebooting into Slot ' + slot.toUpperCase() + '...';
    } else if(xhr.status === 401){
      pt.textContent = 'Auth failed.';
      resetUploadButtons();
    } else if(xhr.status === 409){
      pt.textContent = xhr.responseText;
      resetUploadButtons();
    } else {
      pt.textContent = 'Upload failed: ' + xhr.responseText;
      resetUploadButtons();
    }
  };
  xhr.onerror = function(){ pt.textContent = 'Network error.'; resetUploadButtons(); };
  xhr.send(fd);
}

// ── Boot from slot (already flashed) ─────────────────────────────────────
function bootSlot(slot){
  if(!confirm('Boot from Slot ' + slot.toUpperCase() + '?')) return;
  var modal = document.getElementById('flashModal');
  var title = document.getElementById('modalTitle');
  var sub   = document.getElementById('modalSub');
  modal.classList.add('show');
  title.textContent = 'Booting Slot ' + slot.toUpperCase() + '...';
  sub.textContent   = 'Setting boot target and restarting.';
  fetch('/boot-slot?slot=' + slot, {method:'POST'})
    .catch(function(){ /* Expected — device rebooted */ });
}

// ── Erase slot ────────────────────────────────────────────────────────────
// /erase-slot runs on a background task and returns immediately (202), so
// poll /status until the slot actually reports empty (or give up after ~10s).
function eraseSlot(slot){
  if(!confirm('Erase Slot ' + slot.toUpperCase() + '? The binary in this slot will be permanently removed.')) return;
  fetch('/erase-slot?slot=' + slot, {method:'POST'})
    .then(function(r){ return r.text(); })
    .then(function(){
      var tries = 0;
      var poll = setInterval(function(){
        tries++;
        fetch('/status').then(function(r){ return r.json(); }).then(function(d){
          var st = (slot === 'a') ? d.slot_a.status : d.slot_b.status;
          if (st === 'empty' || tries > 20) { clearInterval(poll); location.reload(); }
        }).catch(function(){ if (tries > 20) { clearInterval(poll); location.reload(); } });
      }, 500);
    })
    .catch(function(e){ alert('Error: ' + e); });
}

// ── Settings panel ──────────────────────────────────────────────────────────
function openSettings(){
  document.getElementById('settingsModal').classList.add('show');
  fetch('/settings').then(function(r){ return r.json(); }).then(function(d){
    document.getElementById('apSsidInput').value = d.ap_ssid || '';
  }).catch(function(){ /* ignore — user can still type a new SSID */ });
}
function closeSettings(){
  document.getElementById('settingsModal').classList.remove('show');
}

function showMsg(id, text, ok){
  var el = document.getElementById(id);
  el.style.display = 'block';
  el.textContent = text;
  el.style.borderLeftColor = ok ? 'var(--w)' : 'var(--dim2)';
}

function saveAP(){
  var ssid = document.getElementById('apSsidInput').value;
  var pass = document.getElementById('apPassInput').value;
  if(!ssid){ showMsg('apMsg', 'SSID is required.', false); return; }
  if(!pass || pass.length < 8){ showMsg('apMsg', 'Password must be at least 8 characters.', false); return; }
  var fd = new URLSearchParams();
  fd.append('ssid', ssid);
  fd.append('pass', pass);
  fetch('/settings/ap', {method:'POST', body: fd})
    .then(function(r){ return r.text().then(function(t){ return {ok:r.ok, text:t}; }); })
    .then(function(res){ showMsg('apMsg', res.text, res.ok); })
    .catch(function(){ showMsg('apMsg', 'Network error.', false); });
}

function saveOtaPass(){
  var oldP = document.getElementById('otaOldPass').value;
  var newP = document.getElementById('otaNewPass').value;
  var confP = document.getElementById('otaConfirmPass').value;
  if(!oldP || !newP || !confP){ showMsg('otaPassMsg', 'All three fields are required.', false); return; }
  if(newP.length < 8){ showMsg('otaPassMsg', 'New password must be at least 8 characters.', false); return; }
  if(newP !== confP){ showMsg('otaPassMsg', 'New password and confirmation do not match.', false); return; }
  var fd = new URLSearchParams();
  fd.append('oldPass', oldP);
  fd.append('newPass', newP);
  fd.append('confirmPass', confP);
  fetch('/settings/ota-password', {method:'POST', body: fd})
    .then(function(r){ return r.text().then(function(t){ return {ok:r.ok, text:t}; }); })
    .then(function(res){
      showMsg('otaPassMsg', res.text, res.ok);
      if(res.ok){
        document.getElementById('otaOldPass').value = '';
        document.getElementById('otaNewPass').value = '';
        document.getElementById('otaConfirmPass').value = '';
      }
    })
    .catch(function(){ showMsg('otaPassMsg', 'Network error.', false); });
}

function updateManager(){
  var fi2 = document.getElementById('mgrFileInput');
  var file = fi2.files[0];
  var otapass = document.getElementById('mgrOtaPass').value;
  if(!file){ alert('Select the new manager .bin file first.'); return; }
  if(!otapass){ alert('Enter the OTA update password.'); return; }
  if(!confirm('Update the manager firmware? Slot B will be temporarily overwritten during this process.')) return;

  var pw = document.getElementById('mgrProgWrap');
  var pf = document.getElementById('mgrProgFill');
  var pt = document.getElementById('mgrProgTxt');
  var btn = document.getElementById('mgrUpdateBtn');
  pw.style.display = 'block';
  btn.disabled = true;

  var fd = new FormData();
  fd.append('file', file, file.name);
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/manager-update?otapass=' + encodeURIComponent(otapass));
  xhr.upload.onprogress = function(e){
    if(!e.lengthComputable) return;
    var pct = Math.round(e.loaded/e.total*100);
    pf.style.width = pct + '%';
    pt.textContent = pct < 100 ? (pct + '%') : 'Verifying and rebooting...';
  };
  xhr.onload = function(){
    if(xhr.status === 200){
      pt.textContent = 'Update staged. Rebooting to complete...';
      setTimeout(function(){ closeSettings(); }, 2000);
    } else {
      pt.textContent = 'Failed: ' + xhr.responseText;
      btn.disabled = false;
    }
  };
  xhr.onerror = function(){ pt.textContent = 'Network error.'; btn.disabled = false; };
  xhr.send(fd);
}
</script>
</body>
</html>
)HTMLEOF";

// ═══════════════════════════════════════════════════════════════════════════
//   HTML BUILDER
// ═══════════════════════════════════════════════════════════════════════════
String buildSlotBtns(const char* slot, SlotStatus status) {
  String s = "";
  if (status == SLOT_EMPTY) {
    // Can only flash from SD
    s += "<span style='font-size:.72rem;color:var(--dim)'>Upload a .bin below to use</span>";
  } else {
    // Flashed — can boot into it or erase it. (No "currently running" state
    // is possible here — see the comment on getSlotStatus() above.)
    s += "<button class='btn btn-primary' onclick=\"bootSlot('" + String(slot) + "')\">Boot</button>";
    s += "<button class='btn btn-danger' onclick=\"eraseSlot('" + String(slot) + "')\">Erase</button>";
  }
  return s;
}

String buildPage() {
  SlotStatus stA = getSlotStatus("slot_a");
  SlotStatus stB = getSlotStatus("slot_b");

  // ── Slot A strings (only EMPTY/VALID are reachable states) ─────────────
  String aName    = (stA == SLOT_EMPTY) ? "— Empty —" : (slotAName.length() ? slotAName : "Unknown");
  String aClass   = (stA == SLOT_VALID) ? "valid" : "empty";
  String aTagCls  = (stA == SLOT_VALID) ? "tag-valid" : "tag-empty";
  String aTag     = (stA == SLOT_VALID) ? "READY" : "EMPTY";
  String aBtns    = buildSlotBtns("a", stA);

  // ── Slot B strings ───────────────────────────────────────────────────────
  String bName    = (stB == SLOT_EMPTY) ? "— Empty —" : (slotBName.length() ? slotBName : "Unknown");
  String bClass   = (stB == SLOT_VALID) ? "valid" : "empty";
  String bTagCls  = (stB == SLOT_VALID) ? "tag-valid" : "tag-empty";
  String bTag     = (stB == SLOT_VALID) ? "READY" : "EMPTY";
  String bBtns    = buildSlotBtns("b", stB);

  // ── Assemble ─────────────────────────────────────────────────────────────
  String page = FPSTR(HTML_PAGE);
  String ip   = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  page.replace("__WIFI_CLASS__",      apMode ? "warn" : "on");
  page.replace("__WIFI_MODE__",       apMode ? " AP Mode" : " Connected");
  page.replace("__IP__",              ip);
  page.replace("__SLOT_A_CLASS__",    aClass);
  page.replace("__SLOT_A_NAME__",     aName);
  page.replace("__SLOT_A_TAG_CLASS__",aTagCls);
  page.replace("__SLOT_A_TAG__",      aTag);
  page.replace("__SLOT_A_BTNS__",     aBtns);
  page.replace("__SLOT_B_CLASS__",    bClass);
  page.replace("__SLOT_B_NAME__",     bName);
  page.replace("__SLOT_B_TAG_CLASS__",bTagCls);
  page.replace("__SLOT_B_TAG__",      bTag);
  page.replace("__SLOT_B_BTNS__",     bBtns);
  return page;
}

// ═══════════════════════════════════════════════════════════════════════════
//   ROUTE HANDLERS
// ═══════════════════════════════════════════════════════════════════════════

// ── GET / ────────────────────────────────────────────────────────────────
void handleRoot() {
  if (!checkAuth()) return;
  String page = buildPage();
  server.send(200, "text/html", page);
}

// esp_ota_abort() does NOT erase flash already written by prior
// esp_ota_write() calls. If an upload fails partway through, the first
// sector — which holds the magic byte getSlotStatus() treats as ground
// truth — was already overwritten by the new (now-truncated) image. That
// would leave a corrupt slot able to report "READY" in the UI, get
// boot-slotted by the user, and crash-loop with no manager-side recovery.
// Erasing just the header sector on any post-esp_ota_begin() failure
// guarantees the slot unambiguously reports SLOT_EMPTY afterward instead
// of a false "valid" — cheap (one 4KB sector) and doesn't need the
// full-partition erase eraseTask() uses for a deliberate user-requested wipe.
void invalidateSlotHeader(const esp_partition_t* part) {
  if (!part) return;
  esp_err_t e = esp_partition_erase_range(part, 0, 4096);
  if (e != ESP_OK) {
    Serial.printf("[UPLOAD] WARNING: header invalidate failed: 0x%X — slot state may be inconsistent\n", e);
  }
}

// ── POST /upload?slot=a|b  →  streams straight into the OTA partition ─────
// V1.4: no SD card in this design. Every byte the browser/phone sends is
// handed to esp_ota_write() as it arrives — the same esp_ota_begin/write/end
// sequence this manager always used, just fed from the live HTTP body
// instead of a file. This runs synchronously inside the WebServer's own
// request handling (the same pattern ArduinoOTA/esp_https_ota use for
// streaming updates) — the HTTP request simply stays open for the duration
// of the upload, exactly like any normal OTA update tool. server.handleClient()
// resuming service to OTHER routes has to wait until this one finishes,
// which is expected/intentional for a firmware write in progress.
//
// Stability note — avoids a crash that previously caused a "network error"
// esp_ota_begin() called with a KNOWN size erases ALL of that size's sectors
// in one single blocking call, upfront, before a single byte is written.
// For a ~1MB slot that single call can run long enough to starve the idle
// task on that core past the default watchdog timeout — the device panics
// and reboots mid-request, which the browser reports as "network error"
// right after it finished sending (it has no way to know the server side
// crashed). Passing OTA_SIZE_UNKNOWN instead makes esp_ota_write() erase
// lazily, one 4KB sector at a time, only as newly-written data actually
// crosses into it — spread naturally across every ~1.4KB HTTP chunk instead
// of one multi-second block, which keeps the idle task fed throughout.
static esp_ota_handle_t g_otaHandle       = 0;
static const esp_partition_t* g_otaPart   = nullptr;
static char   g_otaSlot                    = 0;      // 'a' or 'b'
static bool   g_uploadAuthed               = false;
static bool   g_otaBegun                   = false;
static bool   g_uploadOk                   = false;
static bool   g_firstChunkChecked          = false;
static size_t g_uploadWritten              = 0;
static String g_uploadFilename;
static String g_uploadFailReason;

void handleUploadBody() {
  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    g_uploadAuthed      = false;
    g_otaBegun           = false;
    g_uploadOk            = false;
    g_firstChunkChecked   = false;
    g_uploadWritten       = 0;
    g_uploadFailReason    = "";
    g_otaHandle           = 0;
    g_otaPart             = nullptr;

    if (!checkOrigin()) return;
    if (!checkAuth()) return;      // Only check here — once per request
    g_uploadAuthed = true;

    if (flashInProgress || eraseInProgress) {
      g_uploadFailReason = "A flash/erase operation is already in progress";
      Serial.println("[UPLOAD] Rejected: flash/erase in progress");
      return;
    }

    String slot = server.arg("slot");
    slot.toLowerCase();
    if (slot != "a" && slot != "b") {
      g_uploadFailReason = "Missing/invalid slot — must upload with ?slot=a or ?slot=b";
      Serial.println("[UPLOAD] Rejected: bad slot param");
      return;
    }

    String fname = sanitizeFilename(up.filename);
    if (fname.length() == 0 || !fname.endsWith(".bin")) {
      g_uploadFailReason = "Not a valid .bin filename";
      Serial.println("[UPLOAD] Rejected: not a valid .bin filename");
      return;
    }

    const char* partName = (slot == "a") ? "slot_a" : "slot_b";
    g_otaPart = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, partName);
    if (!g_otaPart) {
      g_uploadFailReason = "Partition '" + String(partName) + "' not found in partition table";
      Serial.println("[UPLOAD] ERROR: partition not found: " + String(partName));
      return;
    }

    if (!lockIO(5000)) {
      g_uploadFailReason = "NVS busy — could not start upload";
      Serial.println("[UPLOAD] Rejected: NVS busy");
      return;
    }
    unlockIO();  // Only needed briefly for the prefs write at UPLOAD_FILE_END

    esp_err_t err = esp_ota_begin(g_otaPart, OTA_SIZE_UNKNOWN, &g_otaHandle);
    if (err != ESP_OK) {
      g_uploadFailReason = "esp_ota_begin failed: 0x" + String(err, HEX);
      Serial.println("[UPLOAD] ERROR: esp_ota_begin failed");
      return;
    }
    g_otaBegun     = true;
    g_otaSlot      = slot.charAt(0);
    g_uploadFilename = fname;
    g_uploadOk       = true;
    flashInProgress  = true;

    Serial.println("[UPLOAD] Start → slot_" + slot + "  file=" + fname);
  }

  else if (up.status == UPLOAD_FILE_WRITE) {
    if (!g_uploadAuthed || !g_uploadOk || !g_otaBegun) return;

    if (!g_firstChunkChecked) {
      g_firstChunkChecked = true;
      if (up.currentSize == 0 || up.buf[0] != OTA_MAGIC_BYTE) {
        g_uploadFailReason = "Invalid firmware header (not a valid ESP32 image)";
        Serial.println("[UPLOAD] ERROR: bad magic byte on first chunk");
        esp_ota_abort(g_otaHandle);
        invalidateSlotHeader(g_otaPart);
        g_uploadOk = false;
        return;
      }
    }

    esp_err_t err = esp_ota_write(g_otaHandle, up.buf, up.currentSize);
    if (err != ESP_OK) {
      g_uploadFailReason = "esp_ota_write failed at offset " + String((unsigned)g_uploadWritten) +
                            ": 0x" + String(err, HEX);
      Serial.println("[UPLOAD] ERROR: esp_ota_write failed");
      esp_ota_abort(g_otaHandle);
      invalidateSlotHeader(g_otaPart);
      g_uploadOk = false;
      return;
    }
    g_uploadWritten += up.currentSize;
    delay(1);  // Yield after every flash write so WiFi/AP beacon timing isn't starved
    if (g_uploadWritten % 65536 < up.currentSize) {  // Log roughly every 64KB
      Serial.printf("[UPLOAD] %u KB written\n", (unsigned)(g_uploadWritten / 1024));
    }
  }

  else if (up.status == UPLOAD_FILE_END) {
    if (!g_uploadAuthed || !g_otaBegun) return;
    if (!g_uploadOk) return;  // Already failed+aborted during WRITE above

    if (g_uploadWritten < MIN_FW_SIZE) {
      g_uploadFailReason = "File too small (" + String((unsigned)g_uploadWritten) + " bytes) — not valid firmware";
      Serial.println("[UPLOAD] ERROR: file too small");
      esp_ota_abort(g_otaHandle);
      invalidateSlotHeader(g_otaPart);
      g_uploadOk = false;
      return;
    }

    esp_err_t err = esp_ota_end(g_otaHandle);
    if (err != ESP_OK) {
      g_uploadFailReason = "esp_ota_end (verify) failed: 0x" + String(err, HEX) + " — binary may be corrupt";
      Serial.println("[UPLOAD] ERROR: esp_ota_end failed");
      invalidateSlotHeader(g_otaPart);
      g_uploadOk = false;
      return;
    }

    String slot(g_otaSlot);
    if (lockIO()) {
      prefs.begin(PREF_NS, false);
      if (slot == "a") { prefs.putString(PREF_SLOT_A_NAME, g_uploadFilename); slotAName = g_uploadFilename; }
      else             { prefs.putString(PREF_SLOT_B_NAME, g_uploadFilename); slotBName = g_uploadFilename; }
      prefs.putString(PREF_BOOT_TARGET, slot == "a" ? TARGET_SLOT_A : TARGET_SLOT_B);
      prefs.putUChar(PREF_BOOT_FAILS, 0);   // Fresh unconfirmed-boot budget
      prefs.end();
      unlockIO();
    }

    Serial.printf("[UPLOAD] Done! %u bytes → slot_%c\n", (unsigned)g_uploadWritten, g_otaSlot);
  }

  else if (up.status == UPLOAD_FILE_ABORTED) {
    if (g_otaBegun) {
      esp_ota_abort(g_otaHandle);
      if (g_uploadWritten > 0) invalidateSlotHeader(g_otaPart);
    }
    g_uploadOk = false;
    g_uploadFailReason = "Upload aborted (connection lost)";
  }
}

void handleUploadDone() {
  flashInProgress = false;

  if (!g_uploadAuthed) return;  // checkAuth()/checkOrigin() already sent the error response

  if (!g_uploadOk) {
    server.send(400, "text/plain", g_uploadFailReason.length() ? g_uploadFailReason : "Upload failed");
    return;
  }

  server.send(200, "text/plain", "Flashed into slot " + String(g_otaSlot) + " — rebooting...");
  Serial.println("[UPLOAD] Rebooting into slot_" + String(g_otaSlot) + " in 1s");
  delay(600);  // Let the HTTP response actually reach the client before restart
  ESP.restart();
}

// ═══════════════════════════════════════════════════════════════════════════
//   SETTINGS — AP credentials, OTA password, manager self-update
// ═══════════════════════════════════════════════════════════════════════════

// ── GET /settings ──────────────────────────────────────────────────────────
// Returns current, non-secret settings for the panel to pre-fill. Passwords
// are never sent back — only whether one is currently set (always true here,
// both have first-boot defaults).
void handleGetSettings() {
  if (!checkAuth()) return;
  String json = "{\"ap_ssid\":\"" + apSsid + "\"}";
  server.send(200, "application/json", json);
}

// ── POST /settings/ap  (ssid, pass) ────────────────────────────────────────
// Changes the AP's own SSID/password and applies it immediately via
// WiFi.softAP() — no reboot needed, existing connections just get dropped
// and can rejoin under the new name/password right away.
void handleSetAP() {
  if (!checkOrigin()) return;
  if (!checkAuth()) return;

  String newSsid = server.arg("ssid");
  String newPass = server.arg("pass");
  newSsid.trim();

  if (newSsid.length() < 1 || newSsid.length() > 31) {
    server.send(400, "text/plain", "SSID must be 1-31 characters");
    return;
  }
  if (newPass.length() < 8 || newPass.length() > 63) {
    server.send(400, "text/plain", "Password must be 8-63 characters (WPA2 requirement)");
    return;
  }

  if (!lockIO(3000)) {
    server.send(503, "text/plain", "NVS busy — try again");
    return;
  }
  prefs.begin(PREF_NS, false);
  prefs.putString(PREF_AP_SSID, newSsid);
  prefs.putString(PREF_AP_PASS, newPass);
  prefs.end();
  unlockIO();

  apSsid = newSsid;
  apPass = newPass;

  bool ok = WiFi.softAP(apSsid.c_str(), apPass.c_str());
  WiFi.setSleep(false);
  if (!ok) {
    server.send(500, "text/plain", "Saved, but applying the new AP settings failed — power-cycle the device to apply.");
    return;
  }

  Serial.println("[AP] SSID/password changed → " + apSsid);
  server.send(200, "text/plain", "Access Point updated. Reconnect using the new SSID/password.");
}

// ── POST /settings/ota-password  (oldPass, newPass, confirmPass) ──────────
void handleChangeOtaPass() {
  if (!checkOrigin()) return;
  if (!checkAuth()) return;

  String oldPass     = server.arg("oldPass");
  String newPass     = server.arg("newPass");
  String confirmPass = server.arg("confirmPass");

  if (!secureEquals(oldPass, otaUpdatePass)) {
    server.send(401, "text/plain", "Current OTA password is incorrect");
    return;
  }
  if (newPass.length() < 8 || newPass.length() > 63) {
    server.send(400, "text/plain", "New password must be at least 8 characters");
    return;
  }
  if (!secureEquals(newPass, confirmPass)) {
    server.send(400, "text/plain", "New password and confirmation do not match");
    return;
  }

  if (!lockIO(3000)) {
    server.send(503, "text/plain", "NVS busy — try again");
    return;
  }
  prefs.begin(PREF_NS, false);
  prefs.putString(PREF_OTA_PASS, newPass);
  prefs.end();
  unlockIO();

  otaUpdatePass = newPass;
  Serial.println("[OTA] Update password changed");
  server.send(200, "text/plain", "OTA update password changed");
}

// ── POST /manager-update?otapass=xxx  →  Hop 1 of the self-update ─────────
// Streams a new manager .bin into slot_b (always — never directly into the
// running "manager" partition, see checkPendingPromotion() for why) and
// marks it for promotion on next boot. Requires BOTH the normal dashboard
// login (checkAuth) AND the separate OTA password, since this is the one
// action that can affect the manager itself.
static bool   g_mgrOriginAuthOk  = false;  // true once checkOrigin()+checkAuth() both pass (they self-respond on failure — must not send again)
static bool   g_mgrUpdateAuthed  = false;  // true once the OTA password ALSO checks out
static bool   g_mgrUpdateOk      = false;
static bool   g_mgrFirstChecked  = false;
static size_t g_mgrWritten       = 0;
static String g_mgrFailReason;

void handleManagerUpdateBody() {
  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    g_mgrOriginAuthOk = false;
    g_mgrUpdateAuthed = false;
    g_mgrUpdateOk      = false;
    g_mgrFirstChecked  = false;
    g_mgrWritten       = 0;
    g_mgrFailReason    = "";
    g_otaHandle        = 0;
    g_otaPart          = nullptr;

    if (!checkOrigin()) return;   // Already sent its own error response
    if (!checkAuth())   return;   // Already sent its own error response
    g_mgrOriginAuthOk = true;     // From here on, nothing has responded yet —
                                   // handleManagerUpdateDone() owns the response.

    if (!secureEquals(server.arg("otapass"), otaUpdatePass)) {
      g_mgrFailReason = "Incorrect OTA update password";
      Serial.println("[MGR-UPDATE] Rejected: bad OTA password");
      return;
    }
    g_mgrUpdateAuthed = true;

    if (flashInProgress || eraseInProgress) {
      g_mgrFailReason = "A flash/erase operation is already in progress";
      return;
    }

    String fname = sanitizeFilename(up.filename);
    if (fname.length() == 0 || !fname.endsWith(".bin")) {
      g_mgrFailReason = "Not a valid .bin filename";
      return;
    }

    g_otaPart = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "slot_b");
    if (!g_otaPart) {
      g_mgrFailReason = "Staging partition 'slot_b' not found";
      Serial.println("[MGR-UPDATE] ERROR: slot_b partition not found");
      return;
    }

    esp_err_t err = esp_ota_begin(g_otaPart, OTA_SIZE_UNKNOWN, &g_otaHandle);
    if (err != ESP_OK) {
      g_mgrFailReason = "esp_ota_begin failed: 0x" + String(err, HEX);
      return;
    }
    g_mgrUpdateOk   = true;
    flashInProgress = true;
    Serial.println("[MGR-UPDATE] Staging new manager firmware into slot_b: " + fname);
  }

  else if (up.status == UPLOAD_FILE_WRITE) {
    if (!g_mgrUpdateAuthed || !g_mgrUpdateOk) return;

    if (!g_mgrFirstChecked) {
      g_mgrFirstChecked = true;
      if (up.currentSize == 0 || up.buf[0] != OTA_MAGIC_BYTE) {
        g_mgrFailReason = "Invalid firmware header (not a valid ESP32 image)";
        esp_ota_abort(g_otaHandle);
        invalidateSlotHeader(g_otaPart);
        g_mgrUpdateOk = false;
        return;
      }
    }

    esp_err_t err = esp_ota_write(g_otaHandle, up.buf, up.currentSize);
    if (err != ESP_OK) {
      g_mgrFailReason = "esp_ota_write failed: 0x" + String(err, HEX);
      esp_ota_abort(g_otaHandle);
      invalidateSlotHeader(g_otaPart);
      g_mgrUpdateOk = false;
      return;
    }
    g_mgrWritten += up.currentSize;
    delay(1);  // Yield after every flash write so WiFi/AP beacon timing isn't starved
    if (g_mgrWritten % 65536 < up.currentSize) {
      Serial.printf("[MGR-UPDATE] %u KB written\n", (unsigned)(g_mgrWritten / 1024));
    }
  }

  else if (up.status == UPLOAD_FILE_END) {
    if (!g_mgrUpdateAuthed || !g_mgrUpdateOk) return;

    if (g_mgrWritten < MIN_FW_SIZE) {
      g_mgrFailReason = "File too small (" + String((unsigned)g_mgrWritten) + " bytes) — not valid firmware";
      esp_ota_abort(g_otaHandle);
      invalidateSlotHeader(g_otaPart);
      g_mgrUpdateOk = false;
      return;
    }

    esp_err_t err = esp_ota_end(g_otaHandle);
    if (err != ESP_OK) {
      g_mgrFailReason = "esp_ota_end (verify) failed: 0x" + String(err, HEX);
      invalidateSlotHeader(g_otaPart);
      g_mgrUpdateOk = false;
      return;
    }

    // Mark for Hop 2 (checkPendingPromotion(), runs at next boot) and point
    // the boot target at slot_b so we actually land there on restart.
    if (lockIO()) {
      prefs.begin(PREF_NS, false);
      prefs.putUChar(PREF_PEND_PROMOTE, 1);
      prefs.putUInt(PREF_PEND_LEN, (uint32_t)g_mgrWritten);
      prefs.putString(PREF_BOOT_TARGET, TARGET_SLOT_B);
      prefs.putUChar(PREF_BOOT_FAILS, 0);
      prefs.end();
      unlockIO();
    }
    Serial.printf("[MGR-UPDATE] Staged %u bytes — will promote to manager on next boot\n", (unsigned)g_mgrWritten);
  }

  else if (up.status == UPLOAD_FILE_ABORTED) {
    if (g_mgrUpdateOk) {
      esp_ota_abort(g_otaHandle);
      if (g_mgrWritten > 0) invalidateSlotHeader(g_otaPart);
    }
    g_mgrUpdateOk = false;
    g_mgrFailReason = "Upload aborted (connection lost)";
  }
}

void handleManagerUpdateDone() {
  flashInProgress = false;

  if (!g_mgrOriginAuthOk) return;  // checkOrigin()/checkAuth() already sent the response

  if (!g_mgrUpdateOk) {
    server.send(g_mgrUpdateAuthed ? 400 : 401, "text/plain",
                g_mgrFailReason.length() ? g_mgrFailReason : "Update failed");
    return;
  }

  server.send(200, "text/plain", "Manager firmware staged — rebooting to complete the update...");
  Serial.println("[MGR-UPDATE] Rebooting to complete self-update");
  delay(600);
  ESP.restart();
}

// ── POST /boot-slot?slot=a|b ──────────────────────────────────────────────
// Boot from an already-flashed slot (instant 2-3sec boot)
void handleBootSlot() {
  if (!checkOrigin()) return;
  if (!checkAuth()) return;
  if (flashInProgress || eraseInProgress) {
    server.send(409, "text/plain", "A flash/erase operation is in progress — try again shortly");
    return;
  }
  if (!server.hasArg("slot")) { server.send(400, "text/plain", "Missing slot"); return; }
  String slot = server.arg("slot");
  slot.toLowerCase();
  if (slot != "a" && slot != "b") { server.send(400, "text/plain", "slot must be a or b"); return; }

  SlotStatus st = (slot == "a") ? getSlotStatus("slot_a") : getSlotStatus("slot_b");
  if (st == SLOT_EMPTY) {
    server.send(409, "text/plain", "Slot " + slot + " is empty — flash a binary first");
    return;
  }

  if (!lockIO()) {
    server.send(503, "text/plain", "NVS busy — try again");
    return;
  }
  prefs.begin(PREF_NS, false);
  prefs.putString(PREF_BOOT_TARGET, slot == "a" ? TARGET_SLOT_A : TARGET_SLOT_B);
  // Fresh unconfirmed-boot budget every time a boot is deliberately
  // requested from the dashboard, so rtm_bootSafetyCheck() in the slot app
  // doesn't inherit a stale fail count from a previous, unrelated session.
  prefs.putUChar(PREF_BOOT_FAILS, 0);
  prefs.end();
  unlockIO();

  server.send(200, "text/plain", "Booting slot " + slot + "...");

  Serial.println("[BOOT] Target: slot_" + slot + " → restarting");
  delay(400);
  ESP.restart();
}

// ── Background erase task ──────────────────────────────────────────────────
// esp_partition_erase_range() on a ~1.25MB slot can take multiple seconds.
// Running it inline inside the HTTP handler blocked server.handleClient()
// for the whole duration — the same class of bug the flash handler avoids,
// just missed for erase. Moved to its own FreeRTOS task under the same
// ioMutex, mirroring flashTask()'s structure.
struct EraseTaskCtx {
  char slot;   // 'a' or 'b'
};

void eraseTask(void* param) {
  EraseTaskCtx* ctx = (EraseTaskCtx*)param;
  String slot = String(ctx->slot);
  const char* partName = (slot == "a") ? "slot_a" : "slot_b";

  if (!lockIO(5000)) {
    Serial.println("[ERASE] SD/NVS busy — could not start erase");
    eraseInProgress = false;
    delete ctx;
    vTaskDelete(NULL);
    return;
  }

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, partName);
  if (!part) {
    Serial.println("[ERASE] ERROR: Partition not found: " + String(partName));
  } else {
    // No "is this the running partition?" guard needed here — this
    // task only ever runs while the manager (factory partition) is running,
    // so `part` (slot_a/slot_b) can never be the running partition in the
    // first place. See getSlotStatus() comment for the full reasoning.
    Serial.printf("[ERASE] Erasing %s (0x%X, %u bytes)\n",
                  partName, (unsigned)part->address, (unsigned)part->size);
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
      Serial.printf("[ERASE] ERROR: erase failed: 0x%X\n", err);
    } else {
      prefs.begin(PREF_NS, false);
      if (slot == "a") { prefs.remove(PREF_SLOT_A_NAME); slotAName = ""; }
      else             { prefs.remove(PREF_SLOT_B_NAME); slotBName = ""; }
      prefs.end();
      Serial.println("[ERASE] Done.");
    }
  }

  unlockIO();
  eraseInProgress = false;
  delete ctx;
  vTaskDelete(NULL);
}

// ── POST /erase-slot?slot=a|b ─────────────────────────────────────────────
// Starts a background task and returns immediately (202) instead
// of blocking the WebServer for the whole erase duration.
void handleEraseSlot() {
  if (!checkOrigin()) return;
  if (!checkAuth()) return;
  if (flashInProgress || eraseInProgress) {
    server.send(409, "text/plain", "A flash/erase operation is already in progress");
    return;
  }
  if (!server.hasArg("slot")) { server.send(400, "text/plain", "Missing slot"); return; }
  String slot = server.arg("slot");
  slot.toLowerCase();
  if (slot != "a" && slot != "b") { server.send(400, "text/plain", "Invalid slot"); return; }

  EraseTaskCtx* ctx = new EraseTaskCtx();
  ctx->slot = slot.charAt(0);

  eraseInProgress = true;
  BaseType_t ok = xTaskCreate(eraseTask, "eraseTask", 4096, ctx, 1, NULL);
  if (ok != pdPASS) {
    eraseInProgress = false;
    delete ctx;
    server.send(500, "text/plain", "Could not start erase task (low memory)");
    return;
  }

  server.send(202, "text/plain", "Erase started");
}


// ── GET /status ───────────────────────────────────────────────────────────
void handleStatus() {
  // Public endpoint — no auth (useful for other apps to check manager status)
  SlotStatus stA = getSlotStatus("slot_a");
  SlotStatus stB = getSlotStatus("slot_b");
  auto stStr = [](SlotStatus s) -> const char* {
    return s == SLOT_VALID ? "valid" : "empty";
  };
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String json = "{\"ip\":\"" + ip + "\""
              + ",\"uptime_s\":" + String(millis() / 1000)  // Real device uptime
              + ",\"ap_mode\":" + (apMode ? "true" : "false")
              + ",\"slot_a\":{\"status\":\"" + stStr(stA) + "\",\"name\":\"" + slotAName + "\"}"
              + ",\"slot_b\":{\"status\":\"" + stStr(stB) + "\",\"name\":\"" + slotBName + "\"}"
              + "}";
  server.send(200, "application/json", json);
}

// There used to be a GET /reboot-manager handler here. It's removed
// — not just disabled — because it could never be legitimately reached: it
// only exists on the manager's own WebServer, which is only running while
// the manager itself is already active. A running slot_a/slot_b app is
// separate firmware with no access to this server or its routes. Worse, it
// only wrote an NVS Preferences flag that the bootloader never reads (see
// the V1.2 changelog at the top of this file for the full root-cause
// explanation), so even if it had been reachable it would not have reliably
// returned control to the manager.
//
// The actual mechanism for a slot app to hand control back to this manager
// lives in ReturnToManager.h (shipped alongside this file) — it must be
// included by EVERY app you flash into slot_a/slot_b. That header calls
// esp_ota_set_boot_partition() on the factory partition directly, which is
// the only thing that actually changes what the 2nd-stage bootloader does
// on the next restart.

// ── 404 handler ───────────────────────────────────────────────────────────
// Captive-portal behavior (FEATURE: auto-open on AP connect): phones/laptops
// probe well-known URLs (e.g. Android's /generate_204, iOS's
// /hotspot-detect.html, Windows's /ncsi.txt) right after joining a WiFi AP.
// None of those match a route we've registered, so they all land here via
// server.onNotFound(). In AP mode, redirecting ANY unmatched request to "/"
// makes those probes fail their expected "internet reachable" check, which
// is exactly what makes the OS pop its "Sign in to network" captive-portal
// browser automatically — landing the user straight on the dashboard.
// In STA mode (connected to the home WiFi) this stays a plain 404; there's
// no captive-portal flow to trigger on a normal network.
void handleNotFound() {
  if (apMode) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "Not found: " + server.uri());
}

// ═══════════════════════════════════════════════════════════════════════════
//   BOOT CHAIN LOGIC
//   Runs at startup: checks NVS target and either stays in manager mode
//   or sets the OTA boot partition and restarts into the target slot.
// ═══════════════════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════════════════
//   MANAGER SELF-UPDATE — safe two-hop promotion
// ═══════════════════════════════════════════════════════════════════════════
// V1.5: the manager can update ITS OWN firmware from the dashboard, but it
// cannot simply esp_ota_write() into the "manager" (factory) partition while
// running from that same partition — ESP-IDF's flash-cache mapping means
// erasing/rewriting flash the CPU is currently executing instructions from
// is undefined behavior (can crash/hang mid-erase). So a self-update is done
// in two safe hops, neither of which ever touches the currently-running
// partition:
//
//   Hop 1 (handleManagerUpdateBody(), below): while running from "manager"
//     (factory) as always, stream the new manager .bin into "slot_b" —
//     perfectly safe, exactly like a normal slot upload. Mark two NVS flags
//     (PREF_PEND_PROMOTE + PREF_PEND_LEN) and reboot into slot_b.
//
//   Hop 2 (checkPendingPromotion(), here): the NEW manager code is now
//     running FROM slot_b. Since "manager" (factory) is no longer the
//     running partition, it's now 100% safe to esp_ota_write() the exact
//     same bytes into it. On success, esp_ota_set_boot_partition() points
//     the hardware boot target back at "manager" and restarts — landing
//     on the updated firmware, running from its permanent home.
//
// If anything in Hop 2 fails, the flag is cleared and boot proceeds
// normally — the device keeps working perfectly fine as "the manager"
// (just physically executing from slot_b instead of factory) so the user
// can safely retry the update from Settings; nothing is ever left in a
// half-flashed or unbootable state.
void checkPendingPromotion() {
  prefs.begin(PREF_NS, true);
  bool     pending = prefs.getUChar(PREF_PEND_PROMOTE, 0) == 1;
  uint32_t len     = prefs.getUInt(PREF_PEND_LEN, 0);
  prefs.end();
  if (!pending) return;

  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || strcmp(running->label, "slot_b") != 0 || len == 0) {
    // Stale/unexpected flag (e.g. user erased slot_b manually before this
    // boot completed) — clear it and continue as a completely normal boot.
    prefs.begin(PREF_NS, false);
    prefs.putUChar(PREF_PEND_PROMOTE, 0);
    prefs.end();
    return;
  }

  Serial.println("[PROMOTE] Pending manager self-update — copying slot_b → manager");
  const esp_partition_t* factoryPart = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);

  bool ok = false;
  if (!factoryPart) {
    Serial.println("[PROMOTE] ERROR: factory partition not found");
  } else if (len > factoryPart->size) {
    Serial.println("[PROMOTE] ERROR: staged image larger than factory partition");
  } else {
    esp_ota_handle_t h = 0;
    // Same fix as the upload handler — OTA_SIZE_UNKNOWN so
    // the erase happens lazily, one sector at a time, interleaved with the
    // write loop below (which already yields every 1KB) — never one long
    // blocking erase call that could starve the idle task during boot.
    esp_err_t err = esp_ota_begin(factoryPart, OTA_SIZE_UNKNOWN, &h);
    if (err != ESP_OK) {
      Serial.printf("[PROMOTE] ERROR: esp_ota_begin failed: 0x%X\n", err);
    } else {
      uint8_t buf[1024];
      size_t  off = 0;
      bool    ioErr = false;
      while (off < len) {
        size_t chunk = (len - off > sizeof(buf)) ? sizeof(buf) : (len - off);
        if (esp_partition_read(running, off, buf, chunk) != ESP_OK) { ioErr = true; break; }
        if (esp_ota_write(h, buf, chunk) != ESP_OK)                { ioErr = true; break; }
        off += chunk;
        delay(1);   // Yield every 1KB — keeps this well clear of any watchdog
      }
      if (ioErr) {
        Serial.println("[PROMOTE] ERROR: read/write failed mid-copy");
        esp_ota_abort(h);
      } else if (esp_ota_end(h) != ESP_OK) {
        Serial.println("[PROMOTE] ERROR: esp_ota_end (image verify) failed");
      } else if (esp_ota_set_boot_partition(factoryPart) != ESP_OK) {
        Serial.println("[PROMOTE] ERROR: could not set boot partition to manager");
      } else {
        ok = true;
      }
    }
  }

  prefs.begin(PREF_NS, false);
  prefs.putUChar(PREF_PEND_PROMOTE, 0);
  prefs.putString(PREF_BOOT_TARGET, TARGET_MANAGER);
  if (ok) {
    // slot_b's flash still physically contains a full copy of the manager
    // image at this point — harmless, but label it clearly instead of
    // leaving a blank-but-"valid" slot that could confuse the dashboard.
    prefs.putString(PREF_SLOT_B_NAME, "(manager backup — safe to overwrite)");
  }
  prefs.end();

  if (ok) {
    Serial.println("[PROMOTE] Success — rebooting into updated manager");
    delay(300);
    ESP.restart();
    // Never returns.
  }
  Serial.println("[PROMOTE] Failed — continuing to run as manager from slot_b. Retry the update from Settings.");
  // Falls through to normal boot. PREF_BOOT_TARGET is now "manager", so the
  // handleBootChain() call right after this will correctly no-op instead of
  // trying to "chain" into slot_b again.
}

void handleBootChain() {
  // GPIO 0 LOW at boot = force manager mode regardless of NVS setting
  // GPIO 0 is typically connected to a button on dev boards (BOOT button)
  pinMode(BOOT_PIN, INPUT_PULLUP);
  delay(10);
  bool forceManager = (digitalRead(BOOT_PIN) == LOW);

  prefs.begin(PREF_NS, true);
  String target = prefs.getString(PREF_BOOT_TARGET, TARGET_MANAGER);
  prefs.end();

  Serial.println("[BOOT] Target from NVS: " + target);
  if (forceManager) Serial.println("[BOOT] GPIO 0 LOW → forcing manager mode");

  if (forceManager || target == TARGET_MANAGER || target == "") {
    // Stay in manager — no partition switch needed
    Serial.println("[BOOT] Manager mode active");

    // Ensure next boot also goes to manager (unless user chooses a slot)
    prefs.begin(PREF_NS, false);
    prefs.putString(PREF_BOOT_TARGET, TARGET_MANAGER);
    prefs.end();
    return;
  }

  // Boot into a slot
  const char* partName = (target == TARGET_SLOT_A) ? "slot_a" :
                         (target == TARGET_SLOT_B) ? "slot_b" : nullptr;
  if (!partName) {
    Serial.println("[BOOT] Unknown target '" + target + "' → staying in manager");
    return;
  }

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, partName);
  if (!part) {
    Serial.println("[BOOT] Partition not found: " + String(partName));
    return;
  }

  // Verify magic byte before booting
  uint8_t magic = 0;
  esp_partition_read(part, 0, &magic, 1);
  if (magic != OTA_MAGIC_BYTE) {
    Serial.printf("[BOOT] Slot %s has no valid firmware (magic=0x%02X) → staying in manager\n",
                  partName, magic);
    // Reset target to manager since slot is empty/corrupt
    prefs.begin(PREF_NS, false);
    prefs.putString(PREF_BOOT_TARGET, TARGET_MANAGER);
    prefs.end();
    return;
  }

  Serial.printf("[BOOT] Setting boot partition → %s, restarting...\n", partName);
  esp_err_t err = esp_ota_set_boot_partition(part);
  if (err != ESP_OK) {
    Serial.printf("[BOOT] esp_ota_set_boot_partition failed: 0x%X → staying in manager\n", err);
    return;
  }

  delay(200);
  esp_restart();
  // ↑ Never returns. If it does, we fall through to manager mode.
}

// ═══════════════════════════════════════════════════════════════════════════
//   SETUP
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n===== ESP32 MultiBoot Manager =====");

  // Create the shared Preferences mutex before anything touches it,
  // so it's guaranteed to exist once the WebServer starts handling requests.
  ioMutex = xSemaphoreCreateMutex();

  // ── Hop 2 of a manager self-update, if one is pending (see the big
  // comment above checkPendingPromotion()). Must run BEFORE handleBootChain()
  // so a promotion-in-progress is handled before any normal boot-target
  // logic runs.
  checkPendingPromotion();

  // ── Boot chain: may restart into a slot ──────────────────────────────────
  handleBootChain();
  // If we reach here, we are in manager mode.

  // ── Load NVS slot names + settings (AP identity, OTA password) ──────────
  prefs.begin(PREF_NS, true);
  slotAName     = prefs.getString(PREF_SLOT_A_NAME, "");
  slotBName     = prefs.getString(PREF_SLOT_B_NAME, "");
  apSsid        = prefs.getString(PREF_AP_SSID, AP_SSID_DEFAULT);
  apPass        = prefs.getString(PREF_AP_PASS, AP_PASS_DEFAULT);
  otaUpdatePass = prefs.getString(PREF_OTA_PASS, OTA_PASS_DEFAULT);
  prefs.end();
  Serial.println("[NVS] Slot A: " + (slotAName.length() ? slotAName : "(empty)"));
  Serial.println("[NVS] Slot B: " + (slotBName.length() ? slotBName : "(empty)"));

  // ── WiFi — V1.5: fully offline. This device never joins another network;
  // it always runs its own Access Point so it works anywhere with zero
  // router/internet dependency. ──────────────────────────────────────────
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  // Disable WiFi modem sleep — with it enabled, any time the CPU is busy
  // with a flash write for more than a few milliseconds (as OTA writes
  // are), the radio can miss beacon/ack timing and the connected phone
  // may drop the WiFi association mid-transfer. This keeps the radio
  // fully awake and responsive throughout an upload.
  WiFi.setSleep(false);
  Serial.println("[AP]   SSID: " + apSsid);
  Serial.println("[AP]   IP:   " + WiFi.softAPIP().toString());

  // Captive portal: answer every DNS query on the AP with our own IP, so any
  // hostname a connecting phone tries to resolve (used by its captive-portal
  // probe) points straight back at us and the dashboard opens automatically.
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("[DNS] Captive portal DNS started — connect to the AP and the dashboard should open automatically");

  // ── Register routes ───────────────────────────────────────────────────────
  server.on("/",                    HTTP_GET,  handleRoot);
  server.on("/status",               HTTP_GET,  handleStatus);
  server.on("/boot-slot",            HTTP_POST, handleBootSlot);
  server.on("/erase-slot",           HTTP_POST, handleEraseSlot);
  server.on("/upload",               HTTP_POST, handleUploadDone, handleUploadBody);
  server.on("/settings",             HTTP_GET,  handleGetSettings);
  server.on("/settings/ap",          HTTP_POST, handleSetAP);
  server.on("/settings/ota-password",HTTP_POST, handleChangeOtaPass);
  server.on("/manager-update",       HTTP_POST, handleManagerUpdateDone, handleManagerUpdateBody);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("[Server] Running on port 80");
  Serial.println("[Server] Open in browser: http://" + WiFi.softAPIP().toString());
  Serial.println("===========================================\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//   LOOP
// ═══════════════════════════════════════════════════════════════════════════
void loop() {
  if (apMode) dnsServer.processNextRequest();  // Captive portal DNS
  server.handleClient();
  delay(2);
}
