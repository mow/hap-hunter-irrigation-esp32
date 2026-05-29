#include "homekit_irrigation.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "hap.h"
#include "hap_apple_chars.h"
#include "hap_apple_servs.h"
#include "irrigation_runtime.h"

static const char *TAG = "hk_irrigation";

#define HUNTER_HAP_SETUP_ID "HNTR"
#define HUNTER_HAP_MAX_ZONES HUNTER_ZONE_COUNT_MAX
#define HUNTER_HAP_VALVE_TYPE_IRRIGATION 1U
#define HUNTER_HAP_PROGRAM_MODE_NOT_SCHEDULED 0U
#define HUNTER_HAP_PROGRAM_MODE_SCHEDULED 1U
#define HUNTER_HAP_ACTIVE_INACTIVE 0U
#define HUNTER_HAP_ACTIVE_ACTIVE 1U
#define HUNTER_HAP_IN_USE_NOT_IN_USE 0U
#define HUNTER_HAP_IN_USE_IN_USE 1U
#define HUNTER_HAP_SETUP_CODE_LEN 11U

static const char *HUNTER_HAP_FALLBACK_SETUP_CODE = "493-68-103";
static bool s_schedule_enabled_pending;

typedef struct {
    uint8_t zone_id;
    hap_serv_t *service;
    hap_char_t *active_char;
    hap_char_t *in_use_char;
    hap_char_t *set_duration_char;
    hap_char_t *remaining_duration_char;
} homekit_zone_ctx_t;

typedef struct {
    bool initialized;
    runtime_state_t *state;
    uint8_t zone_count;
    bool schedule_enabled;
    int paired_controller_count;
    char setup_code[HUNTER_HAP_SETUP_CODE_LEN];
    char setup_id[sizeof(HUNTER_HAP_SETUP_ID)];
    hap_serv_t *irrigation_service;
    hap_char_t *irrigation_active_char;
    hap_char_t *irrigation_in_use_char;
    hap_char_t *irrigation_program_mode_char;
    homekit_zone_ctx_t zones[HUNTER_HAP_MAX_ZONES];
    esp_timer_handle_t sync_timer;
} homekit_ctx_t;

static homekit_ctx_t s_ctx;

static bool is_weak_setup_pin(uint32_t pin)
{
    return pin == 11111111U || pin == 12345678U || pin == 87654321U || pin == 55555555U;
}

static uint32_t hash_mac_for_setup_code(const uint8_t mac[6])
{
    uint32_t hash = 2166136261U;
    static const uint8_t salt[] = {0x48, 0x55, 0x4E, 0x54, 0x45, 0x52, 0x2D, 0x48, 0x41, 0x50};

    for (size_t i = 0; i < 6; ++i) {
        hash ^= mac[i];
        hash *= 16777619U;
    }
    for (size_t i = 0; i < sizeof(salt); ++i) {
        hash ^= salt[i];
        hash *= 16777619U;
    }

    return hash;
}

static esp_err_t ensure_setup_credentials(void)
{
    if (s_ctx.setup_code[0] != '\0' && s_ctx.setup_id[0] != '\0') {
        return ESP_OK;
    }

    memcpy(s_ctx.setup_id, HUNTER_HAP_SETUP_ID, sizeof(HUNTER_HAP_SETUP_ID));

    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed reading STA MAC for HomeKit setup code; using fallback code: %s",
                 esp_err_to_name(err));
        memcpy(s_ctx.setup_code, HUNTER_HAP_FALLBACK_SETUP_CODE, HUNTER_HAP_SETUP_CODE_LEN);
        return err;
    }

    uint32_t pin = 0;
    uint32_t hash = hash_mac_for_setup_code(mac);
    for (int i = 0; i < 4; ++i) {
        pin = (hash % 90000000U) + 10000000U;
        if (!is_weak_setup_pin(pin)) {
            break;
        }
        hash = (hash * 1664525U) + 1013904223U;
    }

    uint8_t digits[8];
    uint32_t tmp = pin;
    for (int i = 7; i >= 0; --i) {
        digits[i] = (uint8_t)(tmp % 10U);
        tmp /= 10U;
    }

    s_ctx.setup_code[0] = (char)('0' + digits[0]);
    s_ctx.setup_code[1] = (char)('0' + digits[1]);
    s_ctx.setup_code[2] = (char)('0' + digits[2]);
    s_ctx.setup_code[3] = '-';
    s_ctx.setup_code[4] = (char)('0' + digits[3]);
    s_ctx.setup_code[5] = (char)('0' + digits[4]);
    s_ctx.setup_code[6] = '-';
    s_ctx.setup_code[7] = (char)('0' + digits[5]);
    s_ctx.setup_code[8] = (char)('0' + digits[6]);
    s_ctx.setup_code[9] = (char)('0' + digits[7]);
    s_ctx.setup_code[10] = '\0';

    ESP_LOGI(TAG,
             "HomeKit setup code derived from MAC ending %02X:%02X",
             mac[4],
             mac[5]);
    return ESP_OK;
}

static void homekit_hap_event_handler(hap_event_t event, void *data)
{
    switch (event) {
    case HAP_EVENT_CTRL_PAIRED:
    case HAP_EVENT_CTRL_UNPAIRED:
        s_ctx.paired_controller_count = hap_get_paired_controller_count();
        ESP_LOGI(TAG,
                 "HomeKit controller event: %s (%s), paired controllers=%d",
                 event == HAP_EVENT_CTRL_PAIRED ? "paired" : "unpaired",
                 data != NULL ? (char *)data : "unknown",
                 s_ctx.paired_controller_count);
        break;
    case HAP_EVENT_PAIRING_STARTED:
        ESP_LOGI(TAG, "HomeKit pairing started");
        break;
    case HAP_EVENT_PAIRING_ABORTED:
        ESP_LOGI(TAG, "HomeKit pairing aborted");
        break;
    case HAP_EVENT_ACC_REBOOTING:
        ESP_LOGI(TAG, "HomeKit accessory rebooting for %s", data != NULL ? (char *)data : "unknown");
        break;
    default:
        break;
    }
}

static int homekit_identify(hap_acc_t *acc)
{
    (void)acc;
    ESP_LOGI(TAG, "HomeKit identify requested");
    return HAP_SUCCESS;
}

static uint16_t round_up_seconds_to_minutes(uint32_t seconds)
{
    uint32_t rounded = (seconds + 59U) / 60U;
    if (rounded == 0U) {
        rounded = 1U;
    }
    if (rounded > 240U) {
        rounded = 240U;
    }
    return (uint16_t)rounded;
}

static void update_char_u8(hap_char_t *hc, uint8_t value)
{
    if (hc == NULL) {
        return;
    }

    const hap_val_t *current = hap_char_get_val(hc);
    if (current != NULL && current->u == value) {
        return;
    }

    hap_val_t val = {.u = value};
    hap_char_update_val(hc, &val);
}

static void update_char_u32(hap_char_t *hc, uint32_t value)
{
    if (hc == NULL) {
        return;
    }

    const hap_val_t *current = hap_char_get_val(hc);
    if (current != NULL && current->u == value) {
        return;
    }

    hap_val_t val = {.u = value};
    hap_char_update_val(hc, &val);
}

static void sync_homekit_state(void)
{
    if (!s_ctx.initialized || s_ctx.state == NULL) {
        return;
    }

    bool any_active = (s_ctx.state->active_zone > 0U);

    update_char_u8(s_ctx.irrigation_active_char,
                   any_active ? HUNTER_HAP_ACTIVE_ACTIVE : HUNTER_HAP_ACTIVE_INACTIVE);
    update_char_u8(s_ctx.irrigation_in_use_char,
                   any_active ? HUNTER_HAP_IN_USE_IN_USE : HUNTER_HAP_IN_USE_NOT_IN_USE);
    update_char_u8(s_ctx.irrigation_program_mode_char,
                   s_ctx.schedule_enabled ? HUNTER_HAP_PROGRAM_MODE_SCHEDULED
                                          : HUNTER_HAP_PROGRAM_MODE_NOT_SCHEDULED);

    for (uint8_t i = 0; i < s_ctx.zone_count; ++i) {
        bool zone_active = (s_ctx.state->active_zone == s_ctx.zones[i].zone_id);
        update_char_u8(s_ctx.zones[i].active_char,
                       zone_active ? HUNTER_HAP_ACTIVE_ACTIVE : HUNTER_HAP_ACTIVE_INACTIVE);
        update_char_u8(s_ctx.zones[i].in_use_char,
                       zone_active ? HUNTER_HAP_IN_USE_IN_USE : HUNTER_HAP_IN_USE_NOT_IN_USE);
        update_char_u32(s_ctx.zones[i].remaining_duration_char,
                        zone_active ? s_ctx.state->remaining_seconds : 0U);
    }
}

static void homekit_sync_timer_cb(void *arg)
{
    (void)arg;
    sync_homekit_state();
}

static int zone_service_write(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv)
{
    (void)write_priv;

    if (serv_priv == NULL || write_data == NULL || count <= 0) {
        return HAP_FAIL;
    }

    homekit_zone_ctx_t *zone_ctx = (homekit_zone_ctx_t *)serv_priv;
    int ret = HAP_SUCCESS;

    for (int i = 0; i < count; ++i) {
        hap_write_data_t *write = &write_data[i];
        const char *uuid = hap_char_get_type_uuid(write->hc);
        *(write->status) = HAP_STATUS_VAL_INVALID;

        if (uuid == NULL) {
            ret = HAP_FAIL;
            continue;
        }

        if (strcmp(uuid, HAP_CHAR_UUID_SET_DURATION) == 0) {
            hap_char_update_val(write->hc, &write->val);
            *(write->status) = HAP_STATUS_SUCCESS;
            continue;
        }

        if (strcmp(uuid, HAP_CHAR_UUID_ACTIVE) == 0) {
            bool turn_on = (write->val.u == HUNTER_HAP_ACTIVE_ACTIVE);
            if (turn_on) {
                const hap_val_t *set_duration = hap_char_get_val(zone_ctx->set_duration_char);
                uint32_t requested_seconds = (set_duration != NULL) ? set_duration->u : HUNTER_RUNTIME_SECONDS_DEFAULT;
                uint16_t minutes = round_up_seconds_to_minutes(requested_seconds);

                esp_err_t start_err = irrigation_runtime_start_zone(zone_ctx->zone_id, minutes);
                if (start_err == ESP_OK) {
                    hap_char_update_val(write->hc, &write->val);
                    *(write->status) = HAP_STATUS_SUCCESS;
                    ESP_LOGI(TAG,
                             "HomeKit start request accepted: zone=%u minutes=%u",
                             zone_ctx->zone_id,
                             minutes);
                } else {
                    ESP_LOGE(TAG,
                             "HomeKit start request failed: zone=%u err=%s",
                             zone_ctx->zone_id,
                             esp_err_to_name(start_err));
                    *(write->status) = HAP_STATUS_COMM_ERR;
                    ret = HAP_FAIL;
                }
            } else {
                if (s_ctx.state != NULL && s_ctx.state->active_zone == zone_ctx->zone_id) {
                    esp_err_t stop_err = irrigation_runtime_stop_all();
                    if (stop_err != ESP_OK) {
                        ESP_LOGE(TAG,
                                 "HomeKit stop request failed: zone=%u err=%s",
                                 zone_ctx->zone_id,
                                 esp_err_to_name(stop_err));
                        *(write->status) = HAP_STATUS_COMM_ERR;
                        ret = HAP_FAIL;
                        continue;
                    }
                }

                hap_char_update_val(write->hc, &write->val);
                *(write->status) = HAP_STATUS_SUCCESS;
                ESP_LOGI(TAG, "HomeKit stop request accepted: zone=%u", zone_ctx->zone_id);
            }
            continue;
        }

        *(write->status) = HAP_STATUS_RES_ABSENT;
        ret = HAP_FAIL;
    }

    sync_homekit_state();
    return ret;
}

static esp_err_t create_zone_services(hap_acc_t *accessory, const hunter_settings_t *settings)
{
    if (accessory == NULL || settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < settings->zone_count; ++i) {
        uint8_t zone_id = (uint8_t)(i + 1U);
        homekit_zone_ctx_t *zone = &s_ctx.zones[i];

        zone->zone_id = zone_id;
        zone->service = hap_serv_valve_create(HUNTER_HAP_ACTIVE_INACTIVE,
                                              HUNTER_HAP_IN_USE_NOT_IN_USE,
                                              HUNTER_HAP_VALVE_TYPE_IRRIGATION);
        if (zone->service == NULL) {
            ESP_LOGE(TAG, "Failed creating valve service for zone=%u", zone_id);
            return ESP_FAIL;
        }

        char *name = (char *)malloc(16);
        if (name == NULL) {
            ESP_LOGE(TAG, "Failed allocating zone name buffer for zone=%u", zone_id);
            return ESP_ERR_NO_MEM;
        }
        snprintf(name, 16, "Zone %u", zone_id);

        int add_ret = hap_serv_add_char(zone->service, hap_char_name_create(name));
        if (add_ret != HAP_SUCCESS) {
            ESP_LOGE(TAG, "Failed adding name characteristic for zone=%u", zone_id);
            return ESP_FAIL;
        }

        if (hap_serv_add_char(zone->service, hap_char_service_label_index_create(zone_id)) != HAP_SUCCESS) {
            ESP_LOGW(TAG, "Failed adding Service Label Index for zone=%u", zone_id);
        }

        zone->active_char = hap_serv_get_char_by_uuid(zone->service, HAP_CHAR_UUID_ACTIVE);
        zone->in_use_char = hap_serv_get_char_by_uuid(zone->service, HAP_CHAR_UUID_IN_USE);
        zone->set_duration_char = hap_serv_get_char_by_uuid(zone->service, HAP_CHAR_UUID_SET_DURATION);
        zone->remaining_duration_char = hap_serv_get_char_by_uuid(zone->service, HAP_CHAR_UUID_REMAINING_DURATION);

        if (zone->active_char == NULL || zone->in_use_char == NULL) {
            ESP_LOGE(TAG, "Missing mandatory valve characteristics (Active/InUse) for zone=%u", zone_id);
            return ESP_FAIL;
        }

        if (zone->set_duration_char == NULL) {
            if (hap_serv_add_char(zone->service,
                                  hap_char_set_duration_create(settings->default_runtime_seconds)) != HAP_SUCCESS) {
                ESP_LOGE(TAG, "Failed adding Set Duration characteristic for zone=%u", zone_id);
                return ESP_FAIL;
            }
            zone->set_duration_char = hap_serv_get_char_by_uuid(zone->service, HAP_CHAR_UUID_SET_DURATION);
        }

        if (zone->remaining_duration_char == NULL) {
            if (hap_serv_add_char(zone->service, hap_char_remaining_duration_create(0U)) != HAP_SUCCESS) {
                ESP_LOGE(TAG, "Failed adding Remaining Duration characteristic for zone=%u", zone_id);
                return ESP_FAIL;
            }
            zone->remaining_duration_char =
                hap_serv_get_char_by_uuid(zone->service, HAP_CHAR_UUID_REMAINING_DURATION);
        }

        if (zone->set_duration_char == NULL || zone->remaining_duration_char == NULL) {
            ESP_LOGE(TAG, "Duration characteristics unavailable after add for zone=%u", zone_id);
            return ESP_FAIL;
        }

        update_char_u32(zone->set_duration_char, settings->default_runtime_seconds);
        update_char_u32(zone->remaining_duration_char, 0U);

        hap_serv_set_priv(zone->service, zone);
        hap_serv_set_write_cb(zone->service, zone_service_write);
        hap_acc_add_serv(accessory, zone->service);

        if (s_ctx.irrigation_service != NULL) {
            if (hap_serv_link_serv(s_ctx.irrigation_service, zone->service) != HAP_SUCCESS) {
                ESP_LOGW(TAG, "Failed linking irrigation service to zone=%u valve service", zone_id);
            }
        }

        ESP_LOGI(TAG, "HomeKit zone service created for zone=%u", zone_id);
    }

    return ESP_OK;
}

esp_err_t homekit_irrigation_init(const hunter_settings_t *settings, runtime_state_t *state)
{
    if (settings == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (settings->zone_count < HUNTER_ZONE_COUNT_MIN || settings->zone_count > HUNTER_ZONE_COUNT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (state->control_mode != CONTROL_MODE_STATION) {
        ESP_LOGI(TAG, "Skipping HomeKit init: control mode is setup AP");
        return ESP_OK;
    }

    if (s_ctx.initialized) {
        ESP_LOGI(TAG, "HomeKit already initialized");
        return ESP_OK;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state = state;
    s_ctx.zone_count = settings->zone_count;
    s_ctx.schedule_enabled = s_schedule_enabled_pending;

    (void)ensure_setup_credentials();

    hap_set_setup_code(s_ctx.setup_code[0] != '\0' ? s_ctx.setup_code : HUNTER_HAP_FALLBACK_SETUP_CODE);
    if (hap_set_setup_id(s_ctx.setup_id[0] != '\0' ? s_ctx.setup_id : HUNTER_HAP_SETUP_ID) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "Failed setting HomeKit setup ID");
        return ESP_FAIL;
    }

    if (hap_init(HAP_TRANSPORT_WIFI) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "hap_init failed");
        return ESP_FAIL;
    }

    hap_register_event_handler(homekit_hap_event_handler);

    hap_enable_mfi_auth(HAP_MFI_AUTH_NONE);

    hap_acc_cfg_t cfg = {
        .name = "Hunter X-Core",
        .manufacturer = "Hunter",
        .model = "X-Core REM",
        .serial_num = "HUNTER-ESP32C3-001",
        .fw_rev = HUNTER_FW_VERSION,
        .hw_rev = "1.0",
        .pv = "1.1",
        .identify_routine = homekit_identify,
        .cid = HAP_CID_SPRINKLER,
    };

    hap_acc_t *accessory = hap_acc_create(&cfg);
    if (accessory == NULL) {
        ESP_LOGE(TAG, "Failed creating HomeKit accessory");
        return ESP_FAIL;
    }

    if (hap_acc_add_wifi_transport_service(accessory, 0) != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed adding HomeKit Wi-Fi transport service; continuing HomeKit init without transport service");
    }

    s_ctx.irrigation_service = hap_serv_irrigation_system_create(HUNTER_HAP_ACTIVE_INACTIVE,
                                                                  HUNTER_HAP_PROGRAM_MODE_NOT_SCHEDULED,
                                                                  HUNTER_HAP_IN_USE_NOT_IN_USE);
    if (s_ctx.irrigation_service == NULL) {
        ESP_LOGE(TAG, "Failed creating irrigation system service");
        return ESP_FAIL;
    }

    if (hap_serv_add_char(s_ctx.irrigation_service, hap_char_name_create("Irrigation System")) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "Failed adding irrigation system name characteristic");
        return ESP_FAIL;
    }

    s_ctx.irrigation_active_char = hap_serv_get_char_by_uuid(s_ctx.irrigation_service, HAP_CHAR_UUID_ACTIVE);
    s_ctx.irrigation_program_mode_char =
        hap_serv_get_char_by_uuid(s_ctx.irrigation_service, HAP_CHAR_UUID_PROGRAM_MODE);
    s_ctx.irrigation_in_use_char = hap_serv_get_char_by_uuid(s_ctx.irrigation_service, HAP_CHAR_UUID_IN_USE);

    if (s_ctx.irrigation_active_char == NULL || s_ctx.irrigation_program_mode_char == NULL ||
        s_ctx.irrigation_in_use_char == NULL) {
        ESP_LOGE(TAG, "Missing required irrigation system characteristics");
        return ESP_FAIL;
    }

    hap_acc_add_serv(accessory, s_ctx.irrigation_service);

    esp_err_t zone_err = create_zone_services(accessory, settings);
    if (zone_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed creating HomeKit zone services (zone_count=%u): %s",
                 settings->zone_count,
                 esp_err_to_name(zone_err));
        return zone_err;
    }

    hap_add_accessory(accessory);

    if (hap_start() != HAP_SUCCESS) {
        ESP_LOGE(TAG, "hap_start failed");
        return ESP_FAIL;
    }

    const esp_timer_create_args_t sync_timer_args = {
        .callback = homekit_sync_timer_cb,
        .name = "hk_sync",
    };

    ESP_ERROR_CHECK(esp_timer_create(&sync_timer_args, &s_ctx.sync_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_ctx.sync_timer, 1000000));

    s_ctx.initialized = true;
    s_ctx.paired_controller_count = hap_get_paired_controller_count();
    sync_homekit_state();

    char *setup_payload = esp_hap_get_setup_payload(s_ctx.setup_code,
                                                    s_ctx.setup_id,
                                                    false,
                                                    HAP_CID_SPRINKLER);
    if (setup_payload != NULL) {
        ESP_LOGI(TAG, "HomeKit setup payload: %s", setup_payload);
        free(setup_payload);
    }

    ESP_LOGI(TAG,
             "HomeKit irrigation initialized: zones=%u setup_code=%s setup_id=%s",
             settings->zone_count,
             s_ctx.setup_code,
             s_ctx.setup_id);
    return ESP_OK;
}

esp_err_t homekit_irrigation_get_status(homekit_status_t *status, char *setup_payload, size_t setup_payload_size)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    (void)ensure_setup_credentials();
    status->setup_code = (s_ctx.setup_code[0] != '\0') ? s_ctx.setup_code : HUNTER_HAP_FALLBACK_SETUP_CODE;
    status->setup_id = (s_ctx.setup_id[0] != '\0') ? s_ctx.setup_id : HUNTER_HAP_SETUP_ID;
    status->initialized = s_ctx.initialized;
    status->paired_controller_count = s_ctx.initialized ? hap_get_paired_controller_count() : 0;
    s_ctx.paired_controller_count = status->paired_controller_count;

    if (setup_payload != NULL && setup_payload_size > 0) {
        setup_payload[0] = '\0';

        char *generated = esp_hap_get_setup_payload((char *)status->setup_code,
                                                    (char *)status->setup_id,
                                                    false,
                                                    HAP_CID_SPRINKLER);
        if (generated != NULL) {
            strncpy(setup_payload, generated, setup_payload_size - 1);
            setup_payload[setup_payload_size - 1] = '\0';
            free(generated);
        }
    }

    return ESP_OK;
}

esp_err_t homekit_irrigation_reset_pairings(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = hap_reset_pairings();
    return (ret == HAP_SUCCESS) ? ESP_OK : ESP_FAIL;
}

esp_err_t homekit_irrigation_set_schedule_enabled(bool enabled)
{
    s_schedule_enabled_pending = enabled;
    s_ctx.schedule_enabled = enabled;
    if (s_ctx.initialized) {
        sync_homekit_state();
    }
    return ESP_OK;
}
