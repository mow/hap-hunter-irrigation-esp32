#include "scheduler.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "homekit_irrigation.h"
#include "irrigation_runtime.h"
#include "nvs.h"

static const char *TAG = "scheduler";

#define SCHED_KEY_ENABLED "sch_en"
#define SCHED_KEY_HOUR "sch_hr"
#define SCHED_KEY_MINUTE "sch_min"
#define SCHED_KEY_DAYS "sch_days"
#define SCHED_KEY_ZONE_PREFIX "sch_z"

typedef struct {
    bool initialized;
    runtime_state_t *state;
    uint8_t zone_count;
    hunter_schedule_t config;
    bool run_active;
    uint8_t next_zone;
    int last_trigger_year;
    int last_trigger_yday;
    esp_timer_handle_t tick_timer;
} scheduler_ctx_t;

static scheduler_ctx_t s_ctx;

static uint8_t clamp_zone_count(uint8_t zone_count)
{
    if (zone_count < HUNTER_ZONE_COUNT_MIN) {
        return HUNTER_ZONE_COUNT_DEFAULT;
    }
    if (zone_count > HUNTER_ZONE_COUNT_MAX) {
        return HUNTER_ZONE_COUNT_MAX;
    }
    return zone_count;
}

static uint8_t clamp_hour(uint8_t hour)
{
    return (hour > 23U) ? 6U : hour;
}

static uint8_t clamp_minute(uint8_t minute)
{
    return (minute > 59U) ? 0U : minute;
}

static uint16_t clamp_zone_minutes(uint16_t minutes)
{
    if (minutes == 0U) {
        return 0U;
    }
    if (minutes > 240U) {
        return 240U;
    }
    return minutes;
}

static void scheduler_defaults(hunter_schedule_t *config, const hunter_settings_t *settings)
{
    memset(config, 0, sizeof(*config));
    config->enabled = false;
    config->start_hour = 6;
    config->start_minute = 0;
    config->days_mask = 0x7FU;

    uint16_t default_minutes = (uint16_t)((settings->default_runtime_seconds + 59U) / 60U);
    if (default_minutes == 0U) {
        default_minutes = 1U;
    }

    for (uint8_t i = 0; i < HUNTER_ZONE_COUNT_MAX; ++i) {
        config->zone_minutes[i] = default_minutes;
    }
}

static void normalize_schedule(hunter_schedule_t *config, uint8_t zone_count)
{
    uint8_t clamped_zone_count = clamp_zone_count(zone_count);

    config->start_hour = clamp_hour(config->start_hour);
    config->start_minute = clamp_minute(config->start_minute);
    config->days_mask &= 0x7FU;

    for (uint8_t i = 0; i < HUNTER_ZONE_COUNT_MAX; ++i) {
        if (i < clamped_zone_count) {
            config->zone_minutes[i] = clamp_zone_minutes(config->zone_minutes[i]);
            if (config->zone_minutes[i] == 0U) {
                config->zone_minutes[i] = 1U;
            }
        } else {
            config->zone_minutes[i] = 0U;
        }
    }
}

static esp_err_t save_schedule_to_nvs(const hunter_schedule_t *config)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HUNTER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, SCHED_KEY_ENABLED, config->enabled ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SCHED_KEY_HOUR, config->start_hour);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SCHED_KEY_MINUTE, config->start_minute);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SCHED_KEY_DAYS, config->days_mask);
    }

    if (err == ESP_OK) {
        for (uint8_t i = 0; i < HUNTER_ZONE_COUNT_MAX; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "%s%u", SCHED_KEY_ZONE_PREFIX, (unsigned)i);
            err = nvs_set_u16(handle, key, config->zone_minutes[i]);
            if (err != ESP_OK) {
                break;
            }
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t load_schedule_from_nvs(hunter_schedule_t *config, const hunter_settings_t *settings)
{
    scheduler_defaults(config, settings);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HUNTER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t u8 = 0;
    if (nvs_get_u8(handle, SCHED_KEY_ENABLED, &u8) == ESP_OK) {
        config->enabled = (u8 != 0U);
    }
    if (nvs_get_u8(handle, SCHED_KEY_HOUR, &u8) == ESP_OK) {
        config->start_hour = u8;
    }
    if (nvs_get_u8(handle, SCHED_KEY_MINUTE, &u8) == ESP_OK) {
        config->start_minute = u8;
    }
    if (nvs_get_u8(handle, SCHED_KEY_DAYS, &u8) == ESP_OK) {
        config->days_mask = u8;
    }

    for (uint8_t i = 0; i < HUNTER_ZONE_COUNT_MAX; ++i) {
        uint16_t minutes = 0;
        char key[8];
        snprintf(key, sizeof(key), "%s%u", SCHED_KEY_ZONE_PREFIX, (unsigned)i);
        if (nvs_get_u16(handle, key, &minutes) == ESP_OK) {
            config->zone_minutes[i] = minutes;
        }
    }

    nvs_close(handle);
    return ESP_OK;
}

static bool get_local_time(time_t *now, struct tm *tm_now)
{
    if (now == NULL || tm_now == NULL) {
        return false;
    }

    time(now);
    if (*now <= 0) {
        return false;
    }

    localtime_r(now, tm_now);
    return (tm_now->tm_year + 1900) >= 2024;
}

static int start_next_scheduled_zone(void)
{
    uint8_t limit = clamp_zone_count(s_ctx.zone_count);
    for (uint8_t i = s_ctx.next_zone; i < limit; ++i) {
        uint16_t minutes = s_ctx.config.zone_minutes[i];
        if (minutes == 0U) {
            continue;
        }

        esp_err_t err = irrigation_runtime_start_zone((uint8_t)(i + 1U), minutes);
        if (err == ESP_OK) {
            s_ctx.next_zone = (uint8_t)(i + 1U);
            ESP_LOGI(TAG,
                     "Scheduled run started zone %u for %u minute(s)",
                     (unsigned)(i + 1U),
                     (unsigned)minutes);
            return 1;
        }

        ESP_LOGW(TAG,
                 "Failed starting scheduled zone %u: %s",
                 (unsigned)(i + 1U),
                 esp_err_to_name(err));
    }

    return 0;
}

static void scheduler_tick_callback(void *arg)
{
    (void)arg;

    if (!s_ctx.initialized || s_ctx.state == NULL || !s_ctx.config.enabled) {
        return;
    }

    time_t now;
    struct tm tm_now;
    if (!get_local_time(&now, &tm_now)) {
        return;
    }

    if (s_ctx.run_active) {
        if (s_ctx.state->active_zone == 0U) {
            if (start_next_scheduled_zone() == 0) {
                s_ctx.run_active = false;
                s_ctx.next_zone = 0;
                ESP_LOGI(TAG, "Scheduled run completed");
            }
        }
        return;
    }

    if ((s_ctx.config.days_mask & (1U << tm_now.tm_wday)) == 0U) {
        return;
    }

    if (tm_now.tm_hour != s_ctx.config.start_hour ||
        tm_now.tm_min != s_ctx.config.start_minute ||
        tm_now.tm_sec > 2) {
        return;
    }

    if (s_ctx.last_trigger_year == tm_now.tm_year && s_ctx.last_trigger_yday == tm_now.tm_yday) {
        return;
    }

    s_ctx.last_trigger_year = tm_now.tm_year;
    s_ctx.last_trigger_yday = tm_now.tm_yday;
    s_ctx.run_active = true;
    s_ctx.next_zone = 0;

    ESP_LOGI(TAG,
             "Scheduled run triggered for %02u:%02u day_mask=0x%02X",
             (unsigned)s_ctx.config.start_hour,
             (unsigned)s_ctx.config.start_minute,
             (unsigned)s_ctx.config.days_mask);

    if (start_next_scheduled_zone() == 0) {
        s_ctx.run_active = false;
        s_ctx.next_zone = 0;
    }
}

esp_err_t scheduler_init(const hunter_settings_t *settings, runtime_state_t *state)
{
    if (settings == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state = state;
    s_ctx.zone_count = clamp_zone_count(settings->zone_count);
    s_ctx.last_trigger_year = -1;
    s_ctx.last_trigger_yday = -1;

    esp_err_t load_err = load_schedule_from_nvs(&s_ctx.config, settings);
    if (load_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed loading schedule from NVS, using defaults: %s", esp_err_to_name(load_err));
        scheduler_defaults(&s_ctx.config, settings);
    }
    normalize_schedule(&s_ctx.config, s_ctx.zone_count);

    const esp_timer_create_args_t timer_args = {
        .callback = scheduler_tick_callback,
        .name = "sched_tick",
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ctx.tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_ctx.tick_timer, 1000000));

    s_ctx.initialized = true;

    (void)homekit_irrigation_set_schedule_enabled(s_ctx.config.enabled);

    ESP_LOGI(TAG,
             "Scheduler initialized: enabled=%d start=%02u:%02u days_mask=0x%02X zone_count=%u",
             s_ctx.config.enabled,
             (unsigned)s_ctx.config.start_hour,
             (unsigned)s_ctx.config.start_minute,
             (unsigned)s_ctx.config.days_mask,
             (unsigned)s_ctx.zone_count);
    return ESP_OK;
}

esp_err_t scheduler_get_config(hunter_schedule_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *config = s_ctx.config;
    return ESP_OK;
}

esp_err_t scheduler_set_config(const hunter_schedule_t *config, uint8_t zone_count)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    hunter_schedule_t updated = *config;
    uint8_t clamped_zone_count = clamp_zone_count(zone_count);
    normalize_schedule(&updated, clamped_zone_count);

    esp_err_t err = save_schedule_to_nvs(&updated);
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.zone_count = clamped_zone_count;
    s_ctx.config = updated;
    s_ctx.run_active = false;
    s_ctx.next_zone = 0;
    s_ctx.last_trigger_year = -1;
    s_ctx.last_trigger_yday = -1;

    (void)homekit_irrigation_set_schedule_enabled(s_ctx.config.enabled);

    ESP_LOGI(TAG,
             "Scheduler updated: enabled=%d start=%02u:%02u days_mask=0x%02X",
             s_ctx.config.enabled,
             (unsigned)s_ctx.config.start_hour,
             (unsigned)s_ctx.config.start_minute,
             (unsigned)s_ctx.config.days_mask);

    return ESP_OK;
}

esp_err_t scheduler_get_status(hunter_schedule_status_t *status, uint8_t zone_count)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t now;
    struct tm tm_now;
    memset(status, 0, sizeof(*status));

    status->clock_valid = get_local_time(&now, &tm_now);
    status->run_active = s_ctx.run_active;
    status->current_zone = (s_ctx.state != NULL) ? s_ctx.state->active_zone : 0U;
    uint8_t bounded_zone_count = clamp_zone_count(zone_count);
    if (s_ctx.run_active && s_ctx.next_zone + 1U < bounded_zone_count) {
        status->next_zone = (uint8_t)(s_ctx.next_zone + 2U);
    } else {
        status->next_zone = 0U;
    }
    status->enabled = s_ctx.config.enabled;
    status->start_hour = s_ctx.config.start_hour;
    status->start_minute = s_ctx.config.start_minute;
    status->days_mask = s_ctx.config.days_mask;
    return ESP_OK;
}
