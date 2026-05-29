#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"
#include "runtime_state.h"
#include "settings_store.h"

typedef struct {
	bool initialized;
	int paired_controller_count;
	const char *setup_code;
	const char *setup_id;
} homekit_status_t;

esp_err_t homekit_irrigation_init(const hunter_settings_t *settings, runtime_state_t *state);
esp_err_t homekit_irrigation_get_status(homekit_status_t *status, char *setup_payload, size_t setup_payload_size);
esp_err_t homekit_irrigation_reset_pairings(void);
esp_err_t homekit_irrigation_set_schedule_enabled(bool enabled);
