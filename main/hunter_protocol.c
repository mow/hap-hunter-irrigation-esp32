#include "hunter_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "rem_gpio_driver.h"

/*
 * Protocol flow adapted from HunterRoam reference implementation:
 * https://github.com/ecodina/hunter-wifi
 */
static const char *TAG = "hunter_proto";

#define HUNTER_ZONE_MIN 1U
#define HUNTER_ZONE_MAX 48U
#define HUNTER_MINUTES_STOP 0U
#define HUNTER_MINUTES_MAX 240U

#define START_ZONE_FRAME_BYTES 15U

static uint8_t s_last_started_zone;

static esp_err_t send_low(void)
{
    return rem_gpio_send_pulse(HUNTER_SHORT_INTERVAL_US, HUNTER_LONG_INTERVAL_US);
}

static esp_err_t send_high(void)
{
    return rem_gpio_send_pulse(HUNTER_LONG_INTERVAL_US, HUNTER_SHORT_INTERVAL_US);
}

static void hunter_bitfield(uint8_t *bits, size_t bits_len, uint8_t pos, uint8_t val, uint8_t len)
{
    while (len > 0) {
        size_t byte_index = (size_t)pos / 8U;
        uint8_t bit_mask = (uint8_t)(0x80U >> (pos % 8U));

        if (byte_index < bits_len) {
            if ((val & 0x01U) != 0U) {
                bits[byte_index] |= bit_mask;
            } else {
                bits[byte_index] &= (uint8_t)(~bit_mask);
            }
        }

        len--;
        val >>= 1;
        pos++;
    }
}

static esp_err_t write_bus(const uint8_t *buffer, size_t len, bool extra_bit)
{
    esp_err_t err = rem_gpio_send_pulse(325000U, 65000U);
    if (err != ESP_OK) {
        return err;
    }

    err = rem_gpio_send_pulse(HUNTER_START_INTERVAL_US, HUNTER_SHORT_INTERVAL_US);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < len; ++i) {
        uint8_t send_byte = buffer[i];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            err = ((send_byte & 0x80U) != 0U) ? send_high() : send_low();
            if (err != ESP_OK) {
                rem_gpio_force_idle();
                return err;
            }
            send_byte <<= 1;
        }
    }

    if (extra_bit) {
        err = send_high();
        if (err != ESP_OK) {
            rem_gpio_force_idle();
            return err;
        }
    }

    err = send_low();
    if (err != ESP_OK) {
        rem_gpio_force_idle();
        return err;
    }

    return rem_gpio_force_idle();
}

static esp_err_t send_start_zone_frame(uint8_t zone, uint8_t time_minutes)
{
    uint8_t buffer[START_ZONE_FRAME_BYTES] = {
        0xff, 0x00, 0x00, 0x00, 0x10,
        0x00, 0x00, 0x04, 0x00, 0x00,
        0x01, 0x00, 0x01, 0xb8, 0x3f,
    };

    hunter_bitfield(buffer,
                    START_ZONE_FRAME_BYTES,
                    9,
                    (zone > 12U) ? 0x1U : 0x2U,
                    2);

    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 23, (uint8_t)(zone + 0x17U), 7);
    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 36, (uint8_t)(zone + 0x17U), 7);

    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 49, (uint8_t)(zone + 0x23U), 7);
    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 62, (uint8_t)(zone + 0x23U), 7);

    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 75, (uint8_t)(zone + 0x2fU), 7);
    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 88, (uint8_t)(zone + 0x2fU), 7);

    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 31, time_minutes, 4);
    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 44, (uint8_t)(time_minutes >> 4), 4);
    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 57, time_minutes, 4);
    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 70, (uint8_t)(time_minutes >> 4), 4);
    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 83, time_minutes, 4);
    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 96, (uint8_t)(time_minutes >> 4), 4);

    hunter_bitfield(buffer, START_ZONE_FRAME_BYTES, 109, (uint8_t)(zone - 1U), 4);

    return write_bus(buffer, START_ZONE_FRAME_BYTES, true);
}

esp_err_t hunter_protocol_init(void)
{
    s_last_started_zone = 0;
    return rem_gpio_force_idle();
}

esp_err_t hunterStartZone(uint8_t zone, uint16_t minutes)
{
    if (zone < HUNTER_ZONE_MIN || zone > HUNTER_ZONE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (minutes > HUNTER_MINUTES_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = send_start_zone_frame(zone, (uint8_t)minutes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to send HunterRoam START frame zone=%u minutes=%u: %s",
                 zone,
                 minutes,
                 esp_err_to_name(err));
        return err;
    }

    if (minutes > HUNTER_MINUTES_STOP) {
        s_last_started_zone = zone;
    } else if (s_last_started_zone == zone) {
        s_last_started_zone = 0;
    }

    ESP_LOGI(TAG, "Sent HunterRoam START frame zone=%u minutes=%u", zone, minutes);
    return ESP_OK;
}

esp_err_t hunterStopZone(uint8_t zone)
{
    if (zone < HUNTER_ZONE_MIN || zone > HUNTER_ZONE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = send_start_zone_frame(zone, HUNTER_MINUTES_STOP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to send HunterRoam STOP frame zone=%u: %s",
                 zone,
                 esp_err_to_name(err));
        return err;
    }

    if (s_last_started_zone == zone) {
        s_last_started_zone = 0;
    }

    ESP_LOGI(TAG, "Sent HunterRoam STOP frame zone=%u", zone);
    return ESP_OK;
}

esp_err_t hunterStopAll(void)
{
    /*
     * The Hunter SmartPort protocol has no broadcast stop frame; the reference
     * library only exposes per-zone stopZone(zone) == startZone(zone, 0).
     * Sweep every configurable zone with a zero-minute frame so the controller
     * is left in a known-stopped state regardless of prior runtime knowledge.
     */
    esp_err_t first_error = ESP_OK;

    for (uint8_t zone = HUNTER_ZONE_MIN; zone <= HUNTER_ZONE_COUNT_MAX; ++zone) {
        esp_err_t err = send_start_zone_frame(zone, HUNTER_MINUTES_STOP);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "Failed to send HunterRoam STOP frame zone=%u: %s",
                     zone,
                     esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }

    s_last_started_zone = 0;

    if (first_error != ESP_OK) {
        return first_error;
    }

    ESP_LOGI(TAG, "Sent HunterRoam stop sweep for zones 1..%u", HUNTER_ZONE_COUNT_MAX);
    return ESP_OK;
}
