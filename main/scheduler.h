#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "runtime_state.h"
#include "settings_store.h"

typedef struct {
    bool enabled;
    uint8_t start_hour;
    uint8_t start_minute;
    uint8_t days_mask;
    uint16_t zone_minutes[HUNTER_ZONE_COUNT_MAX];
} hunter_schedule_t;

typedef struct {
    bool clock_valid;
    bool run_active;
    uint8_t current_zone;
    uint8_t next_zone;
    bool enabled;
    uint8_t start_hour;
    uint8_t start_minute;
    uint8_t days_mask;
} hunter_schedule_status_t;

esp_err_t scheduler_init(const hunter_settings_t *settings, runtime_state_t *state);
esp_err_t scheduler_get_config(hunter_schedule_t *config);
esp_err_t scheduler_set_config(const hunter_schedule_t *config, uint8_t zone_count);
esp_err_t scheduler_get_status(hunter_schedule_status_t *status, uint8_t zone_count);
