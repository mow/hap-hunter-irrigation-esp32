#include "service_boundaries.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mbedtls/base64.h"

#include "app_config.h"
#include "homekit_irrigation.h"
#include "hunter_protocol.h"
#include "irrigation_runtime.h"
#include "scheduler.h"
#include "time_sync.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rem_gpio_driver.h"
#include "qrcodegen.h"

static const char *TAG = "services";
static httpd_handle_t s_http_server;
static hunter_settings_t s_settings_cache;
static runtime_state_t *s_runtime_state;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static bool s_wifi_stack_ready;
static bool s_wifi_started;
static bool s_wifi_handlers_registered;
static int s_sta_reconnect_attempts;
static hunter_schedule_t s_schedule_cache;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;

static bool parse_uint32(const char *value, uint32_t *out);
static bool parse_json_string_field(const char *json, const char *key, char *out, size_t out_size);

static void delayed_homekit_reset_task(void *unused)
{
    (void)unused;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = homekit_irrigation_reset_pairings();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HomeKit reset-pairings request ignored: %s", esp_err_to_name(err));
    }
}

static bool admin_auth_required(void)
{
    return s_settings_cache.admin_password[0] != '\0';
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Hunter\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Authentication required");
    return ESP_FAIL;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    if (!admin_auth_required()) {
        return ESP_OK;
    }

    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0 || hdr_len > 255) {
        return send_unauthorized(req);
    }

    char header[260];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return send_unauthorized(req);
    }

    if (strncasecmp(header, "Basic ", 6) != 0) {
        return send_unauthorized(req);
    }

    const char *b64 = header + 6;
    while (*b64 == ' ') {
        b64++;
    }

    unsigned char decoded[128];
    size_t out_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                              (const unsigned char *)b64, strlen(b64)) != 0) {
        return send_unauthorized(req);
    }
    decoded[out_len] = '\0';

    char *colon = strchr((char *)decoded, ':');
    if (colon == NULL) {
        return send_unauthorized(req);
    }
    *colon = '\0';
    const char *user = (const char *)decoded;
    const char *pass = colon + 1;

    if (strcmp(user, "admin") != 0 ||
        strcmp(pass, s_settings_cache.admin_password) != 0) {
        return send_unauthorized(req);
    }

    return ESP_OK;
}

static bool settings_differ(const hunter_settings_t *a, const hunter_settings_t *b)
{
    if (a == NULL || b == NULL) {
        return true;
    }

    return (a->wifi_configured != b->wifi_configured) ||
           (a->zone_count != b->zone_count) ||
           (a->default_runtime_seconds != b->default_runtime_seconds) ||
           (a->safety_cutoff_seconds != b->safety_cutoff_seconds) ||
           (strcmp(a->wifi_ssid, b->wifi_ssid) != 0) ||
           (strcmp(a->wifi_password, b->wifi_password) != 0) ||
           (strcmp(a->admin_password, b->admin_password) != 0);
}

static void config_change_reboot_task(void *unused)
{
    (void)unused;

    homekit_status_t hk_status;
    if (homekit_irrigation_get_status(&hk_status, NULL, 0) == ESP_OK && hk_status.initialized) {
        esp_err_t reset_err = homekit_irrigation_reset_pairings();
        if (reset_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Config changed; HomeKit pairing reset failed before reboot: %s",
                     esp_err_to_name(reset_err));
        } else {
            ESP_LOGI(TAG, "Config changed; HomeKit pairings reset before reboot");
        }
    } else {
        ESP_LOGI(TAG, "Config changed; HomeKit not initialized, skipping pairing reset");
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void html_escape_text(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;

    if (dst == NULL || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_size; ++si) {
        const char *replacement = NULL;

        switch (src[si]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default:
            dst[di++] = src[si];
            continue;
        }

        size_t replacement_len = strlen(replacement);
        if (di + replacement_len >= dst_size) {
            break;
        }
        memcpy(dst + di, replacement, replacement_len);
        di += replacement_len;
    }

    dst[di] = '\0';
}

static esp_err_t stream_homekit_qr_svg(httpd_req_t *req)
{
    homekit_status_t hk_status;
    char setup_payload[64];
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];

    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = homekit_irrigation_get_status(&hk_status, setup_payload, sizeof(setup_payload));
    if (err != ESP_OK || setup_payload[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "HomeKit QR unavailable");
        return ESP_FAIL;
    }

    if (!qrcodegen_encodeText(setup_payload,
                              temp,
                              qr,
                              qrcodegen_Ecc_LOW,
                              qrcodegen_VERSION_MIN,
                              5,
                              qrcodegen_Mask_AUTO,
                              true)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build HomeKit QR");
        return ESP_FAIL;
    }

    int size = qrcodegen_getSize(qr);
    httpd_resp_set_type(req, "image/svg+xml");
    char header[320];
    int header_len = snprintf(header,
                              sizeof(header),
                              "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 %d %d' role='img' aria-label='HomeKit QR code'"
                              " style='max-width:280px;width:100%%;height:auto;display:block;background:#fff;border-radius:16px;padding:12px;'>",
                              size + 8,
                              size + 8);
    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        return ESP_FAIL;
    }

    if (httpd_resp_sendstr_chunk(req, header) != ESP_OK) {
        return ESP_FAIL;
    }

    if (httpd_resp_sendstr_chunk(req, "<rect width='100%' height='100%' fill='#fff'/>") != ESP_OK) {
        return ESP_FAIL;
    }

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!qrcodegen_getModule(qr, x, y)) {
                continue;
            }

            char chunk[96];
            int written = snprintf(chunk,
                                   sizeof(chunk),
                                   "<rect x='%d' y='%d' width='1' height='1' fill='#111'/>",
                                   x + 4,
                                   y + 4);
            if (written <= 0 || written >= (int)sizeof(chunk)) {
                return ESP_FAIL;
            }
            if (httpd_resp_sendstr_chunk(req, chunk) != ESP_OK) {
                return ESP_FAIL;
            }
        }
    }

    if (httpd_resp_sendstr_chunk(req, "</svg>") != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(req, NULL) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t render_homekit_section(char *section, size_t section_size)
{
    homekit_status_t hk_status;
    char setup_payload[64];
    char setup_code[32];
    char setup_id[16];
    char status_label[128];
    char pairing_hint[160];
    char payload_label[96];

    esp_err_t err = homekit_irrigation_get_status(&hk_status, setup_payload, sizeof(setup_payload));
    if (err != ESP_OK) {
        return err;
    }

    html_escape_text(hk_status.setup_code, setup_code, sizeof(setup_code));
    html_escape_text(hk_status.setup_id, setup_id, sizeof(setup_id));
    html_escape_text(setup_payload[0] != '\0' ? setup_payload : "Not available yet", payload_label, sizeof(payload_label));

    if (!hk_status.initialized) {
        snprintf(status_label, sizeof(status_label), "Not initialized");
        snprintf(pairing_hint,
                 sizeof(pairing_hint),
                 "HomeKit service is unavailable in the current mode.");
    } else if (hk_status.paired_controller_count == 0) {
        snprintf(status_label, sizeof(status_label), "Ready for first pairing");
        snprintf(pairing_hint,
                 sizeof(pairing_hint),
                 "No controllers are paired yet. Add this accessory in Apple Home.");
    } else {
        snprintf(status_label,
                 sizeof(status_label),
                 "Paired (%d paired controllers)",
                 hk_status.paired_controller_count);
        snprintf(pairing_hint,
                 sizeof(pairing_hint),
                 "HomeKit allows multiple controllers for the same accessory. Use reset pairings to block new joins.");
    }

    int len = snprintf(section,
                       section_size,
                       "<section style='margin-top:1.5rem;padding:1rem;border:1px solid #ddd;border-radius:12px;'>"
                       "<h2>HomeKit Pairing</h2>"
                       "<p><strong>Status:</strong> %s</p>"
                       "<p><strong>Setup code:</strong> %s</p>"
                       "<p><strong>Setup ID:</strong> %s</p>"
                       "<p><strong>QR / barcode payload:</strong><br><code style='word-break:break-all;'>%s</code></p>"
                       "<div style='margin:1rem 0;'><img src='/api/v1/homekit/qr.svg' alt='HomeKit QR code' style='max-width:280px;width:100%%;height:auto;display:block;border:1px solid #ddd;border-radius:16px;background:#fff;padding:12px;'></div>"
                       "<p><strong>Paired controllers:</strong> %d</p>"
                       "<p><strong>Pairing behavior:</strong> %s</p>"
                       "<p><strong>Pre-pairing:</strong> scan the payload or enter the setup code in Apple Home.</p>"
                       "<p><strong>Post-pairing:</strong> this count updates after controllers are added or removed.</p>"
                       "<form method='post' action='/api/v1/homekit/reset-pairings' onsubmit=\"return confirm('Reset HomeKit pairings and reboot the device?');\">"
                       "<button type='submit'>Reset HomeKit Pairing</button>"
                       "</form></section>",
                       status_label,
                       setup_code,
                       setup_id,
                       payload_label,
                       hk_status.paired_controller_count,
                       pairing_hint);
    if (len <= 0 || len >= (int)section_size) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void schedule_minutes_to_csv(const hunter_schedule_t *schedule,
                                    uint8_t zone_count,
                                    char *out,
                                    size_t out_size)
{
    if (out == NULL || out_size == 0 || schedule == NULL) {
        return;
    }

    out[0] = '\0';
    size_t used = 0;
    uint8_t bounded_zone_count = zone_count;
    if (bounded_zone_count < HUNTER_ZONE_COUNT_MIN || bounded_zone_count > HUNTER_ZONE_COUNT_MAX) {
        bounded_zone_count = HUNTER_ZONE_COUNT_DEFAULT;
    }

    for (uint8_t i = 0; i < bounded_zone_count; ++i) {
        int written = snprintf(out + used,
                               out_size - used,
                               "%s%u",
                               i == 0 ? "" : ",",
                               (unsigned)schedule->zone_minutes[i]);
        if (written <= 0 || (size_t)written >= (out_size - used)) {
            break;
        }
        used += (size_t)written;
    }
}

static void parse_schedule_minutes_csv(const char *csv, hunter_schedule_t *schedule, uint8_t zone_count)
{
    if (csv == NULL || schedule == NULL) {
        return;
    }

    char buffer[192];
    strncpy(buffer, csv, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    uint8_t bounded_zone_count = zone_count;
    if (bounded_zone_count < HUNTER_ZONE_COUNT_MIN || bounded_zone_count > HUNTER_ZONE_COUNT_MAX) {
        bounded_zone_count = HUNTER_ZONE_COUNT_DEFAULT;
    }

    char *save_ptr = NULL;
    char *token = strtok_r(buffer, ",", &save_ptr);
    uint8_t index = 0;
    while (token != NULL && index < bounded_zone_count) {
        uint32_t value = 0;
        if (parse_uint32(token, &value)) {
            if (value > 240U) {
                value = 240U;
            }
            schedule->zone_minutes[index] = (uint16_t)value;
        }
        token = strtok_r(NULL, ",", &save_ptr);
        index++;
    }
}

static esp_err_t render_schedule_section(char *section, size_t section_size)
{
    hunter_schedule_t schedule;
    hunter_schedule_status_t status;
    char minutes_csv[128];
    char status_label[96];

    if (scheduler_get_config(&schedule) != ESP_OK ||
        scheduler_get_status(&status, s_settings_cache.zone_count) != ESP_OK) {
        return ESP_FAIL;
    }

    schedule_minutes_to_csv(&schedule, s_settings_cache.zone_count, minutes_csv, sizeof(minutes_csv));

    snprintf(status_label,
             sizeof(status_label),
             "%s%s",
             schedule.enabled ? "Enabled" : "Disabled",
             status.clock_valid ? "" : " (waiting for valid clock)");

    int len = snprintf(section,
                       section_size,
                       "<section style='margin-top:1.5rem;padding:1rem;border:1px solid #ddd;border-radius:12px;'>"
                       "<h2>Local Scheduler</h2>"
                       "<p><strong>Status:</strong> %s</p>"
                       "<p><strong>Run state:</strong> %s</p>"
                       "<p><strong>Days mask:</strong> %u (bit0=Sun ... bit6=Sat)</p>"
                       "<form method='post' action='/api/v1/schedule/form'>"
                       "<label><input type='checkbox' name='enabled' value='1' %s> Enabled</label>"
                       "<label>Start Hour (0-23)<input name='start_hour' type='number' min='0' max='23' value='%u' required></label>"
                       "<label>Start Minute (0-59)<input name='start_minute' type='number' min='0' max='59' value='%u' required></label>"
                       "<label>Days Mask (0-127)<input name='days_mask' type='number' min='0' max='127' value='%u' required></label>"
                       "<label>Zone Minutes CSV (zone1..zoneN)<input name='zone_minutes_csv' maxlength='120' value='%s' required></label>"
                       "<button type='submit'>Save Scheduler</button>"
                       "</form></section>",
                       status_label,
                       status.run_active ? "Running" : "Idle",
                       (unsigned)schedule.days_mask,
                       schedule.enabled ? "checked" : "",
                       (unsigned)schedule.start_hour,
                       (unsigned)schedule.start_minute,
                       (unsigned)schedule.days_mask,
                       minutes_csv);

    if (len <= 0 || len >= (int)section_size) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t render_time_section(char *section, size_t section_size)
{
    time_sync_status_t status;
    char status_label[96];

    if (time_sync_get_status(&status) != ESP_OK) {
        return ESP_FAIL;
    }

    snprintf(status_label,
             sizeof(status_label),
             "%s%s",
             status.clock_valid ? "Clock valid" : "Clock invalid",
             status.ntp_active ? " (NTP active)" : " (NTP idle)");

    int len = snprintf(section,
                       section_size,
                       "<section style='margin-top:1.5rem;padding:1rem;border:1px solid #ddd;border-radius:12px;'>"
                       "<h2>Time Sync</h2>"
                       "<p><strong>Status:</strong> %s</p>"
                       "<p><strong>Current Unix Time:</strong> %llu</p>"
                       "<p><strong>Current Local Time:</strong> %s</p>"
                       "<form method='post' action='/api/v1/time/form'>"
                       "<label>NTP Server<input name='ntp_server' maxlength='63' value='%s' required></label>"
                       "<label>Timezone (POSIX TZ)<input name='timezone' maxlength='63' value='%s' required></label>"
                       "<label>Manual Unix Epoch (optional)<input name='manual_epoch' type='number' min='1704067200' max='4102444800' value=''></label>"
                       "<button type='submit'>Save Time Settings</button>"
                       "</form></section>",
                       status_label,
                       (unsigned long long)status.unix_time,
                       status.local_time,
                       status.ntp_server,
                       status.timezone);
    if (len <= 0 || len >= (int)section_size) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const char *SETTINGS_PAGE_TEMPLATE =
    "<!doctype html><html><head><meta charset='utf-8'><title>Hunter Setup</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;max-width:520px;margin:2rem auto;padding:0 1rem;}"
    "label{display:block;margin-top:0.8rem;}input{width:100%;padding:0.5rem;}"
    "button{margin-top:1rem;padding:0.6rem 1rem;}small{color:#666;}</style></head><body>"
    "<h1>Hunter Irrigation Setup</h1>"
    "<p>Configure Wi-Fi and irrigation defaults. Device will reboot after save.</p>"
    "<form method='post' action='/api/settings'>"
    "<label>Wi-Fi SSID<input name='ssid' maxlength='32' value='%s' required></label>"
    "<label>Wi-Fi Password<input name='password' type='password' maxlength='64' value='%s'></label>"
    "<label>Admin Password (locks UI/API)<input name='admin_password' type='password' maxlength='32' placeholder='%s' autocomplete='new-password'></label>"
    "<label><input type='checkbox' name='admin_password_clear' value='1'> Remove admin password (open access)</label>"
    "<label>Zone Count<input name='zone_count' type='number' min='1' max='8' value='%u' required></label>"
    "<label>Default Runtime Seconds<input name='runtime_seconds' type='number' min='60' max='7200' value='%lu' required></label>"
    "<label>Safety Cutoff Seconds<input name='safety_cutoff_seconds' type='number' min='300' max='14400' value='%lu' required></label>"
    "<button type='submit'>Save</button></form>"
    "%s%s%s"
    "<small>If not redirected automatically, reconnect after reboot.</small></body></html>";

static bool parse_json_bool_field(const char *json, const char *key, bool *out)
{
    char pattern[48];
    const char *loc = NULL;

    if (json == NULL || key == NULL || out == NULL) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    loc = strstr(json, pattern);
    if (loc == NULL) {
        return false;
    }
    loc = strchr(loc, ':');
    if (loc == NULL) {
        return false;
    }
    loc++;

    while (*loc == ' ' || *loc == '\t') {
        loc++;
    }

    if (strncmp(loc, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(loc, "false", 5) == 0) {
        *out = false;
        return true;
    }
    if (*loc == '1') {
        *out = true;
        return true;
    }
    if (*loc == '0') {
        *out = false;
        return true;
    }

    return false;
}

static bool parse_json_string_field(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[48];
    const char *loc = NULL;
    const char *start = NULL;
    const char *end = NULL;
    size_t len = 0;

    if (json == NULL || key == NULL || out == NULL || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    loc = strstr(json, pattern);
    if (loc == NULL) {
        return false;
    }
    loc = strchr(loc, ':');
    if (loc == NULL) {
        return false;
    }
    loc++;

    while (*loc == ' ' || *loc == '\t') {
        loc++;
    }

    if (*loc != '"') {
        return false;
    }

    start = loc + 1;
    end = strchr(start, '"');
    if (end == NULL) {
        return false;
    }

    len = (size_t)(end - start);
    if (len == 0 || len >= out_size) {
        return false;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static esp_err_t decode_url_component(const char *src, char *dst, size_t dst_size)
{
    size_t si = 0;
    size_t di = 0;
    if (src == NULL || dst == NULL || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (src[si] != '\0' && di + 1 < dst_size) {
        if (src[si] == '+') {
            dst[di++] = ' ';
            si++;
        } else if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) &&
                   isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = {src[si + 1], src[si + 2], '\0'};
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 3;
        } else {
            dst[di++] = src[si++];
        }
    }

    dst[di] = '\0';
    return ESP_OK;
}

static bool parse_uint32(const char *value, uint32_t *out)
{
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (value == NULL || *value == '\0' || end == NULL || *end != '\0') {
        return false;
    }
    *out = (uint32_t)parsed;
    return true;
}

static bool parse_uint8(const char *value, uint8_t *out)
{
    uint32_t parsed = 0;
    if (!parse_uint32(value, &parsed) || parsed > 255U) {
        return false;
    }
    *out = (uint8_t)parsed;
    return true;
}

static bool parse_json_uint32_field(const char *json, const char *key, uint32_t *out)
{
    char pattern[48];
    const char *loc = NULL;
    char *end = NULL;
    unsigned long value = 0;

    if (json == NULL || key == NULL || out == NULL) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    loc = strstr(json, pattern);
    if (loc == NULL) {
        return false;
    }

    loc = strchr(loc, ':');
    if (loc == NULL) {
        return false;
    }
    loc++;

    while (*loc == ' ' || *loc == '\t') {
        loc++;
    }

    value = strtoul(loc, &end, 10);
    if (end == loc) {
        return false;
    }

    *out = (uint32_t)value;
    return true;
}

static esp_err_t parse_form_and_apply(const char *body, hunter_settings_t *settings)
{
    char working[512];
    char decoded_value[128];

    if (body == NULL || settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(working, body, sizeof(working) - 1);
    working[sizeof(working) - 1] = '\0';

    char *save_ptr = NULL;
    char *token = strtok_r(working, "&", &save_ptr);
    while (token != NULL) {
        char *eq = strchr(token, '=');
        if (eq != NULL) {
            *eq = '\0';
            const char *key = token;
            const char *value = eq + 1;
            decode_url_component(value, decoded_value, sizeof(decoded_value));

            if (strcmp(key, "ssid") == 0) {
                strncpy(settings->wifi_ssid, decoded_value, sizeof(settings->wifi_ssid) - 1);
                settings->wifi_ssid[sizeof(settings->wifi_ssid) - 1] = '\0';
            } else if (strcmp(key, "password") == 0) {
                strncpy(settings->wifi_password,
                        decoded_value,
                        sizeof(settings->wifi_password) - 1);
                settings->wifi_password[sizeof(settings->wifi_password) - 1] = '\0';
            } else if (strcmp(key, "admin_password") == 0) {
                /* Only overwrite when non-empty so the form can omit it to keep the current value. */
                if (decoded_value[0] != '\0') {
                    strncpy(settings->admin_password,
                            decoded_value,
                            sizeof(settings->admin_password) - 1);
                    settings->admin_password[sizeof(settings->admin_password) - 1] = '\0';
                }
            } else if (strcmp(key, "admin_password_clear") == 0) {
                if (strcmp(decoded_value, "1") == 0) {
                    settings->admin_password[0] = '\0';
                }
            } else if (strcmp(key, "zone_count") == 0) {
                uint8_t zone_count = 0;
                if (parse_uint8(decoded_value, &zone_count)) {
                    settings->zone_count = zone_count;
                }
            } else if (strcmp(key, "runtime_seconds") == 0) {
                uint32_t runtime = 0;
                if (parse_uint32(decoded_value, &runtime)) {
                    settings->default_runtime_seconds = runtime;
                }
            } else if (strcmp(key, "safety_cutoff_seconds") == 0) {
                uint32_t cutoff = 0;
                if (parse_uint32(decoded_value, &cutoff)) {
                    settings->safety_cutoff_seconds = cutoff;
                }
            }
        }
        token = strtok_r(NULL, "&", &save_ptr);
    }

    settings->wifi_configured = (settings->wifi_ssid[0] != '\0');
    return settings->wifi_configured ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t settings_page_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char *page = NULL;
    char *homekit_section = NULL;
    char *schedule_section = NULL;
    char *time_section = NULL;
    int len;

    page = calloc(1, 8192);
    homekit_section = calloc(1, 2048);
    schedule_section = calloc(1, 2048);
    time_section = calloc(1, 2048);
    if (page == NULL || homekit_section == NULL || schedule_section == NULL || time_section == NULL) {
        free(page);
        free(homekit_section);
        free(schedule_section);
        free(time_section);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    if (render_homekit_section(homekit_section, 2048) != ESP_OK) {
        snprintf(homekit_section,
                 2048,
                 "<section style='margin-top:1.5rem;padding:1rem;border:1px solid #ddd;border-radius:12px;'>"
                 "<h2>HomeKit Pairing</h2>"
                 "<p>HomeKit details are not available yet.</p></section>");
    }

    if (render_schedule_section(schedule_section, 2048) != ESP_OK) {
        snprintf(schedule_section,
                 2048,
                 "<section style='margin-top:1.5rem;padding:1rem;border:1px solid #ddd;border-radius:12px;'>"
                 "<h2>Local Scheduler</h2>"
                 "<p>Scheduler details are not available yet.</p></section>");
    }

    if (render_time_section(time_section, 2048) != ESP_OK) {
        snprintf(time_section,
                 2048,
                 "<section style='margin-top:1.5rem;padding:1rem;border:1px solid #ddd;border-radius:12px;'>"
                 "<h2>Time Sync</h2>"
                 "<p>Time details are not available yet.</p></section>");
    }

    len = snprintf(page,
                   8192,
                   SETTINGS_PAGE_TEMPLATE,
                   s_settings_cache.wifi_ssid,
                   s_settings_cache.wifi_password,
                   (s_settings_cache.admin_password[0] != '\0')
                       ? "leave blank to keep current password"
                       : "set a password to lock UI/API",
                   s_settings_cache.zone_count,
                   (unsigned long)s_settings_cache.default_runtime_seconds,
                   (unsigned long)s_settings_cache.safety_cutoff_seconds,
                   homekit_section,
                   schedule_section,
                   time_section);
    if (len <= 0 || len >= 8192) {
        free(page);
        free(homekit_section);
        free(schedule_section);
        free(time_section);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to render page");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    free(homekit_section);
    free(schedule_section);
    free(time_section);
    return err;
}

static esp_err_t api_schedule_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    hunter_schedule_t schedule;
    hunter_schedule_status_t status;
    char body[512];

    if (scheduler_get_config(&schedule) != ESP_OK ||
        scheduler_get_status(&status, s_settings_cache.zone_count) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scheduler status unavailable");
        return ESP_FAIL;
    }

    int len = snprintf(body,
                       sizeof(body),
                       "{\"enabled\":%s,\"start_hour\":%u,\"start_minute\":%u,\"days_mask\":%u,"
                       "\"clock_valid\":%s,\"run_active\":%s,\"current_zone\":%u,"
                       "\"zone_minutes\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
                       schedule.enabled ? "true" : "false",
                       (unsigned)schedule.start_hour,
                       (unsigned)schedule.start_minute,
                       (unsigned)schedule.days_mask,
                       status.clock_valid ? "true" : "false",
                       status.run_active ? "true" : "false",
                       (unsigned)status.current_zone,
                       (unsigned)schedule.zone_minutes[0],
                       (unsigned)schedule.zone_minutes[1],
                       (unsigned)schedule.zone_minutes[2],
                       (unsigned)schedule.zone_minutes[3],
                       (unsigned)schedule.zone_minutes[4],
                       (unsigned)schedule.zone_minutes[5],
                       (unsigned)schedule.zone_minutes[6],
                       (unsigned)schedule.zone_minutes[7]);
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to render scheduler status");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t apply_schedule_json_update(const char *body)
{
    hunter_schedule_t updated;
    uint32_t value = 0;
    bool enabled = false;

    if (scheduler_get_config(&updated) != ESP_OK) {
        return ESP_FAIL;
    }

    if (parse_json_bool_field(body, "enabled", &enabled)) {
        updated.enabled = enabled;
    }
    if (parse_json_uint32_field(body, "start_hour", &value)) {
        updated.start_hour = (uint8_t)value;
    }
    if (parse_json_uint32_field(body, "start_minute", &value)) {
        updated.start_minute = (uint8_t)value;
    }
    if (parse_json_uint32_field(body, "days_mask", &value)) {
        updated.days_mask = (uint8_t)value;
    }

    for (uint8_t i = 0; i < HUNTER_ZONE_COUNT_MAX; ++i) {
        char field[24];
        snprintf(field, sizeof(field), "zone_minutes_%u", (unsigned)(i + 1U));
        if (parse_json_uint32_field(body, field, &value)) {
            updated.zone_minutes[i] = (uint16_t)value;
        }
    }

    return scheduler_set_config(&updated, s_settings_cache.zone_count);
}

static esp_err_t api_schedule_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char body[512];
    int received = 0;
    int remaining = req->content_len;

    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large or empty");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int chunk = httpd_req_recv(req, body + received, remaining);
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        received += chunk;
        remaining -= chunk;
    }
    body[received] = '\0';

    esp_err_t err = apply_schedule_json_update(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid scheduler payload");
        return err;
    }

    scheduler_get_config(&s_schedule_cache);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"schedule_updated\"}");
    return ESP_OK;
}

static esp_err_t api_schedule_form_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char body[512];
    char working[512];
    char decoded_value[128];
    int received = 0;
    int remaining = req->content_len;
    hunter_schedule_t updated;

    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large or empty");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int chunk = httpd_req_recv(req, body + received, remaining);
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        received += chunk;
        remaining -= chunk;
    }
    body[received] = '\0';

    if (scheduler_get_config(&updated) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scheduler unavailable");
        return ESP_FAIL;
    }
    updated.enabled = false;

    strncpy(working, body, sizeof(working) - 1);
    working[sizeof(working) - 1] = '\0';

    char *save_ptr = NULL;
    char *token = strtok_r(working, "&", &save_ptr);
    while (token != NULL) {
        char *eq = strchr(token, '=');
        if (eq != NULL) {
            *eq = '\0';
            const char *key = token;
            const char *value = eq + 1;
            decode_url_component(value, decoded_value, sizeof(decoded_value));

            if (strcmp(key, "enabled") == 0) {
                updated.enabled = (strcmp(decoded_value, "1") == 0 || strcmp(decoded_value, "true") == 0);
            } else if (strcmp(key, "start_hour") == 0) {
                uint32_t parsed = 0;
                if (parse_uint32(decoded_value, &parsed)) {
                    updated.start_hour = (uint8_t)parsed;
                }
            } else if (strcmp(key, "start_minute") == 0) {
                uint32_t parsed = 0;
                if (parse_uint32(decoded_value, &parsed)) {
                    updated.start_minute = (uint8_t)parsed;
                }
            } else if (strcmp(key, "days_mask") == 0) {
                uint32_t parsed = 0;
                if (parse_uint32(decoded_value, &parsed)) {
                    updated.days_mask = (uint8_t)parsed;
                }
            } else if (strcmp(key, "zone_minutes_csv") == 0) {
                parse_schedule_minutes_csv(decoded_value, &updated, s_settings_cache.zone_count);
            }
        }
        token = strtok_r(NULL, "&", &save_ptr);
    }

    esp_err_t err = scheduler_set_config(&updated, s_settings_cache.zone_count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to save scheduler settings");
        return err;
    }

    scheduler_get_config(&s_schedule_cache);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t api_time_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    time_sync_status_t status;
    char body[512];

    if (time_sync_get_status(&status) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Time status unavailable");
        return ESP_FAIL;
    }

    int len = snprintf(body,
                       sizeof(body),
                       "{\"clock_valid\":%s,\"ntp_active\":%s,\"unix_time\":%llu,\"local_time\":\"%s\",\"ntp_server\":\"%s\",\"timezone\":\"%s\"}",
                       status.clock_valid ? "true" : "false",
                       status.ntp_active ? "true" : "false",
                       (unsigned long long)status.unix_time,
                       status.local_time,
                       status.ntp_server,
                       status.timezone);
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to render time status");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_time_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char body[512];
    int received = 0;
    int remaining = req->content_len;
    char ntp_server[64] = {0};
    char timezone[64] = {0};
    uint32_t epoch = 0;

    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large or empty");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int chunk = httpd_req_recv(req, body + received, remaining);
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        received += chunk;
        remaining -= chunk;
    }
    body[received] = '\0';

    if (parse_json_string_field(body, "ntp_server", ntp_server, sizeof(ntp_server))) {
        esp_err_t err = time_sync_set_ntp_server(ntp_server, true);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ntp_server");
            return err;
        }
    }

    if (parse_json_string_field(body, "timezone", timezone, sizeof(timezone))) {
        esp_err_t err = time_sync_set_timezone(timezone, true);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid timezone");
            return err;
        }
    }

    if (parse_json_uint32_field(body, "manual_epoch", &epoch)) {
        esp_err_t err = time_sync_set_manual_epoch(epoch);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid manual_epoch");
            return err;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"time_updated\"}");
    return ESP_OK;
}

static esp_err_t api_time_form_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char body[512];
    char working[512];
    char decoded_value[128];
    int received = 0;
    int remaining = req->content_len;
    bool ntp_seen = false;
    bool timezone_seen = false;

    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large or empty");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int chunk = httpd_req_recv(req, body + received, remaining);
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        received += chunk;
        remaining -= chunk;
    }
    body[received] = '\0';

    strncpy(working, body, sizeof(working) - 1);
    working[sizeof(working) - 1] = '\0';

    char *save_ptr = NULL;
    char *token = strtok_r(working, "&", &save_ptr);
    while (token != NULL) {
        char *eq = strchr(token, '=');
        if (eq != NULL) {
            *eq = '\0';
            const char *key = token;
            const char *value = eq + 1;
            decode_url_component(value, decoded_value, sizeof(decoded_value));

            if (strcmp(key, "ntp_server") == 0) {
                ntp_seen = true;
                esp_err_t err = time_sync_set_ntp_server(decoded_value, true);
                if (err != ESP_OK) {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid NTP server");
                    return err;
                }
            } else if (strcmp(key, "timezone") == 0) {
                timezone_seen = true;
                esp_err_t err = time_sync_set_timezone(decoded_value, true);
                if (err != ESP_OK) {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid timezone");
                    return err;
                }
            } else if (strcmp(key, "manual_epoch") == 0 && decoded_value[0] != '\0') {
                uint32_t epoch = 0;
                if (!parse_uint32(decoded_value, &epoch)) {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid manual epoch");
                    return ESP_ERR_INVALID_ARG;
                }

                esp_err_t err = time_sync_set_manual_epoch(epoch);
                if (err != ESP_OK) {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid manual epoch");
                    return err;
                }
            }
        }
        token = strtok_r(NULL, "&", &save_ptr);
    }

    if (!ntp_seen) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ntp_server is required");
        return ESP_ERR_INVALID_ARG;
    }

    if (!timezone_seen) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "timezone is required");
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t settings_save_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char body[512];
    int received = 0;
    int remaining = req->content_len;

    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large or empty");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int chunk = httpd_req_recv(req, body + received, remaining);
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        received += chunk;
        remaining -= chunk;
    }
    body[received] = '\0';

    hunter_settings_t updated = s_settings_cache;
    bool changed = false;
    esp_err_t err = parse_form_and_apply(body, &updated);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid settings payload");
        return err;
    }

    err = settings_store_save(&updated);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
        return err;
    }

    changed = settings_differ(&s_settings_cache, &updated);
    s_settings_cache = updated;
    if (s_runtime_state != NULL) {
        s_runtime_state->control_mode = updated.wifi_configured ? CONTROL_MODE_STATION : CONTROL_MODE_SETUP_AP;
    }

    if (scheduler_get_config(&s_schedule_cache) == ESP_OK) {
        scheduler_set_config(&s_schedule_cache, updated.zone_count);
    }

    httpd_resp_set_type(req, "application/json");
    if (changed) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"rebooting\":true,\"homekit_pairings_reset\":true}");
        xTaskCreate(config_change_reboot_task, "cfg_change_reboot", 3072, NULL, 5, NULL);
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"rebooting\":false,\"changed\":false}");
    }
    return ESP_OK;
}

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char body[512];
    hunter_schedule_status_t sched_status = {0};

    (void)scheduler_get_status(&sched_status, s_settings_cache.zone_count);

    if (s_runtime_state == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Runtime unavailable");
        return ESP_FAIL;
    }

    int len = snprintf(body,
                       sizeof(body),
                       "{\"mode\":\"%s\",\"network_connected\":%s,\"active_zone\":%u,"
                       "\"remaining_seconds\":%lu,\"elapsed_seconds\":%lu,"
                       "\"zone_count\":%u,\"default_runtime_seconds\":%lu,"
                       "\"safety_cutoff_seconds\":%lu,\"scheduler_enabled\":%s,\"scheduler_run_active\":%s}",
                       runtime_state_mode_name(s_runtime_state->control_mode),
                       s_runtime_state->network_connected ? "true" : "false",
                       s_runtime_state->active_zone,
                       (unsigned long)s_runtime_state->remaining_seconds,
                       (unsigned long)s_runtime_state->elapsed_seconds,
                       s_settings_cache.zone_count,
                       (unsigned long)s_settings_cache.default_runtime_seconds,
                       (unsigned long)s_settings_cache.safety_cutoff_seconds,
                       sched_status.enabled ? "true" : "false",
                       sched_status.run_active ? "true" : "false");
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to render status");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_start_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char body[256];
    int received = 0;
    int remaining = req->content_len;
    uint32_t zone = 0;
    uint32_t minutes = 0;
    uint32_t runtime_seconds = 0;

    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large or empty");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int chunk = httpd_req_recv(req, body + received, remaining);
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        received += chunk;
        remaining -= chunk;
    }
    body[received] = '\0';

    if (!parse_json_uint32_field(body, "zone", &zone)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing numeric field: zone");
        return ESP_ERR_INVALID_ARG;
    }

    if (!parse_json_uint32_field(body, "minutes", &minutes)) {
        if (parse_json_uint32_field(body, "runtime_seconds", &runtime_seconds)) {
            minutes = (runtime_seconds + 59U) / 60U;
        } else {
            minutes = (s_settings_cache.default_runtime_seconds + 59U) / 60U;
        }
    }

    if (zone < HUNTER_ZONE_COUNT_MIN || zone > s_settings_cache.zone_count) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Zone out of configured range");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = irrigation_runtime_start_zone((uint8_t)zone, (uint16_t)minutes);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start irrigation");
        return err;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"start\"}");
    return ESP_OK;
}

static esp_err_t api_stop_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    (void)req;
    esp_err_t err = irrigation_runtime_stop_all();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to stop irrigation");
        return err;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"stop\"}");
    return ESP_OK;
}

static esp_err_t api_settings_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char body[512];
    int received = 0;
    int remaining = req->content_len;
    hunter_settings_t updated = s_settings_cache;
    bool changed = false;
    uint32_t value = 0;

    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large or empty");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int chunk = httpd_req_recv(req, body + received, remaining);
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        received += chunk;
        remaining -= chunk;
    }
    body[received] = '\0';

    if (parse_json_uint32_field(body, "zone_count", &value)) {
        updated.zone_count = (uint8_t)value;
    }
    if (parse_json_uint32_field(body, "default_runtime_seconds", &value)) {
        updated.default_runtime_seconds = value;
    }
    if (parse_json_uint32_field(body, "safety_cutoff_seconds", &value)) {
        updated.safety_cutoff_seconds = value;
    }

    esp_err_t err = settings_store_save(&updated);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
        return err;
    }

    changed = settings_differ(&s_settings_cache, &updated);
    s_settings_cache = updated;
    if (s_runtime_state != NULL) {
        s_runtime_state->control_mode = updated.wifi_configured ? CONTROL_MODE_STATION : CONTROL_MODE_SETUP_AP;
    }

    if (scheduler_get_config(&s_schedule_cache) == ESP_OK) {
        scheduler_set_config(&s_schedule_cache, updated.zone_count);
    }

    httpd_resp_set_type(req, "application/json");
    if (changed) {
        httpd_resp_sendstr(req,
                          "{\"status\":\"ok\",\"action\":\"settings_updated\",\"rebooting\":true,\"homekit_pairings_reset\":true}");
        xTaskCreate(config_change_reboot_task, "cfg_change_reboot", 3072, NULL, 5, NULL);
    } else {
        httpd_resp_sendstr(req,
                          "{\"status\":\"ok\",\"action\":\"settings_updated\",\"rebooting\":false,\"changed\":false}");
    }
    return ESP_OK;
}

static esp_err_t api_homekit_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    homekit_status_t hk_status;
    char setup_payload[64];
    char body[256];
    esp_err_t err = homekit_irrigation_get_status(&hk_status, setup_payload, sizeof(setup_payload));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HomeKit status unavailable");
        return err;
    }

    int len = snprintf(body,
                       sizeof(body),
                       "{\"initialized\":%s,\"paired_controller_count\":%d,\"setup_code\":\"%s\","
                       "\"setup_id\":\"%s\",\"setup_payload\":\"%s\"}",
                       hk_status.initialized ? "true" : "false",
                       hk_status.paired_controller_count,
                       hk_status.setup_code,
                       hk_status.setup_id,
                       setup_payload);
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to render HomeKit status");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_homekit_qr_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    return stream_homekit_qr_svg(req);
}

static esp_err_t api_homekit_reset_pairings_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    homekit_status_t hk_status;

    (void)req;

    if (homekit_irrigation_get_status(&hk_status, NULL, 0) != ESP_OK || !hk_status.initialized) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HomeKit not initialized");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"reset_pairings\",\"rebooting\":true}");
    xTaskCreate(delayed_homekit_reset_task, "hk_reset_pairings", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 20;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t root_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = settings_page_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t save_post = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = settings_save_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_status_get = {
        .uri = "/api/v1/status",
        .method = HTTP_GET,
        .handler = api_status_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_start_post = {
        .uri = "/api/v1/start",
        .method = HTTP_POST,
        .handler = api_start_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_stop_post = {
        .uri = "/api/v1/stop",
        .method = HTTP_POST,
        .handler = api_stop_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_settings_post = {
        .uri = "/api/v1/settings",
        .method = HTTP_POST,
        .handler = api_settings_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_homekit_get = {
        .uri = "/api/v1/homekit",
        .method = HTTP_GET,
        .handler = api_homekit_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_homekit_qr_get = {
        .uri = "/api/v1/homekit/qr.svg",
        .method = HTTP_GET,
        .handler = api_homekit_qr_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_homekit_reset_pairings_post = {
        .uri = "/api/v1/homekit/reset-pairings",
        .method = HTTP_POST,
        .handler = api_homekit_reset_pairings_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_schedule_get = {
        .uri = "/api/v1/schedule",
        .method = HTTP_GET,
        .handler = api_schedule_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_schedule_post = {
        .uri = "/api/v1/schedule",
        .method = HTTP_POST,
        .handler = api_schedule_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_schedule_form_post = {
        .uri = "/api/v1/schedule/form",
        .method = HTTP_POST,
        .handler = api_schedule_form_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_time_get = {
        .uri = "/api/v1/time",
        .method = HTTP_GET,
        .handler = api_time_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_time_post = {
        .uri = "/api/v1/time",
        .method = HTTP_POST,
        .handler = api_time_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_time_form_post = {
        .uri = "/api/v1/time/form",
        .method = HTTP_POST,
        .handler = api_time_form_post_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &root_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &save_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_status_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_start_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_stop_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_settings_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_homekit_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_homekit_qr_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_homekit_reset_pairings_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_schedule_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_schedule_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_schedule_form_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_time_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_time_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &api_time_form_post));
    return ESP_OK;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_sta_reconnect_attempts = 0;
        if (s_runtime_state != NULL) {
            s_runtime_state->network_connected = false;
        }
        ESP_LOGI(TAG, "Wi-Fi station started, connecting to configured AP");
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_reconnect_attempts++;
        if (s_runtime_state != NULL) {
            s_runtime_state->network_connected = false;
        }
        ESP_LOGW(TAG,
                 "Wi-Fi disconnected, reconnect attempt=%d",
                 s_sta_reconnect_attempts);
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (event != NULL) {
            ESP_LOGI(TAG,
                     "Station connected, got IP: " IPSTR,
                     IP2STR(&event->ip_info.ip));
        } else {
            ESP_LOGI(TAG, "Station connected, got IP");
        }
        if (s_runtime_state != NULL) {
            s_runtime_state->network_connected = true;
        }
        s_sta_reconnect_attempts = 0;

        esp_err_t time_err = time_sync_on_network_connected();
        if (time_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start SNTP sync on connect: %s", esp_err_to_name(time_err));
        }
    }
}

static esp_err_t ensure_wifi_stack_initialized(bool need_ap, bool need_sta)
{
    if (!s_wifi_stack_ready) {
        ESP_ERROR_CHECK(esp_netif_init());

        esp_err_t loop_err = esp_event_loop_create_default();
        if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
            return loop_err;
        }

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_stack_ready = true;
    }

    if (need_ap && s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            return ESP_FAIL;
        }
    }

    if (need_sta && s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            return ESP_FAIL;
        }
    }

    if (need_sta && !s_wifi_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_wifi_event_instance));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &s_ip_event_instance));
        s_wifi_handlers_registered = true;
    }

    return ESP_OK;
}

static esp_err_t ensure_wifi_started(void)
{
    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    }
    return ESP_OK;
}

static esp_err_t start_softap_setup(void)
{
    ESP_ERROR_CHECK(ensure_wifi_stack_initialized(true, false));

    if (s_runtime_state != NULL) {
        s_runtime_state->network_connected = false;
    }

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, HUNTER_SETUP_AP_SSID, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(HUNTER_SETUP_AP_SSID);
    ap_config.ap.channel = HUNTER_SETUP_AP_CHANNEL;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(ensure_wifi_started());

    ESP_LOGI(TAG, "Setup AP started: SSID=%s IP=192.168.4.1", HUNTER_SETUP_AP_SSID);
    return ESP_OK;
}

static esp_err_t start_station_mode(const hunter_settings_t *settings)
{
    if (settings == NULL || settings->wifi_ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(ensure_wifi_stack_initialized(false, true));

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid,
            settings->wifi_ssid,
            sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password,
            settings->wifi_password,
            sizeof(sta_config.sta.password) - 1);
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(ensure_wifi_started());

    if (s_runtime_state != NULL) {
        s_runtime_state->network_connected = false;
    }

    ESP_LOGI(TAG,
             "Station mode started for SSID='%s'; waiting for IP and serving settings UI",
             settings->wifi_ssid);
    return ESP_OK;
}

esp_err_t provisioning_service_init(const hunter_settings_t *settings, runtime_state_t *state)
{
    if (settings == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_settings_cache = *settings;
    s_runtime_state = state;
    (void)scheduler_get_config(&s_schedule_cache);

    if (state->control_mode == CONTROL_MODE_SETUP_AP) {
        ESP_LOGI(TAG, "Provisioning mode: starting setup AP + web settings service");
        ESP_ERROR_CHECK(start_softap_setup());
        ESP_ERROR_CHECK(start_http_server());
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Configured mode: starting station mode + web settings service");
    ESP_ERROR_CHECK(start_station_mode(settings));
    ESP_ERROR_CHECK(start_http_server());
    return ESP_OK;
}

esp_err_t protocol_service_init(void)
{
    ESP_ERROR_CHECK(rem_gpio_init((gpio_num_t)HUNTER_PROTOCOL_GPIO));
    ESP_ERROR_CHECK(hunter_protocol_init());
    ESP_LOGI(TAG,
             "Hunter protocol boundary ready: REM GPIO driver initialized on GPIO%d",
             HUNTER_PROTOCOL_GPIO);
    return ESP_OK;
}

esp_err_t homekit_service_init(const hunter_settings_t *settings, runtime_state_t *state)
{
    esp_err_t err = homekit_irrigation_init(settings, state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HomeKit initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "HomeKit service boundary ready");
    return ESP_OK;
}

esp_err_t rest_api_service_init(const hunter_settings_t *settings, runtime_state_t *state)
{
    if (settings == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_settings_cache = *settings;
    s_runtime_state = state;
    (void)scheduler_get_config(&s_schedule_cache);

    ESP_ERROR_CHECK(start_http_server());
    ESP_LOGI(TAG,
             "REST API service boundary ready: /api/v1/status /api/v1/start /api/v1/stop /api/v1/settings /api/v1/schedule /api/v1/time");
    return ESP_OK;
}
