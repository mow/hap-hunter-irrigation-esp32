#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t hunter_protocol_init(void);
esp_err_t hunterStartZone(uint8_t zone, uint16_t minutes);
esp_err_t hunterStopZone(uint8_t zone);
esp_err_t hunterStopAll(void);
