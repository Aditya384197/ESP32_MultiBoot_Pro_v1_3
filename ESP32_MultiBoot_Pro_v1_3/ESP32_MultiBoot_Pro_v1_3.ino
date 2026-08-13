/*
 * ╔══════════════════════════════════════════════════════════════════════════╗
 * ║   ESP32 MULTIBOOT MANAGER — PRO                                        ║
 * ║   Dual-Slot Persistent Boot Manager — Direct WiFi Upload (No SD Card)  ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  CONCEPT                                                                ║
 * ║  This firmware lives in the FACTORY partition and is NEVER erased by   ║
 * ║  OTA operations. It manages two persistent OTA slots (A and B) which   ║
 * ║  can each hold a different application binary (~1.2MB each). A .bin    ║
 * ║  file is uploaded straight from a connected phone/PC over WiFi (AP or  ║
 * ║  home network) and streamed directly into the target slot's flash     ║
 * ║  partition as it arrives — no SD card, no intermediate storage. Once   ║
 * ║  a slot is flashed, booting from it takes only 2-3 seconds. The       ║
 * ║  manager itself always boots first; it checks a boot-flag in NVS and   ║
 * ║  either stays as manager or chains to the selected slot.               ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  V1.4 CHANGE: SD card module removed entirely. Earlier versions of     ║
 * ║  this file stored uploaded binaries on an SD card and flashed them     ║
 * ║  into a slot as a second step; that whole subsystem (SD.h/SPI.h,       ║
 * ║  BINARIES_DIR listing, /flash, /delete) is gone. Upload now writes     ║
 * ║  directly into the target esp_ota partition via esp_ota_begin/write/   ║
 * ║  end as each HTTP chunk arrives — the same trusted, well-tested API    ║
 * ║  this manager already used for the SD→slot write, just fed from the   ║
 * ║  live upload stream instead of a file. Historical FIX-N comments below ║
 * ║  describing the old SD-based flow are kept as-is for project history;  ║
 * ║  they describe behavior that no longer exists in this version.         ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  HARDWARE                                                               ║
 * ║  • ESP32 (4MB Flash minimum)                                           ║
 * ║  • (Optional) Button on GPIO 0 → GND = force manager mode at boot     ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  ARDUINO IDE / ARDUINODROID SETUP                                      ║
 * ║  Board          : ESP32 Dev Module                                     ║
 * ║  Partition Scheme: Custom (use partitions_multiboot.csv)               ║
 * ║  Flash Size     : 4MB                                                  ║
 * ║  Upload Speed   : 921600                                               ║
 * ║                                                                        ║
 * ║  CUSTOM PARTITION TABLE (partitions_multiboot.csv):                    ║
 * ║  nvs,     data, nvs,     0x9000,   0x5000                             ║
 * ║  otadata, data, ota,     0xe000,   0x2000                             ║
 * ║  manager, app,  factory, 0x10000,  0x100000 ← THIS firmware (1024KB) ║
 * ║  slot_a,  app,  ota_0,   0x110000, 0x130000  ← Persistent slot A     ║
 * ║  slot_b,  app,  ota_1,   0x240000, 0x130000  ← Persistent slot B     ║
 * ║  spiffs,  data, spiffs,  0x370000, 0x28000   ← Config/log (160KB)   ║
 * ║  (Grown from the original 960KB factory slot — the compiled manager  ║
 * ║  binary is ~1010KB, so 960KB overflowed by ~49KB. All partitions     ║
 * ║  after 'manager' shifted by +64KB to compensate; still fits in 4MB.) ║
 * ║                                                                        ║
 * ║  LIBRARIES (all built-in with ESP32 Arduino Core):                     ║
 * ║  WiFi, WebServer, SD, SPI, Update, Preferences, SPIFFS                ║
 * ║  esp_ota_ops.h, esp_partition.h (ESP-IDF, included via core)          ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  BOOT FLOW                                                              ║
 * ║  Power ON → Manager boots (factory partition, always first)            ║
 * ║     ├─ GPIO 0 held LOW? → Force manager mode (ignore boot target)     ║
 * ║     ├─ NVS boot_target == "manager"? → Stay in manager mode           ║
 * ║     ├─ NVS boot_target == "slot_a"? → esp_ota_set_boot_partition(A)  ║
 * ║     │      → ESP.restart() → Slot A boots (2-3 sec)                  ║
 * ║     └─ NVS boot_target == "slot_b"? → Same for slot B                ║
 * ║                                                                        ║
 * ║  Returning to Manager from a running app:                              ║
 * ║     • Physical: Hold GPIO 0 LOW and press Reset button                ║
 * ║     • Software: app includes ReturnToManager.h and calls              ║
 * ║       returnToManager() — this calls esp_ota_set_boot_partition() on  ║
 * ║       the factory partition directly (NOT an NVS flag — the           ║
 * ║       bootloader only reads otadata, see V1.2 changelog below) then   ║
 * ║       esp_restart(). ReturnToManager.h must be included by every app  ║
 * ║       you flash into slot_a/slot_b for this to work.                  ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  SECURITY                                                               ║
 * ║  Web UI protected by Basic Authentication (username + password).       ║
 * ║  Default: admin / esp32boot  — change in CONFIG section below.        ║
 * ║  All write operations (flash, upload, delete, erase) require auth.    ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  V1.0 — Production Grade                                               ║
 * ║  ✔ All original bugs fixed (see bug list below)                       ║
 * ║  ✔ Custom partition table with factory + dual OTA slots               ║
 * ║  ✔ Proper esp_ota_ops based slot flashing (not Update.h mismatch)    ║
 * ║  ✔ Magic byte (0xE9) + size validation before any flash               ║
 * ║  ✔ POST routes for all write operations (not GET)                     ║
 * ║  ✔ File handle cleanup (root.close(), file.close() everywhere)        ║
 * ║  ✔ Update.write() return value checked — abort on partial write       ║
 * ║  ✔ Chunked response for flash progress (no race with restart)         ║
 * ║  ✔ SD card fail detection with proper error reporting                 ║
 * ║  ✔ Upload: non-static file handles (no concurrent corruption)         ║
 * ║  ✔ Basic Auth on all write routes                                     ║
 * ║  ✔ GPIO 0 boot-pin for force-manager mode                             ║
 * ║  ✔ WiFi AP fallback if STA connect fails                              ║
 * ║  ✔ Slot status (empty/flashed/active) from esp_ota_ops               ║
 * ║  ✔ ESP32 Arduino Core 2.x + 3.x compatible                           ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  V1.1 — Audit Fixes (deep review pass)                                 ║
 * ║  ✔ FIX-1: Upload JS sent a hardcoded blank-password Authorization     ║
 * ║           header on every /upload XHR, permanently overriding the     ║
 * ║           browser's correctly cached Basic-Auth credentials →         ║
 * ║           upload always failed with 401. Removed the bad header;      ║
 * ║           browser now auto-attaches its cached creds.                 ║
 * ║  ✔ FIX-2: handleFlash() ran the entire SD→flash write + ESP.restart() ║
 * ║           synchronously inside the HTTP handler. WebServer is single- ║
 * ║           threaded, so /flash-progress polls could never be served    ║
 * ║           until AFTER the device had already restarted → progress bar ║
 * ║           never animated. Flashing now runs in a dedicated FreeRTOS   ║
 * ║           task; the HTTP handler returns immediately so loop() stays  ║
 * ║           free to serve /flash-progress polls in real time.           ║
 * ║  ✔ FIX-3: /flash accepted the SD filename with NO path-traversal      ║
 * ║           sanitization (unlike /delete, which had it). An authed      ║
 * ║           request with file=../something could read outside          ║
 * ║           /binaries. Same sanitization as /delete now applied.        ║
 * ║  ✔ FIX-4: No mutual exclusion between the new async flash task and    ║
 * ║           other handlers touching SD/NVS (/delete, /upload,           ║
 * ║           /erase-slot, page listing) → possible corruption if two     ║
 * ║           operations hit the SD card at the same time. Added a        ║
 * ║           shared FreeRTOS mutex (ioMutex) around every SD/Preferences ║
 * ║           access, plus a flashInProgress guard rejecting overlapping  ║
 * ║           /flash requests with 409.                                   ║
 * ║  ✔ FIX-5: handleUploadBody() called checkAuth() on every multipart    ║
 * ║           chunk. On repeated auth failure this could call             ║
 * ║           server.send()/requestAuthentication() more than once for   ║
 * ║           the same request mid-multipart-parse → undefined response   ║
 * ║           state. Auth is now checked once at UPLOAD_FILE_START only.  ║
 * ║  ✔ FIX-6: Uploaded filenames were only stripped of path separators,   ║
 * ║           not of quote/angle-bracket characters — a crafted filename  ║
 * ║           could break out of the onclick="..." JS string when listed  ║
 * ║           on the dashboard. Filenames are now whitelisted to          ║
 * ║           [A-Za-z0-9._-] before being written to SD.                  ║
 * ║  ✔ FIX-7: No CSRF protection on state-changing POST routes. Because   ║
 * ║           browsers auto-attach cached Basic-Auth credentials to       ║
 * ║           same-origin-looking simple POST requests, a page on any     ║
 * ║           other site could silently trigger /flash, /erase-slot,     ║
 * ║           /boot-slot, /delete against a logged-in admin's browser.    ║
 * ║           Added checkOrigin() — validates Origin/Referer against the  ║
 * ║           device's own current IP — on every write route.             ║
 * ║  ✔ FIX-8: No brute-force protection on Basic Auth — unlimited guesses ║
 * ║           at the password. Added a small per-IP failed-attempt        ║
 * ║           tracker with temporary lockout (mirrors the pattern used    ║
 * ║           in the weather-station V9-Secure hardening pass).           ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  V1.2 — Holistic Audit Fixes (root-cause pass, not patches)            ║
 * ║  ✔ FIX-9 (CSRF): checkOrigin() did indexOf() substring containment on  ║
 * ║           the Referer fallback — an attacker page whose own URL path   ║
 * ║           happened to contain the device's LAN IP (e.g.               ║
 * ║           evil.com/192.168.1.50/x.html) passed the check. Replaced     ║
 * ║           with exact host[:port] extraction + exact-match comparison. ║
 * ║           Also: previously, if BOTH Origin and Referer were absent,    ║
 * ║           the request was ALLOWED (fail-open). Now fails CLOSED —      ║
 * ║           no Origin/Referer on a state-changing route = rejected.      ║
 * ║  ✔ FIX-10 (dead code): SLOT_ACTIVE could never be observed by this     ║
 * ║           firmware — the manager only ever runs from the factory       ║
 * ║           partition, so esp_ota_get_running_partition() can never      ║
 * ║           equal slot_a/slot_b from inside manager code. Removed the    ║
 * ║           whole active-slot branch (enum, UI class, button logic).     ║
 * ║           Slots are now simply EMPTY or FLASHED — that matches what    ║
 * ║           the manager can actually know about itself.                 ║
 * ║  ✔ FIX-11 (boot logic — root cause): handleRebootToManager() and the   ║
 * ║           documented "app sets mbmgr/boot_tgt=manager + restart"       ║
 * ║           pattern only wrote a Preferences flag in a namespace the     ║
 * ║           BOOTLOADER never reads. The 2nd-stage ESP-IDF bootloader     ║
 * ║           decides which partition to boot from otadata — set purely   ║
 * ║           by esp_ota_set_boot_partition(). Once a slot has been        ║
 * ║           booted into, otadata still points at that slot forever       ║
 * ║           until something calls esp_ota_set_boot_partition() back to  ║
 * ║           the factory partition — writing an NVS flag the running     ║
 * ║           slot app doesn't even use does nothing to otadata. Net       ║
 * ║           effect: "return to manager" could silently just reboot back ║
 * ║           into the same slot app forever. Removed the unreachable      ║
 * ║           /reboot-manager route from the MANAGER (it can only ever be ║
 * ║           hit while the manager is already active — a slot app is     ║
 * ║           different firmware and cannot call a route on a server that ║
 * ║           isn't running). The actual "return" must happen from        ║
 * ║           WITHIN the slot app via the corrected ReturnToManager.h      ║
 * ║           (shipped separately), which now calls                       ║
 * ║           esp_ota_set_boot_partition() on the factory partition        ║
 * ║           itself before restarting — that's the only thing that       ║
 * ║           actually changes what the bootloader does next.             ║
 * ║  ✔ FIX-12 (data race): fp.error[80] was written by flashTask() and     ║
 * ║           read by handleFlashProgress() across two FreeRTOS tasks      ║
 * ║           with no synchronization (only pct/done/ok were volatile).    ║
 * ║           Added a dedicated fpMutex guarding every read/write of the   ║
 * ║           whole FlashProgress struct as one atomic unit.               ║
 * ║  ✔ FIX-13 (route hardening): /flash-progress was fully unauthenticated ║
 * ║           and could leak SD filenames via error strings to anyone on   ║
 * ║           the LAN. Now requires the same auth as every other route —   ║
 * ║           free for legitimate clients since the browser already        ║
 * ║           auto-attaches its cached Basic-Auth credentials to           ║
 * ║           same-origin polls.                                           ║
 * ║  ✔ FIX-14: AP_PASS defaulted to the exact same string as AUTH_PASS —   ║
 * ║           compromising one secret compromised both surfaces. Given     ║
 * ║           distinct defaults; both still must be changed before         ║
 * ║           deployment (unchanged architectural limit, see below).       ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  V1.3 — Deep Bug Hunt (system-breaker / invisible-bug pass)            ║
 * ║  ✔ FIX-15 (atomic integrity): esp_ota_abort() does NOT erase already-  ║
 * ║           written flash. A write failure mid-stream left the first     ║
 * ║           sector (magic byte) intact while the rest was truncated —    ║
 * ║           getSlotStatus() only checks the magic byte, so a corrupt     ║
 * ║           slot could still report "READY", get boot-slotted by the    ║
 * ║           user, and crash-loop with no manager-side recovery. Any      ║
 * ║           failure after esp_ota_begin() now erases the header sector   ║
 * ║           (invalidateSlotHeader()) and clears the slot's NVS name, so  ║
 * ║           a failed flash unambiguously reports SLOT_EMPTY afterward.   ║
 * ║  ✔ FIX-16 (watchdog/clean-up): /erase-slot erased up to 1.25MB         ║
 * ║           synchronously inside the HTTP handler — the same class of    ║
 * ║           server-blocking bug FIX-2 fixed for flashing, just missed    ║
 * ║           for erase. Moved to a background eraseTask() guarded by the  ║
 * ║           same ioMutex, with its own eraseInProgress flag combined     ║
 * ║           into every existing flashInProgress guard.                   ║
 * ║  ✔ FIX-17 (auto-recovery): GPIO0-hold-to-return only ever worked from  ║
 * ║           inside the MANAGER's own boot chain — which never runs once  ║
 * ║           otadata points at a slot. A slot app that crash-loops before ║
 * ║           calling returnToManager() had zero recovery path. Added      ║
 * ║           rtm_bootSafetyCheck()/rtm_markBootOK() to ReturnToManager.h: ║
 * ║           checks GPIO0 AND a consecutive-unconfirmed-boot counter,     ║
 * ║           auto-reverting to the manager after 3 unconfirmed restarts.  ║
 * ║           Honest limit: if a slot binary crashes before this call even ║
 * ║           executes, no software fix can help — serial reflash only.    ║
 * ║  ✔ FIX-18 (consistency): removed dead .slot.active/.tag-active CSS     ║
 * ║           left over from the FIX-10 SLOT_ACTIVE removal. Replaced the  ║
 * ║           client-side "Uptime" fake clock (Date.now() since page load, ║
 * ║           not device boot — reset on every refresh) with a real        ║
 * ║           millis()-based uptime_s field polled from /status.           ║
 * ║  ✔ FIX-19 (consistency/security): findLoginSlot() used to evict idle   ║
 * ║           (unlocked) tracking slots before locked ones — an attacker   ║
 * ║           rotating >8 source IPs could keep landing in fresh slots and ║
 * ║           reset their own fail counter for free. Now recycles a truly  ║
 * ║           idle slot first, and only evicts the soonest-to-expire       ║
 * ║           locked slot as last resort.                                  ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  KNOWN ARCHITECTURAL LIMITS (honestly scoped, not "fixed")             ║
 * ║  • No TLS — Basic Auth credentials travel in cleartext on the LAN.     ║
 * ║    ESP32 Arduino core has no lightweight HTTPS server suited to a      ║
 * ║    single .ino sketch; would need a full WiFiClientSecure rewrite.    ║
 * ║  • WebServer::authenticate() does a non-constant-time string compare  ║
 * ║    internally (library internal, not exposed for override) — a        ║
 * ║    timing side-channel remains theoretically possible on the LAN.      ║
 * ║  • Brute-force lockout counters live in RAM only — reset on reboot.   ║
 * ║  • checkOrigin() fail-closed means a non-browser API client (e.g.      ║
 * ║    another ESP32 app calling /flash with no Origin/Referer header)     ║
 * ║    will now be rejected by design — CSRF protection and "callable by   ║
 * ║    arbitrary scripts with no browser" are mutually exclusive goals.    ║
 * ║    If you need a non-browser caller, give it its own explicit auth     ║
 * ║    path — don't loosen checkOrigin() itself.                           ║
 * ╚══════════════════════════════════════════════════════════════════════════╝
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
const char* WIFI_SSID       = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD   = "YOUR_WIFI_PASSWORD";

// Web UI Basic Auth credentials
const char* AUTH_USER       = "admin";
const char* AUTH_PASS       = "esp32boot";

// AP fallback (if WiFi STA fails)
// FIX-14: deliberately DIFFERENT from AUTH_PASS — these protect two
// different surfaces (network join vs. web admin login); reusing one
// secret for both means compromising either one compromises both.
const char* AP_SSID         = "ESP32-BootManager";
const char* AP_PASS         = "bootmgr-ap-2026";

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
// FIX-17: same key ReturnToManager.h's rtm_bootSafetyCheck() writes/reads in
// the "mbmgr" namespace — manager resets it to 0 on any manually-requested
// boot so the slot app gets a fresh unconfirmed-boot budget.
#define PREF_BOOT_FAILS  "boot_fails"
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

// ─── FIX-4: shared mutex — guards every prefs.* access so the upload/flash ─
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

// ─── FIX-2/FIX-4: guards against a second upload/flash request while one ──
// is already streaming into an OTA partition.
volatile bool flashInProgress = false;

// ─── FIX-16: same pattern as flashInProgress, for the now-async erase task ─
volatile bool eraseInProgress = false;

// ─── FIX-8: lightweight per-IP brute-force lockout for Basic Auth ─────────
// (struct LoginAttempt is defined near the top of the file — see the
// "FORWARD TYPE DEFINITIONS" block — so it's already known before Arduino's
// hoisted function prototypes need it.)
#define LOGIN_TRACK_SLOTS   8
#define LOGIN_MAX_FAILS     5
#define LOGIN_LOCK_MS       30000UL
LoginAttempt loginTrack[LOGIN_TRACK_SLOTS];

// FIX-19: recycle a genuinely idle slot (never used, or used-but-clean)
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
// FIX-10: there used to be a third state, SLOT_ACTIVE, for "this slot is the
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

// ─── Auth helper (FIX-8: per-IP brute-force lockout added) ─────────────────
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

// ─── FIX-9: CSRF guard — exact host match, fail-closed ─────────────────────
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
// that was the FIX-9 bug: an attacker-controlled Referer path/query like
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

  // FIX-9: fail CLOSED. A browser making a real same-origin request to this
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

// ─── FIX-3/FIX-6: single shared filename sanitizer ─────────────────────────
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
</style>
</head>
<body>
<div class="wrap">

<h1>MultiBoot Manager</h1>
<div class="sub">ESP32 Dual-Slot Persistent Boot System</div>

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
  // FIX-1: no explicit Authorization header — the browser already cached
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
    // is possible here — see FIX-10 comment on getSlotStatus().)
    s += "<button class='btn btn-primary' onclick=\"bootSlot('" + String(slot) + "')\">Boot</button>";
    s += "<button class='btn btn-danger' onclick=\"eraseSlot('" + String(slot) + "')\">Erase</button>";
  }
  return s;
}

String buildPage() {
  SlotStatus stA = getSlotStatus("slot_a");
  SlotStatus stB = getSlotStatus("slot_b");

  // ── Slot A strings (FIX-10: only EMPTY/VALID are reachable states) ───────
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

// FIX-15: esp_ota_abort() does NOT erase flash already written by prior
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
// Stability note: esp_ota_begin() is given the real upload size (from the
// Content-Length header, collected explicitly in setup() via
// server.collectHeaders()) whenever available, so it erases only the
// sectors the incoming image actually needs — not the whole partition.
// Modern esp32-arduino cores yield periodically inside flash-erase loops,
// so this does not trip the idle-task watchdog even for a ~1MB slot.
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

    // Use the real Content-Length when the client sent one (collected via
    // server.collectHeaders() in setup()) so esp_ota_begin() only erases the
    // sectors this image actually needs, instead of the whole partition.
    size_t sizeHint = OTA_SIZE_UNKNOWN;
    String cl = server.header("Content-Length");
    if (cl.length() > 0) {
      long clVal = cl.toInt();
      // Multipart overhead (boundaries/headers) is a few hundred bytes at
      // most — pad generously so esp_ota_begin never under-erases.
      if (clVal > 1024) sizeHint = (size_t)clVal;
    }

    esp_err_t err = esp_ota_begin(g_otaPart, sizeHint, &g_otaHandle);
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

    Serial.println("[UPLOAD] Start → slot_" + slot + "  file=" + fname +
                    "  sizeHint=" + String((long)sizeHint));
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
      prefs.putUChar(PREF_BOOT_FAILS, 0);   // Fresh unconfirmed-boot budget (mirrors FIX-17)
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
  // FIX-17: fresh unconfirmed-boot budget every time a boot is deliberately
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

// ── Background erase task (FIX-16) ──────────────────────────────────────────
// esp_partition_erase_range() on a ~1.25MB slot can take multiple seconds.
// Running it inline inside the HTTP handler blocked server.handleClient()
// for the whole duration — the exact class of bug FIX-2 fixed for flashing,
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
    // FIX-10: no "is this the running partition?" guard needed here — this
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
// FIX-16: now starts a background task and returns immediately (202) instead
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
              + ",\"uptime_s\":" + String(millis() / 1000)  // FIX-18: real device uptime
              + ",\"ap_mode\":" + (apMode ? "true" : "false")
              + ",\"slot_a\":{\"status\":\"" + stStr(stA) + "\",\"name\":\"" + slotAName + "\"}"
              + ",\"slot_b\":{\"status\":\"" + stStr(stB) + "\",\"name\":\"" + slotBName + "\"}"
              + "}";
  server.send(200, "application/json", json);
}

// FIX-11: there used to be a GET /reboot-manager handler here. It's removed
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

  // FIX-4: create the shared Preferences mutex before anything touches it,
  // so it's guaranteed to exist once the WebServer starts handling requests.
  ioMutex = xSemaphoreCreateMutex();

  // ── Boot chain: may restart into a slot ──────────────────────────────────
  handleBootChain();
  // If we reach here, we are in manager mode.

  // ── Load NVS slot names ──────────────────────────────────────────────────
  prefs.begin(PREF_NS, true);
  slotAName = prefs.getString(PREF_SLOT_A_NAME, "");
  slotBName = prefs.getString(PREF_SLOT_B_NAME, "");
  prefs.end();
  Serial.println("[NVS] Slot A: " + (slotAName.length() ? slotAName : "(empty)"));
  Serial.println("[NVS] Slot B: " + (slotBName.length() ? slotBName : "(empty)"));

  // ── WiFi ─────────────────────────────────────────────────────────────────
  Serial.print("[WiFi] Connecting to: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.println("\n[WiFi] STA failed → AP mode");
    Serial.println("[AP]   SSID: " + String(AP_SSID) + "  Pass: " + String(AP_PASS));
    Serial.println("[AP]   IP:   " + WiFi.softAPIP().toString());

    // FEATURE: captive portal — answer every DNS query on the AP with our
    // own IP, so any hostname a connecting phone tries to resolve (used by
    // its captive-portal probe) points straight back at us.
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    Serial.println("[DNS] Captive portal DNS started — connect to AP and the dashboard should open automatically");
  }

  // Explicitly collect Content-Length so handleUploadBody() can read the
  // real upload size via server.header("Content-Length") — WebServer does
  // not expose headers unless they're requested up front like this.
  const char* headersToCollect[] = { "Content-Length" };
  server.collectHeaders(headersToCollect, 1);

  // ── Register routes ───────────────────────────────────────────────────────
  server.on("/",                HTTP_GET,  handleRoot);
  server.on("/status",          HTTP_GET,  handleStatus);
  server.on("/boot-slot",       HTTP_POST, handleBootSlot);
  server.on("/erase-slot",      HTTP_POST, handleEraseSlot);
  server.on("/upload",          HTTP_POST, handleUploadDone, handleUploadBody);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("[Server] Running on port 80");
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  Serial.println("[Server] Open in browser: http://" + ip);
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
