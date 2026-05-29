#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "runtime_state.h"
#include "settings_store.h"

esp_err_t irrigation_runtime_init(runtime_state_t *state, const hunter_settings_t *settings);
esp_err_t irrigation_runtime_start_zone(uint8_t zone, uint16_t minutes);
esp_err_t irrigation_runtime_stop_all(void);
