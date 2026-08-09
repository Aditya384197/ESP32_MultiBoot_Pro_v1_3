/*
 * ═══════════════════════════════════════════════════════════════════════════
 *   ReturnToManager.h  —  v3 (root-cause fix + auto-recovery boot safety)
 *   Include this in EVERY app you flash into slot_a / slot_b.
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * WHY v1 WAS BROKEN (see MultiBoot Manager V1.2 changelog for the full
 * writeup):
 *   The ESP-IDF 2nd-stage bootloader decides which app partition to boot by
 *   reading the "otadata" partition — nothing else. It never looks at your
 *   app's own NVS/Preferences namespace. esp_ota_set_boot_partition() is the
 *   ONLY API call that changes otadata. Once the MultiBoot Manager has
 *   chained into slot_a (by calling esp_ota_set_boot_partition(slot_a) then
 *   esp_restart()), otadata stays pointed at slot_a forever — across any
 *   number of future restarts — until something calls
 *   esp_ota_set_boot_partition() again to point it somewhere else.
 *
 *   A v1 implementation that only wrote a Preferences flag like
 *   "mbmgr/boot_tgt = manager" and called ESP.restart() changed nothing the
 *   bootloader reads. The device would simply reboot straight back into the
 *   same slot_a app — the manager would never regain control, silently.
 *
 * THE FIX (v2):
 *   returnToManager() finds the factory partition (subtype
 *   ESP_PARTITION_SUBTYPE_APP_FACTORY — this is the MultiBoot Manager
 *   itself) and calls esp_ota_set_boot_partition() on THAT partition
 *   directly. That's the only thing that actually flips otadata back to
 *   factory. The NVS flag is still written too, purely so the manager's own
 *   UI has a value to read on boot — it is NOT what makes the return work.
 *
 * WHY v3 ADDS BOOT-SAFETY (FIX-17, auto-recovery):
 *   v2 solved the case where a HEALTHY slot app deliberately asks to return
 *   to the manager. It did nothing for the case where a slot app is BROKEN
 *   and never gets far enough to call returnToManager() at all — e.g. it
 *   crashes early in setup(), or hangs before ever reaching the code that
 *   would return control. In that situation otadata still points at the
 *   broken slot, the manager's own GPIO0 boot-chain check never runs
 *   (manager code isn't executing — a different partition is), and the
 *   device just crash-reboots into the same broken app forever with zero
 *   software recovery path.
 *
 *   rtm_bootSafetyCheck() closes most of that gap by running INSIDE the
 *   slot app itself, as early as possible:
 *     • GPIO0 held LOW at boot → immediate return to manager (this makes
 *       the "hold GPIO0 + Reset" instructions on the manager's dashboard
 *       actually true while a slot is running — previously that check only
 *       existed in the manager's own boot-chain code, which cannot run
 *       once a slot is active).
 *     • A consecutive-unconfirmed-boot counter in NVS. Every boot increments
 *       it before anything else; call rtm_markBootOK() once your app is
 *       confirmed stable (e.g. after WiFi connects) to reset it to 0. If it
 *       reaches RTM_MAX_BOOT_FAILS before ever being reset, the NEXT boot
 *       auto-reverts to the manager instead of trying the broken slot again.
 *
 *   HONEST LIMIT: if the slot binary is broken badly enough to crash or hang
 *   BEFORE rtm_bootSafetyCheck() itself executes (corrupt image, crash
 *   before setup(), watchdog-starving infinite loop before this call), this
 *   cannot help — no code running inside a partition can protect against
 *   never running. Physical recovery (GPIO0+Reset, or reflashing over
 *   serial) remains the fallback for that case. This mechanism protects
 *   against the much more common case: an app that boots far enough to run
 *   setup() but then fails before confirming itself stable.
 *
 * USAGE (from your slot app):
 *   #include "ReturnToManager.h"
 *
 *   void setup() {
 *     rtm_bootSafetyCheck();   // FIRST line of setup() — always
 *     ...
 *     // once WiFi/sensors/whatever confirms this boot is healthy:
 *     rtm_markBootOK();
 *     ...
 *   }
 *
 *   server.on("/reboot-manager", HTTP_GET, [](){
 *     server.send(200, "text/plain", "Returning to manager...");
 *     returnToManager();   // never returns
 *   });
 * ═══════════════════════════════════════════════════════════════════════════
 */

#ifndef RETURN_TO_MANAGER_H
#define RETURN_TO_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// Must match the MultiBoot Manager's own PREF_NS / PREF_BOOT_TARGET /
// TARGET_MANAGER constants — written for UI-consistency only, see note
// above on returnToManager().
#ifndef RTM_PREF_NS
#define RTM_PREF_NS          "mbmgr"
#endif
#ifndef RTM_PREF_BOOT_TARGET
#define RTM_PREF_BOOT_TARGET "boot_tgt"
#endif
#ifndef RTM_TARGET_MANAGER
#define RTM_TARGET_MANAGER   "manager"
#endif

// FIX-17: must match the manager's own PREF_BOOT_FAILS constant — the
// manager resets this to 0 whenever a boot is deliberately requested from
// the dashboard, so a slot app always starts with a fresh budget on a
// user-initiated boot.
#ifndef RTM_PREF_BOOT_FAILS
#define RTM_PREF_BOOT_FAILS  "boot_fails"
#endif
#ifndef RTM_MAX_BOOT_FAILS
#define RTM_MAX_BOOT_FAILS   3
#endif
#ifndef RTM_BOOT_PIN
#define RTM_BOOT_PIN         0
#endif

// Returns false (does NOT restart) if the factory partition couldn't be
// found or the switch failed — caller can decide how to handle that instead
// of silently rebooting into an undefined state. On success this function
// never returns (it calls esp_restart()).
inline bool returnToManager() {
  const esp_partition_t* factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);

  if (!factory) {
    Serial.println("[ReturnToManager] ERROR: factory partition not found — "
                    "check your partition table matches partitions_multiboot.csv");
    return false;
  }

  // This is the call that actually matters — it rewrites otadata so the
  // NEXT restart's 2nd-stage bootloader picks the factory (manager)
  // partition instead of whatever slot we're currently running from.
  esp_err_t err = esp_ota_set_boot_partition(factory);
  if (err != ESP_OK) {
    Serial.printf("[ReturnToManager] ERROR: esp_ota_set_boot_partition failed: 0x%X\n", err);
    return false;
  }

  // Cosmetic only — lets the manager's own UI show "manager" as the last
  // known target without having to re-derive it. Does NOT affect boot
  // behavior; that was v1's mistake.
  Preferences prefs;
  prefs.begin(RTM_PREF_NS, false);
  prefs.putString(RTM_PREF_BOOT_TARGET, RTM_TARGET_MANAGER);
  prefs.end();

  Serial.println("[ReturnToManager] otadata → factory. Restarting...");
  delay(200);
  esp_restart();
  return true;  // unreachable — esp_restart() never returns
}

// FIX-17 (auto-recovery): call this as the FIRST line of your slot app's
// setup(), before anything that could hang or crash. It:
//   1. Checks GPIO0 — if held LOW at boot, returns to the manager
//      immediately (makes the manager dashboard's "hold GPIO0 + Reset"
//      instructions actually work while a slot is running).
//   2. Increments a consecutive-unconfirmed-boot counter in NVS. If this
//      boot is the RTM_MAX_BOOT_FAILS-th one in a row without a prior
//      rtm_markBootOK() call, returns to the manager instead of letting
//      the (apparently broken) app continue.
// Does nothing and returns normally otherwise — your setup() continues.
inline void rtm_bootSafetyCheck() {
  pinMode(RTM_BOOT_PIN, INPUT_PULLUP);
  delay(10);
  bool gpio0Low = (digitalRead(RTM_BOOT_PIN) == LOW);

  Preferences prefs;
  prefs.begin(RTM_PREF_NS, false);
  uint8_t fails = prefs.getUChar(RTM_PREF_BOOT_FAILS, 0);
  fails = (fails >= 255) ? 255 : fails + 1;   // avoid uint8_t wraparound
  prefs.putUChar(RTM_PREF_BOOT_FAILS, fails);
  prefs.end();

  if (gpio0Low) {
    Serial.println("[ReturnToManager] GPIO0 held LOW at boot — returning to manager");
    returnToManager();  // never returns
  }
  if (fails >= RTM_MAX_BOOT_FAILS) {
    Serial.printf("[ReturnToManager] %u consecutive unconfirmed boots — "
                  "auto-returning to manager\n", fails);
    returnToManager();  // never returns
  }
}

// FIX-17: call once your app is confirmed stable — e.g. after WiFi connects,
// or after N seconds of a clean loop() with no crash — to reset the
// unconfirmed-boot counter back to 0. Safe to call more than once.
inline void rtm_markBootOK() {
  Preferences prefs;
  prefs.begin(RTM_PREF_NS, false);
  prefs.putUChar(RTM_PREF_BOOT_FAILS, 0);
  prefs.end();
}

#endif // RETURN_TO_MANAGER_H
