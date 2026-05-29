#include "time_sync.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs.h"

static const char *TAG = "time_sync";

#define DEFAULT_NTP_SERVER "time.cloudflare.com"
#define NTP_SERVER_KEY "ntp_srv"
#define DEFAULT_TIMEZONE "UTC0"
#define TIMEZONE_KEY "tz_str"
#define MIN_VALID_EPOCH 1704067200ULL

typedef struct {
    bool initialized;
    bool ntp_active;
    char ntp_server[64];
    char timezone[64];
} time_sync_ctx_t;

static time_sync_ctx_t s_ctx;

static bool time_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return ((uint64_t)now) >= MIN_VALID_EPOCH;
}

static esp_err_t load_ntp_server(char *server, size_t server_size)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HUNTER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t value_size = server_size;
    err = nvs_get_str(handle, NTP_SERVER_KEY, server, &value_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t load_timezone(char *timezone, size_t timezone_size)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HUNTER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t value_size = timezone_size;
    err = nvs_get_str(handle, TIMEZONE_KEY, timezone, &value_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t persist_ntp_server(const char *server)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HUNTER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NTP_SERVER_KEY, server);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t persist_timezone(const char *timezone)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HUNTER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, TIMEZONE_KEY, timezone);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t apply_timezone(const char *timezone)
{
    if (timezone == NULL || timezone[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (setenv("TZ", timezone, 1) != 0) {
        return ESP_FAIL;
    }

    tzset();
    ESP_LOGI(TAG, "Timezone applied: %s", timezone);
    return ESP_OK;
}

static esp_err_t restart_sntp(void)
{
    esp_sntp_stop();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_ctx.ntp_server);
    esp_sntp_init();
    s_ctx.ntp_active = true;
    ESP_LOGI(TAG, "SNTP started with server=%s", s_ctx.ntp_server);
    return ESP_OK;
}

esp_err_t time_sync_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    strncpy(s_ctx.ntp_server, DEFAULT_NTP_SERVER, sizeof(s_ctx.ntp_server) - 1);
    strncpy(s_ctx.timezone, DEFAULT_TIMEZONE, sizeof(s_ctx.timezone) - 1);

    char stored_server[64] = {0};
    if (load_ntp_server(stored_server, sizeof(stored_server)) == ESP_OK && stored_server[0] != '\0') {
        strncpy(s_ctx.ntp_server, stored_server, sizeof(s_ctx.ntp_server) - 1);
    }

    char stored_timezone[64] = {0};
    if (load_timezone(stored_timezone, sizeof(stored_timezone)) == ESP_OK && stored_timezone[0] != '\0') {
        strncpy(s_ctx.timezone, stored_timezone, sizeof(s_ctx.timezone) - 1);
    }

    esp_err_t tz_err = apply_timezone(s_ctx.timezone);
    if (tz_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed applying timezone '%s': %s", s_ctx.timezone, esp_err_to_name(tz_err));
        strncpy(s_ctx.timezone, DEFAULT_TIMEZONE, sizeof(s_ctx.timezone) - 1);
        s_ctx.timezone[sizeof(s_ctx.timezone) - 1] = '\0';
        ESP_ERROR_CHECK(apply_timezone(s_ctx.timezone));
    }

    s_ctx.initialized = true;

    ESP_LOGI(TAG,
             "Time sync initialized: ntp_server=%s timezone=%s clock_valid=%d",
             s_ctx.ntp_server,
             s_ctx.timezone,
             time_is_valid());
    return ESP_OK;
}

esp_err_t time_sync_on_network_connected(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return restart_sntp();
}

esp_err_t time_sync_set_ntp_server(const char *server, bool persist)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (server == NULL || server[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strnlen(server, sizeof(s_ctx.ntp_server));
    if (len == 0 || len >= sizeof(s_ctx.ntp_server)) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_ctx.ntp_server, server, sizeof(s_ctx.ntp_server) - 1);
    s_ctx.ntp_server[sizeof(s_ctx.ntp_server) - 1] = '\0';

    if (persist) {
        esp_err_t err = persist_ntp_server(s_ctx.ntp_server);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (s_ctx.ntp_active) {
        return restart_sntp();
    }

    ESP_LOGI(TAG, "NTP server updated: %s", s_ctx.ntp_server);
    return ESP_OK;
}

esp_err_t time_sync_set_timezone(const char *timezone, bool persist)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timezone == NULL || timezone[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strnlen(timezone, sizeof(s_ctx.timezone));
    if (len == 0 || len >= sizeof(s_ctx.timezone)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t tz_err = apply_timezone(timezone);
    if (tz_err != ESP_OK) {
        return tz_err;
    }

    strncpy(s_ctx.timezone, timezone, sizeof(s_ctx.timezone) - 1);
    s_ctx.timezone[sizeof(s_ctx.timezone) - 1] = '\0';

    if (persist) {
        return persist_timezone(s_ctx.timezone);
    }

    return ESP_OK;
}

esp_err_t time_sync_set_manual_epoch(uint64_t epoch_seconds)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (epoch_seconds < MIN_VALID_EPOCH || epoch_seconds > 4102444800ULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct timeval tv = {
        .tv_sec = (time_t)epoch_seconds,
        .tv_usec = 0,
    };

    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Manual time set: epoch=%llu", (unsigned long long)epoch_seconds);
    return ESP_OK;
}

esp_err_t time_sync_get_status(time_sync_status_t *status)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    status->clock_valid = time_is_valid();
    status->ntp_active = s_ctx.ntp_active;

    time_t now = 0;
    time(&now);
    status->unix_time = (uint64_t)now;
    strncpy(status->ntp_server, s_ctx.ntp_server, sizeof(status->ntp_server) - 1);
    strncpy(status->timezone, s_ctx.timezone, sizeof(status->timezone) - 1);

    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (strftime(status->local_time,
                 sizeof(status->local_time),
                 "%Y-%m-%d %H:%M:%S %Z",
                 &tm_now) == 0) {
        strncpy(status->local_time, "unknown", sizeof(status->local_time) - 1);
    }
    return ESP_OK;
}
