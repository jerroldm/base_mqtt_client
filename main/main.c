#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include "led_strip.h"
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <mqtt_client.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define KIOSK_NAME "Kiosk 5" // Change to "Kiosk 2", etc., for other clients

static const char *TAG = "ESP32_Client";
static bool led_state = false;
static esp_mqtt_client_handle_t mqtt_client;
static bool wifi_connected = false;

//----------------------------------------
static led_strip_handle_t led_strip;
#define LED_STRIP_GPIO 48 // Onboard RGB LED on ESP32-S3-DevKitC-1 v1.1

static void configure_led(void)
{
    /* LED strip initialization with the GPIO and pixels number*/
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    /* Set all LED off to clear all pixels */
    led_strip_clear(led_strip);
}

static void set_led_state(bool state) {
    if (state) {
        // Set RGB to white (low intensity to avoid bright glare)
        //                                                R   G  B
        ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, 0, 16, 0));
    } else {
        // Clear RGB LED
        ESP_ERROR_CHECK(led_strip_clear(led_strip));
    }
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));
    ESP_LOGI(TAG, "RGB LED set to %s", state ? "on" : "off");
}
//----------------------------------------


static void log_stack_usage(const char *task_name, TaskHandle_t task_handle) {
    UBaseType_t high_water_mark = uxTaskGetStackHighWaterMark(task_handle);
    ESP_LOGI(TAG, "%s stack high water mark: %u bytes", task_name, high_water_mark * sizeof(StackType_t));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

static void heartbeat_task(void *pvParameters) {
    TaskHandle_t task_handle = xTaskGetCurrentTaskHandle();
    while (true) {
        if (mqtt_client && wifi_connected) {
            static char topic[64]; // Static to reduce stack usage
            snprintf(topic, sizeof(topic), "esp32/kiosk/%s/heartbeat", KIOSK_NAME);
            esp_mqtt_client_publish(mqtt_client, topic, "alive", 0, 1, 0);
            ESP_LOGI(TAG, "Published heartbeat to %s", topic);
            log_stack_usage("Heartbeat", task_handle);
            ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // Heartbeat every 10 seconds
    }
}

static void led_status_task(void *pvParameters) {
    TaskHandle_t task_handle = xTaskGetCurrentTaskHandle();
    while (true) {
        if (mqtt_client && wifi_connected) {
            char topic[64];
            snprintf(topic, sizeof(topic), "esp32/kiosk/%s/led_status", KIOSK_NAME);
            const char *status = led_state ? "on" : "off";
            esp_mqtt_client_publish(mqtt_client, topic, status, 0, 1, 0);
            ESP_LOGI(TAG, "Published LED status to %s: %s", topic, status);
            log_stack_usage("LED Status", task_handle);
        }
        vTaskDelay(pdMS_TO_TICKS(5000)); // Report every 5 seconds
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            esp_mqtt_client_subscribe(mqtt_client, "esp32/kiosk/" KIOSK_NAME "/led", 1);
            esp_mqtt_client_subscribe(mqtt_client, "esp32/request_announce", 1);
            // Publish initial announcement
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            char announce_topic[64];
            snprintf(announce_topic, sizeof(announce_topic), "esp32/kiosk/%s/announce", KIOSK_NAME);
            esp_mqtt_client_publish(mqtt_client, announce_topic, ip_str, 0, 1, 0);
            ESP_LOGI(TAG, "Published IP: %s to %s", ip_str, announce_topic);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            break;
        case MQTT_EVENT_DATA:
            if (strncmp(event->topic, "esp32/kiosk/" KIOSK_NAME "/led", event->topic_len) == 0) {
                if (strncmp(event->data, "toggle", event->data_len) == 0) {
                    led_state = !led_state;
                    set_led_state(led_state);
                    ESP_LOGI(TAG, "LED toggled to %d", led_state);
                } else if (strncmp(event->data, "on", event->data_len) == 0) {
                    led_state = 1;
                    set_led_state(led_state);
                    ESP_LOGI(TAG, "LED turned on");
                } else if (strncmp(event->data, "off", event->data_len) == 0) {
                    led_state = 0;
                    set_led_state(led_state);
                    ESP_LOGI(TAG, "LED turned off");
                }
                if (wifi_connected) {
                    char status_topic[64];
                    snprintf(status_topic, sizeof(status_topic), "esp32/kiosk/%s/led_status", KIOSK_NAME);
                    const char *status = led_state ? "on" : "off";
                    esp_mqtt_client_publish(mqtt_client, status_topic, status, 0, 1, 0);
                    ESP_LOGI(TAG, "Published LED status on change to %s: %s", status_topic, status);
                }
            } else if (strncmp(event->topic, "esp32/request_announce", event->topic_len) == 0) {
                esp_netif_ip_info_t ip_info;
                esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                char announce_topic[64];
                snprintf(announce_topic, sizeof(announce_topic), "esp32/kiosk/%s/announce", KIOSK_NAME);
                esp_mqtt_client_publish(mqtt_client, announce_topic, ip_str, 0, 1, 0);
                ESP_LOGI(TAG, "Published IP: %s to %s on announce request", ip_str, announce_topic);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error, error_code=%d", event->error_handle->error_type);
            break;
        default:
            break;
    }
}

void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize RGB LED
    configure_led();

    // Initialize Wi-Fi
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register Wi-Fi event handler
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "ESP32_Network",
            .password = "password123",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Initialize MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://192.168.4.1",
        .broker.address.port = 1883,
        .credentials.client_id = KIOSK_NAME,
        .session.keepalive = 30,
        .network.timeout_ms = 10000,
        .network.reconnect_timeout_ms = 5000,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    // Start heartbeat task
    xTaskCreate(heartbeat_task, "heartbeat_task", 4096, NULL, 5, NULL);
    // Start led_status task
    xTaskCreate(led_status_task, "led_status_task", 4096, NULL, 5, NULL);
}
