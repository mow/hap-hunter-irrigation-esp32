#include "rem_gpio_driver.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "rem_gpio";

#define REM_PULSE_US_MIN 50U
#define REM_PULSE_US_MAX 2000000U
#define REM_PULSE_COUNT_MAX 2048U

static gpio_num_t s_pin = GPIO_NUM_NC;
static bool s_initialized;
static rem_delay_us_fn_t s_delay_fn = esp_rom_delay_us;

static bool pulse_duration_valid(uint32_t usec)
{
    return (usec >= REM_PULSE_US_MIN) && (usec <= REM_PULSE_US_MAX);
}

esp_err_t rem_gpio_init(gpio_num_t pin)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_pin = pin;
    s_initialized = true;
    return rem_gpio_force_idle();
}

void rem_gpio_set_delay_provider(rem_delay_us_fn_t delay_fn)
{
    s_delay_fn = (delay_fn != NULL) ? delay_fn : esp_rom_delay_us;
}

esp_err_t rem_gpio_force_idle(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = gpio_set_level(s_pin, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level idle failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t rem_gpio_send_pulse(uint32_t high_us, uint32_t low_us)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pulse_duration_valid(high_us) || !pulse_duration_valid(low_us)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = gpio_set_level(s_pin, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level high failed: %s", esp_err_to_name(err));
        return err;
    }
    s_delay_fn(high_us);

    err = gpio_set_level(s_pin, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level low failed: %s", esp_err_to_name(err));
        return err;
    }
    s_delay_fn(low_us);

    return ESP_OK;
}

esp_err_t rem_gpio_send_pulse_train(uint32_t high_us, uint32_t low_us, uint32_t count)
{
    if (count == 0 || count > REM_PULSE_COUNT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = rem_gpio_force_idle();
    if (err != ESP_OK) {
        return err;
    }

    for (uint32_t i = 0; i < count; ++i) {
        err = rem_gpio_send_pulse(high_us, low_us);
        if (err != ESP_OK) {
            rem_gpio_force_idle();
            return err;
        }
    }

    return rem_gpio_force_idle();
}
