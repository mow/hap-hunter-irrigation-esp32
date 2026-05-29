#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef void (*rem_delay_us_fn_t)(uint32_t microseconds);

esp_err_t rem_gpio_init(gpio_num_t pin);
void rem_gpio_set_delay_provider(rem_delay_us_fn_t delay_fn);
esp_err_t rem_gpio_send_pulse(uint32_t high_us, uint32_t low_us);
esp_err_t rem_gpio_send_pulse_train(uint32_t high_us, uint32_t low_us, uint32_t count);
esp_err_t rem_gpio_force_idle(void);
