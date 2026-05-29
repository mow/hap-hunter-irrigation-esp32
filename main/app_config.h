#pragma once

#include <stdint.h>

#define HUNTER_FW_VERSION "0.1.0-dev"

#define HUNTER_PROTOCOL_GPIO 4

#define HUNTER_ZONE_COUNT_MIN 1
#define HUNTER_ZONE_COUNT_MAX 8
#define HUNTER_ZONE_COUNT_DEFAULT 4

#define HUNTER_RUNTIME_SECONDS_MIN 60
#define HUNTER_RUNTIME_SECONDS_MAX (2 * 60 * 60)
#define HUNTER_RUNTIME_SECONDS_DEFAULT (10 * 60)

#define HUNTER_SAFETY_CUTOFF_SECONDS_MIN (5 * 60)
#define HUNTER_SAFETY_CUTOFF_SECONDS_MAX (4 * 60 * 60)
#define HUNTER_SAFETY_CUTOFF_SECONDS_DEFAULT (60 * 60)

#define HUNTER_WATCHDOG_TIMEOUT_SECONDS 10

/* Board-level choice: use GPIO18 for status LED on this build. */
#define HUNTER_STATUS_LED_GPIO 18

/* Use BOOT button GPIO on common ESP32-C3 dev boards; set to -1 to disable. */
#define HUNTER_RESET_BUTTON_GPIO 9
#define HUNTER_RESET_BUTTON_ACTIVE_LEVEL 0

#define HUNTER_RESET_LONG_PRESS_MS 3000

/* Safety behavior: force an all-zones stop command on every boot. */
#define HUNTER_STOP_ZONES_ON_BOOT 1

/* HunterRoam reference timings for REM bus signaling. */
#define HUNTER_START_INTERVAL_US 900
#define HUNTER_SHORT_INTERVAL_US 208
#define HUNTER_LONG_INTERVAL_US 1875

#define HUNTER_WIFI_SSID_MAX_LEN 32
#define HUNTER_WIFI_PASSWORD_MAX_LEN 64
#define HUNTER_ADMIN_PASSWORD_MAX_LEN 32
#define HUNTER_SETUP_AP_SSID "HunterSetupAP"
#define HUNTER_SETUP_AP_CHANNEL 1

#define HUNTER_NVS_NAMESPACE "hunter_cfg"

#if !CONFIG_IDF_TARGET_ESP32C3
#error "This firmware baseline currently supports only ESP32-C3 targets."
#endif
