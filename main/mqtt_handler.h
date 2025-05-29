#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <mqtt_client.h>
#include "config.h"

// Event group bits
#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT1

// External global variables
extern esp_mqtt_client_handle_t mqtt_client;
extern bool wifi_connected;
extern bool mqtt_connected;
extern EventGroupHandle_t connectivity_event_group;

// Function prototypes
void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
void heartbeat_task(void *pvParameters);
void led_status_task(void *pvParameters);

#endif // MQTT_HANDLER_H
