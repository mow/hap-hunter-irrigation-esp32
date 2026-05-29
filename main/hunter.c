#include "app_config.h"
#include "irrigation_runtime.h"
#include "reset_button.h"
#include "service_boundaries.h"
#include "settings_store.h"
#include "status_led.h"
#include "runtime_state.h"
#include "scheduler.h"
#include "time_sync.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "hunter";
static hunter_settings_t s_settings;
static runtime_state_t s_state;

void app_main(void)
{
	ESP_LOGI(TAG, "Hunter firmware booting: version=%s", HUNTER_FW_VERSION);

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	hunter_settings_t *settings = &s_settings;
	ESP_ERROR_CHECK(settings_store_load(settings));

	control_mode_t mode = settings->wifi_configured ? CONTROL_MODE_STATION : CONTROL_MODE_SETUP_AP;

	runtime_state_init(&s_state, mode);

	ESP_LOGI(TAG,
			 "Startup mode=%s, zones=%u, runtime_default_seconds=%lu, safety_cutoff_seconds=%lu, "
			 "protocol_gpio=%d",
			 runtime_state_mode_name(s_state.control_mode),
			 settings->zone_count,
			 (unsigned long)settings->default_runtime_seconds,
			 (unsigned long)settings->safety_cutoff_seconds,
			 HUNTER_PROTOCOL_GPIO);

	ESP_ERROR_CHECK(time_sync_init());
	ESP_ERROR_CHECK(provisioning_service_init(settings, &s_state));
	ESP_ERROR_CHECK(protocol_service_init());
	ESP_ERROR_CHECK(irrigation_runtime_init(&s_state, settings));

#if HUNTER_STOP_ZONES_ON_BOOT
	esp_err_t boot_stop_err = irrigation_runtime_stop_all();
	if (boot_stop_err == ESP_OK) {
		ESP_LOGI(TAG, "Boot safety stop-all executed");
	} else {
		ESP_LOGW(TAG, "Boot safety stop-all failed: %s", esp_err_to_name(boot_stop_err));
	}
#endif

	ESP_ERROR_CHECK(scheduler_init(settings, &s_state));
	ESP_ERROR_CHECK(status_led_init(&s_state));
	ESP_ERROR_CHECK(reset_button_init(&s_state));
	err = homekit_service_init(settings, &s_state);
	if (err != ESP_OK) {
		ESP_LOGW(TAG,
				 "HomeKit service unavailable, continuing without HomeKit: %s",
				 esp_err_to_name(err));
	}
	ESP_ERROR_CHECK(rest_api_service_init(settings, &s_state));

}
