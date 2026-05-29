#include "reset_button.h"

#include <stdbool.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "irrigation_runtime.h"
#include "settings_store.h"

static const char *TAG = "reset_button";

static runtime_state_t *s_state;
static esp_timer_handle_t s_poll_timer;
static int64_t s_press_start_us;
static bool s_reset_triggered;

static void reset_button_poll_cb(void *arg)
{
    (void)arg;

    if (HUNTER_RESET_BUTTON_GPIO < 0 || s_reset_triggered) {
        return;
    }

    int level = gpio_get_level((gpio_num_t)HUNTER_RESET_BUTTON_GPIO);
    bool pressed = (level == HUNTER_RESET_BUTTON_ACTIVE_LEVEL);

    if (!pressed) {
        s_press_start_us = 0;
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (s_press_start_us == 0) {
        s_press_start_us = now_us;
        return;
    }

    int64_t held_ms = (now_us - s_press_start_us) / 1000;
    if (held_ms < HUNTER_RESET_LONG_PRESS_MS) {
        return;
    }

    s_reset_triggered = true;
    ESP_LOGW(TAG, "Factory reset triggered by long press (%lld ms)", (long long)held_ms);

    if (s_state != NULL && s_state->active_zone > 0) {
        esp_err_t stop_err = irrigation_runtime_stop_all();
        if (stop_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop irrigation before reset: %s", esp_err_to_name(stop_err));
        }
    }

    esp_err_t clear_err = settings_store_clear();
    if (clear_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear settings: %s", esp_err_to_name(clear_err));
        s_reset_triggered = false;
        s_press_start_us = 0;
        return;
    }

    esp_restart();
}

esp_err_t reset_button_init(runtime_state_t *state)
{
    int reset_gpio = HUNTER_RESET_BUTTON_GPIO;

    s_state = state;
    s_press_start_us = 0;
    s_reset_triggered = false;

    if (reset_gpio < 0) {
        ESP_LOGI(TAG, "Reset button disabled (HUNTER_RESET_BUTTON_GPIO < 0)");
        return ESP_OK;
    }

    if (!GPIO_IS_VALID_GPIO(reset_gpio)) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)reset_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&cfg));

    const esp_timer_create_args_t timer_args = {
        .callback = reset_button_poll_cb,
        .name = "reset_button",
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_poll_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_poll_timer, 100000));

    ESP_LOGI(TAG,
             "Reset button initialized on GPIO%d, long-press=%dms",
             reset_gpio,
             HUNTER_RESET_LONG_PRESS_MS);
    return ESP_OK;
}
