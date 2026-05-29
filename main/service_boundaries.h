#pragma once

#include "esp_err.h"
#include "runtime_state.h"
#include "settings_store.h"

esp_err_t provisioning_service_init(const hunter_settings_t *settings, runtime_state_t *state);
esp_err_t protocol_service_init(void);
esp_err_t homekit_service_init(const hunter_settings_t *settings, runtime_state_t *state);
esp_err_t rest_api_service_init(const hunter_settings_t *settings, runtime_state_t *state);
