#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <stdbool.h>
#include "led_strip.h"

#define LED_STRIP_GPIO 48 // Onboard RGB LED on ESP32-S3-DevKitC-1 v1.1

// External global variables
extern bool led_state;
extern led_strip_handle_t led_strip;

// Function prototypes
void configure_led(void);
void set_led_state(bool state);

#endif // LED_CONTROL_H
