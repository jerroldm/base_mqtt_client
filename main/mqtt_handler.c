#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include "config.h"
#include "mqtt_handler.h"
#include "led_control.h"

static const char *TAG = CONFIG_TAG;        // Defined in config.h

// Global variables
esp_mqtt_client_handle_t mqtt_client = NULL;
bool wifi_connected = false;
bool mqtt_connected = false;
EventGroupHandle_t connectivity_event_group;

static void log_stack_usage(const char *task_name, TaskHandle_t task_handle)
{
    UBaseType_t high_water_mark = uxTaskGetStackHighWaterMark(task_handle);
    ESP_LOGI(TAG, "%s stack high water mark: %u bytes", task_name, high_water_mark * sizeof(StackType_t));
}

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(connectivity_event_group, WIFI_CONNECTED_BIT);
        wifi_connected = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        xEventGroupSetBits(connectivity_event_group, WIFI_CONNECTED_BIT);
    }
}

void heartbeat_task(void *pvParameters)
{
    TaskHandle_t task_handle = xTaskGetCurrentTaskHandle();
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t heartbeat_interval = pdMS_TO_TICKS(10000);

    while (true) {
        if (mqtt_connected && wifi_connected) {
            char topic[64];
            snprintf(topic, sizeof(topic), "esp32/kiosk/%s/heartbeat", KIOSK_NAME);
            esp_mqtt_client_enqueue(mqtt_client, topic, "alive", 0, 1, 0, false);
            ESP_LOGI(TAG, "Enqueued heartbeat to %s", topic);
            log_stack_usage("Heartbeat", task_handle);
            ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());
        }
        vTaskDelayUntil(&last_wake_time, heartbeat_interval);
    }
}

void led_status_task(void *pvParameters)
{
    TaskHandle_t task_handle = xTaskGetCurrentTaskHandle();
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t status_interval = pdMS_TO_TICKS(5000);     // Update status every 5s

    while (true) {
        if (mqtt_connected && wifi_connected) {
            char topic[64];
            snprintf(topic, sizeof(topic), "esp32/kiosk/%s/led_status", KIOSK_NAME);
            const char *status = led_state ? "on" : "off";
            esp_mqtt_client_enqueue(mqtt_client, topic, status, 0, 1, 0, false);
            ESP_LOGI(TAG, "Enqueued LED status to %s: %s", topic, status);
            log_stack_usage("LED Status", task_handle);
        }
        vTaskDelayUntil(&last_wake_time, status_interval);
    }
}

void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            mqtt_connected = true;
            xEventGroupSetBits(connectivity_event_group, MQTT_CONNECTED_BIT);
            esp_mqtt_client_subscribe(mqtt_client, "esp32/kiosk/" KIOSK_NAME "/led", 1);
            esp_mqtt_client_subscribe(mqtt_client, "esp32/request_announce", 1);
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            char announce_topic[64];
            snprintf(announce_topic, sizeof(announce_topic), "esp32/kiosk/%s/announce", KIOSK_NAME);
            esp_mqtt_client_enqueue(mqtt_client, announce_topic, ip_str, 0, 1, 0, false);
            ESP_LOGI(TAG, "Enqueued IP: %s to %s", ip_str, announce_topic);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            mqtt_connected = false;
            xEventGroupClearBits(connectivity_event_group, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_DATA:
            if (strncmp(event->topic, "esp32/kiosk/" KIOSK_NAME "/led", event->topic_len) == 0) {
                bool publish_status = false;
                if (strncmp(event->data, "toggle", event->data_len) == 0) {
                    led_state = !led_state;
                    set_led_state(led_state);
                    ESP_LOGI(TAG, "LED toggled to %d", led_state);
                    publish_status = true;
                } else if (strncmp(event->data, "on", event->data_len) == 0) {
                    led_state = true;
                    set_led_state(led_state);
                    ESP_LOGI(TAG, "LED turned on");
                    publish_status = true;
                } else if (strncmp(event->data, "off", event->data_len) == 0) {
                    led_state = false;
                    set_led_state(led_state);
                    ESP_LOGI(TAG, "LED turned off");
                    publish_status = true;
                }

                if (publish_status && mqtt_connected && wifi_connected) {
                    char status_topic[64];
                    snprintf(status_topic, sizeof(status_topic), "esp32/kiosk/%s/led_status", KIOSK_NAME);
                    const char *status = led_state ? "on" : "off";
                    esp_mqtt_client_enqueue(mqtt_client, status_topic, status, 0, 0, 0, true);
                    ESP_LOGI(TAG, "Immediate LED status update enqueued to %s: %s", status_topic, status);
                }
            } else if (strncmp(event->topic, "esp32/request_announce", event->topic_len) == 0) {
                esp_netif_ip_info_t ip_info;
                esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                char announce_topic[64];
                snprintf(announce_topic, sizeof(announce_topic), "esp32/kiosk/%s/announce", KIOSK_NAME);
                esp_mqtt_client_enqueue(mqtt_client, announce_topic, ip_str, 0, 1, 0, false);
                ESP_LOGI(TAG, "Enqueued IP: %s to %s on announce request", ip_str, announce_topic);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error, error_code=%d", event->error_handle->error_type);
            break;
        default:
            break;
    }
}
