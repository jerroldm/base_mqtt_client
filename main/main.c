#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <mqtt_client.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "ESP32_Client";
static const int LED_PIN = 2; // Adjust to your LED GPIO pin
static bool led_state = false;
#define KIOSK_NAME "Kiosk 4" // Change to "Kiosk 2", etc., for other clients
static esp_mqtt_client_handle_t mqtt_client;
static bool wifi_connected = false;

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
    while (1) {
        if (mqtt_client && wifi_connected) {
            char topic[64];
            snprintf(topic, sizeof(topic), "esp32/kiosk/%s/heartbeat", KIOSK_NAME);
            esp_mqtt_client_publish(mqtt_client, topic, "alive", 0, 1, 0);
            ESP_LOGI(TAG, "Published heartbeat to %s", topic);
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // Heartbeat every 10 seconds
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            char topic[64];
            snprintf(topic, sizeof(topic), "esp32/kiosk/%s/led", KIOSK_NAME);
            esp_mqtt_client_subscribe(event->client, topic, 1);
            ESP_LOGI(TAG, "Subscribed to %s", topic);

            mqtt_client = event->client;

            // Send initial announcement
            esp_netif_ip_info_t ip_info;
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                cJSON *json = cJSON_CreateObject();
                cJSON_AddStringToObject(json, "name", KIOSK_NAME);
                cJSON_AddStringToObject(json, "ip", ip_str);
                char *message = cJSON_PrintUnformatted(json);
                esp_mqtt_client_publish(event->client, "devices/announce", message, 0, 1, 0);
                ESP_LOGI(TAG, "Published initial announcement: %s", message);
                cJSON_Delete(json);
                free(message);
            } else {
                ESP_LOGW(TAG, "Skipping initial announcement: invalid IP");
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            mqtt_client = NULL;
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT data received, topic: %.*s, data: %.*s",
                     event->topic_len, event->topic, event->data_len, event->data);
            if (strncmp(event->data, "toggle", event->data_len) == 0) {
                led_state = !led_state;
                gpio_set_level(LED_PIN, led_state);
                ESP_LOGI(TAG, "LED toggled to %d", led_state);
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

    // Initialize LED GPIO
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, led_state);

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
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for Wi-Fi connection
    while (!wifi_connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

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
    xTaskCreate(heartbeat_task, "heartbeat_task", 2048, NULL, 5, NULL);

    vTaskDelete(NULL);
}
