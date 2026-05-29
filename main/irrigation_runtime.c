#include "irrigation_runtime.h"

#include <stdbool.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hunter_protocol.h"

static const char *TAG = "irrig_runtime";

typedef struct {
    runtime_state_t *state;
    uint32_t safety_cutoff_seconds;
    esp_timer_handle_t tick_timer;
    bool initialized;
} irrigation_runtime_ctx_t;

static irrigation_runtime_ctx_t s_ctx;

static void stop_active_zone_from_timer(const char *reason)
{
    if (s_ctx.state == NULL || s_ctx.state->active_zone == 0) {
        return;
    }

    uint8_t active_zone = s_ctx.state->active_zone;
    ESP_LOGW(TAG,
             "Stopping zone %u due to %s (remaining=%lu elapsed=%lu)",
             active_zone,
             reason,
             (unsigned long)s_ctx.state->remaining_seconds,
             (unsigned long)s_ctx.state->elapsed_seconds);

    esp_err_t err = hunterStopZone(active_zone);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "hunterStopZone(%u) failed while handling %s: %s; escalating to stop sweep",
                 active_zone,
                 reason,
                 esp_err_to_name(err));
        esp_err_t sweep_err = hunterStopAll();
        if (sweep_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "hunterStopAll fallback also failed for %s: %s",
                     reason,
                     esp_err_to_name(sweep_err));
        }
    }

    s_ctx.state->active_zone = 0;
    s_ctx.state->remaining_seconds = 0;
    s_ctx.state->elapsed_seconds = 0;
}

static void runtime_tick_callback(void *arg)
{
    (void)arg;

    if (s_ctx.state == NULL || s_ctx.state->active_zone == 0) {
        return;
    }

    if (s_ctx.state->active_zone < HUNTER_ZONE_COUNT_MIN ||
        s_ctx.state->active_zone > HUNTER_ZONE_COUNT_MAX) {
        ESP_LOGE(TAG,
                 "Invalid active zone detected in runtime state: %u. Resetting runtime counters.",
                 s_ctx.state->active_zone);
        s_ctx.state->active_zone = 0;
        s_ctx.state->remaining_seconds = 0;
        s_ctx.state->elapsed_seconds = 0;
        return;
    }

    if (s_ctx.state->remaining_seconds > 0) {
        s_ctx.state->remaining_seconds--;
    }
    s_ctx.state->elapsed_seconds++;

    if (s_ctx.state->elapsed_seconds >= s_ctx.safety_cutoff_seconds) {
        stop_active_zone_from_timer("safety cutoff reached");
        return;
    }

    if (s_ctx.state->remaining_seconds == 0) {
        stop_active_zone_from_timer("requested runtime expired");
    }
}

static void watchdog_task(void *arg)
{
    (void)arg;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "Watchdog supervision task started");

    while (true) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t irrigation_runtime_init(runtime_state_t *state, const hunter_settings_t *settings)
{
    if (state == NULL || settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (settings->safety_cutoff_seconds < HUNTER_SAFETY_CUTOFF_SECONDS_MIN ||
        settings->safety_cutoff_seconds > HUNTER_SAFETY_CUTOFF_SECONDS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state = state;
    s_ctx.safety_cutoff_seconds = settings->safety_cutoff_seconds;

    const esp_timer_create_args_t timer_args = {
        .callback = runtime_tick_callback,
        .name = "runtime_tick",
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ctx.tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_ctx.tick_timer, 1000000));

    const esp_task_wdt_config_t wdt_config = {
        .timeout_ms = HUNTER_WATCHDOG_TIMEOUT_SECONDS * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };

    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_config);
    if (wdt_err == ESP_ERR_NOT_FOUND) {
        ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_config));
    } else {
        ESP_ERROR_CHECK(wdt_err);
    }

    BaseType_t task_ok = xTaskCreate(watchdog_task, "rt_watchdog", 2048, NULL, 5, NULL);
    if (task_ok != pdPASS) {
        return ESP_FAIL;
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG,
             "Runtime reliability initialized (safety_cutoff_seconds=%lu watchdog_timeout=%d)",
             (unsigned long)s_ctx.safety_cutoff_seconds,
             HUNTER_WATCHDOG_TIMEOUT_SECONDS);
    return ESP_OK;
}

esp_err_t irrigation_runtime_start_zone(uint8_t zone, uint16_t minutes)
{
    if (!s_ctx.initialized || s_ctx.state == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (zone < HUNTER_ZONE_COUNT_MIN || zone > HUNTER_ZONE_COUNT_MAX || minutes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t requested_seconds = (uint32_t)minutes * 60U;
    if (requested_seconds > s_ctx.safety_cutoff_seconds) {
        requested_seconds = s_ctx.safety_cutoff_seconds;
    }

    if (s_ctx.state->active_zone != 0 && s_ctx.state->active_zone != zone) {
        ESP_LOGW(TAG,
                 "Zone %u requested while zone %u active; stopping current first",
                 zone,
                 s_ctx.state->active_zone);
        esp_err_t stop_err = irrigation_runtime_stop_all();
        if (stop_err != ESP_OK) {
            return stop_err;
        }
    }

    esp_err_t start_err = hunterStartZone(zone, minutes);
    if (start_err != ESP_OK) {
        return start_err;
    }
    s_ctx.state->active_zone = zone;
    s_ctx.state->remaining_seconds = requested_seconds;
    s_ctx.state->elapsed_seconds = 0;

    ESP_LOGI(TAG,
             "Zone %u started for %u min (tracked_remaining_seconds=%lu)",
             zone,
             minutes,
             (unsigned long)s_ctx.state->remaining_seconds);
    return ESP_OK;
}

esp_err_t irrigation_runtime_stop_all(void)
{
    if (!s_ctx.initialized || s_ctx.state == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = hunterStopAll();
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.state->active_zone = 0;
    s_ctx.state->remaining_seconds = 0;
    s_ctx.state->elapsed_seconds = 0;

    ESP_LOGI(TAG, "All zones stopped by runtime controller");
    return ESP_OK;
}
