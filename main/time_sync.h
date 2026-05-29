#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool clock_valid;
    bool ntp_active;
    uint64_t unix_time;
    char ntp_server[64];
    char timezone[64];
    char local_time[40];
} time_sync_status_t;

esp_err_t time_sync_init(void);
esp_err_t time_sync_on_network_connected(void);
esp_err_t time_sync_set_ntp_server(const char *server, bool persist);
esp_err_t time_sync_set_timezone(const char *timezone, bool persist);
esp_err_t time_sync_set_manual_epoch(uint64_t epoch_seconds);
esp_err_t time_sync_get_status(time_sync_status_t *status);
