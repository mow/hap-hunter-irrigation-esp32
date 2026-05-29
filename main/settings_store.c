#include "settings_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings_store";

void settings_store_defaults(hunter_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    memset(settings, 0, sizeof(*settings));
    settings->wifi_configured = false;
    settings->zone_count = HUNTER_ZONE_COUNT_DEFAULT;
    settings->default_runtime_seconds = HUNTER_RUNTIME_SECONDS_DEFAULT;
    settings->safety_cutoff_seconds = HUNTER_SAFETY_CUTOFF_SECONDS_DEFAULT;
}

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

static uint32_t clamp_runtime_seconds(uint32_t seconds)
{
    if (seconds < HUNTER_RUNTIME_SECONDS_MIN) {
        return HUNTER_RUNTIME_SECONDS_DEFAULT;
    }

    if (seconds > HUNTER_RUNTIME_SECONDS_MAX) {
        return HUNTER_RUNTIME_SECONDS_MAX;
    }

    return seconds;
}

static uint32_t clamp_safety_cutoff_seconds(uint32_t seconds)
{
    if (seconds < HUNTER_SAFETY_CUTOFF_SECONDS_MIN) {
        return HUNTER_SAFETY_CUTOFF_SECONDS_DEFAULT;
    }

    if (seconds > HUNTER_SAFETY_CUTOFF_SECONDS_MAX) {
        return HUNTER_SAFETY_CUTOFF_SECONDS_MAX;
    }

    return seconds;
}

esp_err_t settings_store_load(hunter_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    settings_store_defaults(settings);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HUNTER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No stored settings found; using defaults");
        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed opening NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t wifi_configured = 0;
    if (nvs_get_u8(handle, "wifi_cfg", &wifi_configured) == ESP_OK) {
        settings->wifi_configured = (wifi_configured != 0);
    }

    size_t ssid_size = sizeof(settings->wifi_ssid);
    if (nvs_get_str(handle, "wifi_ssid", settings->wifi_ssid, &ssid_size) != ESP_OK) {
        settings->wifi_ssid[0] = '\0';
    }

    size_t password_size = sizeof(settings->wifi_password);
    if (nvs_get_str(handle, "wifi_pwd", settings->wifi_password, &password_size) != ESP_OK) {
        settings->wifi_password[0] = '\0';
    }

    size_t admin_size = sizeof(settings->admin_password);
    if (nvs_get_str(handle, "adm_pwd", settings->admin_password, &admin_size) != ESP_OK) {
        settings->admin_password[0] = '\0';
    }

    uint8_t zone_count = settings->zone_count;
    if (nvs_get_u8(handle, "zone_count", &zone_count) == ESP_OK) {
        settings->zone_count = clamp_zone_count(zone_count);
    }

    uint32_t runtime_seconds = settings->default_runtime_seconds;
    if (nvs_get_u32(handle, "rt_default", &runtime_seconds) == ESP_OK) {
        settings->default_runtime_seconds = clamp_runtime_seconds(runtime_seconds);
    }

    uint32_t safety_cutoff_seconds = settings->safety_cutoff_seconds;
    if (nvs_get_u32(handle, "safe_cutoff", &safety_cutoff_seconds) == ESP_OK) {
        settings->safety_cutoff_seconds = clamp_safety_cutoff_seconds(safety_cutoff_seconds);
    }

    nvs_close(handle);

    ESP_LOGI(TAG,
             "Loaded settings: wifi_configured=%d, zone_count=%u, default_runtime_seconds=%lu, "
             "safety_cutoff_seconds=%lu",
             settings->wifi_configured,
             settings->zone_count,
             (unsigned long)settings->default_runtime_seconds,
             (unsigned long)settings->safety_cutoff_seconds);
    return ESP_OK;
}

esp_err_t settings_store_save(const hunter_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    hunter_settings_t to_save = *settings;
    to_save.zone_count = clamp_zone_count(to_save.zone_count);
    to_save.default_runtime_seconds = clamp_runtime_seconds(to_save.default_runtime_seconds);
    to_save.safety_cutoff_seconds = clamp_safety_cutoff_seconds(to_save.safety_cutoff_seconds);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HUNTER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed opening NVS namespace for save: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, "wifi_cfg", to_save.wifi_configured ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "wifi_ssid", to_save.wifi_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "wifi_pwd", to_save.wifi_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "adm_pwd", to_save.admin_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "zone_count", to_save.zone_count);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "rt_default", to_save.default_runtime_seconds);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "safe_cutoff", to_save.safety_cutoff_seconds);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed saving settings: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "Saved settings: wifi_configured=%d, ssid_len=%u, zone_count=%u, "
             "default_runtime_seconds=%lu, safety_cutoff_seconds=%lu",
             to_save.wifi_configured,
             (unsigned)strlen(to_save.wifi_ssid),
             to_save.zone_count,
             (unsigned long)to_save.default_runtime_seconds,
             (unsigned long)to_save.safety_cutoff_seconds);
    return ESP_OK;
}

esp_err_t settings_store_clear(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HUNTER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed opening NVS namespace for clear: %s", esp_err_to_name(err));
        return err;
    }

    /* Keep key deletes explicit for schema clarity and forward migration safety. */
    nvs_erase_key(handle, "wifi_cfg");
    nvs_erase_key(handle, "wifi_ssid");
    nvs_erase_key(handle, "wifi_pwd");
    nvs_erase_key(handle, "adm_pwd");
    nvs_erase_key(handle, "rst_seq_cnt");
    nvs_erase_key(handle, "zone_count");
    nvs_erase_key(handle, "rt_default");
    nvs_erase_key(handle, "safe_cutoff");
    nvs_erase_key(handle, "sch_en");
    nvs_erase_key(handle, "sch_hr");
    nvs_erase_key(handle, "sch_min");
    nvs_erase_key(handle, "sch_days");
    nvs_erase_key(handle, "sch_z0");
    nvs_erase_key(handle, "sch_z1");
    nvs_erase_key(handle, "sch_z2");
    nvs_erase_key(handle, "sch_z3");
    nvs_erase_key(handle, "sch_z4");
    nvs_erase_key(handle, "sch_z5");
    nvs_erase_key(handle, "sch_z6");
    nvs_erase_key(handle, "sch_z7");
    nvs_erase_key(handle, "ntp_srv");
    nvs_erase_key(handle, "tz_str");

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed committing cleared settings: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Settings cleared from persistent storage");
    return ESP_OK;
}
