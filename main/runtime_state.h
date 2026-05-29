#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CONTROL_MODE_SETUP_AP = 0,
    CONTROL_MODE_STATION = 1,
} control_mode_t;

typedef struct {
    control_mode_t control_mode;
    uint8_t active_zone;
    uint32_t remaining_seconds;
    uint32_t elapsed_seconds;
    bool network_connected;
} runtime_state_t;

void runtime_state_init(runtime_state_t *state, control_mode_t mode);
const char *runtime_state_mode_name(control_mode_t mode);
