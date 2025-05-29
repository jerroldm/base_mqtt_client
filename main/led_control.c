#include <esp_log.h>
#include "led_control.h"

static const char *TAG = "ESP32_Client";

// Global variables
bool led_state = false;
led_strip_handle_t led_strip;

void configure_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

void set_led_state(bool state)
{
    if (state) {
        ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, 0, 16, 0));
    } else {
        ESP_ERROR_CHECK(led_strip_clear(led_strip));
    }
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));
    ESP_LOGI(TAG, "RGB LED set to %s", state ? "on" : "off");
}
