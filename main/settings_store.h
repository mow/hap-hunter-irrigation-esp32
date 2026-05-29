#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "app_config.h"

typedef struct {
    bool wifi_configured;
    char wifi_ssid[HUNTER_WIFI_SSID_MAX_LEN + 1];
    char wifi_password[HUNTER_WIFI_PASSWORD_MAX_LEN + 1];
    char admin_password[HUNTER_ADMIN_PASSWORD_MAX_LEN + 1];
    uint8_t zone_count;
    uint32_t default_runtime_seconds;
    uint32_t safety_cutoff_seconds;
} hunter_settings_t;

void settings_store_defaults(hunter_settings_t *settings);
esp_err_t settings_store_load(hunter_settings_t *settings);
esp_err_t settings_store_save(const hunter_settings_t *settings);
esp_err_t settings_store_clear(void);
