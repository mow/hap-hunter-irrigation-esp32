#include "runtime_state.h"

#include <string.h>

void runtime_state_init(runtime_state_t *state, control_mode_t mode)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->control_mode = mode;
    state->active_zone = 0;
    state->remaining_seconds = 0;
    state->elapsed_seconds = 0;
    state->network_connected = false;
}

const char *runtime_state_mode_name(control_mode_t mode)
{
    return (mode == CONTROL_MODE_STATION) ? "station" : "setup_ap";
}
