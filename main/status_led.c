#include "status_led.h"

#include <stdbool.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "status_led";

static runtime_state_t *s_state;
static esp_timer_handle_t s_led_timer;
static bool s_blink_phase;

static void led_tick_callback(void *arg)
{
    (void)arg;

    if (s_state == NULL || HUNTER_STATUS_LED_GPIO < 0) {
        return;
    }

    int level = 0;

    if (!s_state->network_connected) {
        s_blink_phase = !s_blink_phase;
        level = s_blink_phase ? 1 : 0;
    } else if (s_state->active_zone > 0) {
        level = 1;
    } else {
        level = 0;
    }

    gpio_set_level((gpio_num_t)HUNTER_STATUS_LED_GPIO, level);
}

esp_err_t status_led_init(runtime_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_state = state;
    s_blink_phase = false;

    if (HUNTER_STATUS_LED_GPIO < 0) {
        ESP_LOGI(TAG, "Status LED disabled (HUNTER_STATUS_LED_GPIO < 0)");
        return ESP_OK;
    }

    if (!GPIO_IS_VALID_OUTPUT_GPIO(HUNTER_STATUS_LED_GPIO)) {
        return ESP_ERR_INVALID_ARG;
    }

    int led_pin = HUNTER_STATUS_LED_GPIO;
    uint64_t led_pin_mask = (1ULL << (uint32_t)led_pin);

    gpio_config_t cfg = {
        .pin_bit_mask = led_pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)HUNTER_STATUS_LED_GPIO, 0));

    const esp_timer_create_args_t timer_args = {
        .callback = led_tick_callback,
        .name = "status_led",
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_led_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_led_timer, 300000));

    ESP_LOGI(TAG, "Status LED initialized on GPIO%d", HUNTER_STATUS_LED_GPIO);
    return ESP_OK;
}
