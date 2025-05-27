#include <stdio.h>
#include <string.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <mqtt_client.h>
#include <cJSON.h>

static const char *TAG = "ESP32_Client";
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
const int MQTT_CONNECTED_BIT = BIT1;
static int wifi_retry_count = 0;
const int MAX_WIFI_RETRIES = 30;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting to AP...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count < MAX_WIFI_RETRIES) {
            ESP_LOGW(TAG, "Disconnected from AP, retrying (%d/%d)...", wifi_retry_count + 1, MAX_WIFI_RETRIES);
            wifi_retry_count++;
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        } else {
            ESP_LOGE(TAG, "Failed to connect to AP after %d retries, restarting...", wifi_retry_count);
            esp_restart();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            esp_mqtt_client_subscribe(event->client, "devices/control", 0);
            xEventGroupSetBits(wifi_event_group, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT disconnected from broker");
            xEventGroupClearBits(wifi_event_group, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT subscribed to devices/control");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT data received, topic: %.*s, data: %.*s",
                     event->topic_len, event->topic, event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error, error_code=%d", event->error_handle->error_type);
            break;
        default:
            ESP_LOGI(TAG, "MQTT event %d", event->event_id);
            break;
    }
}

static void mqtt_task(void *pvParameters) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://192.168.4.1",
        .broker.address.port = 1883,
        .credentials.client_id = "Client1",
        .session.keepalive = 60,
        .network.timeout_ms = 10000,
        .network.reconnect_timeout_ms = 2000,
        .network.disable_auto_reconnect = false,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    xEventGroupWaitBits(wifi_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "MQTT connected, starting publishes");

    while (1) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "name", "Client1");
        cJSON_AddStringToObject(msg, "status", "active");
        cJSON_AddNumberToObject(msg, "timestamp", xTaskGetTickCount() * portTICK_PERIOD_MS);
        char *payload = cJSON_PrintUnformatted(msg);
        esp_mqtt_client_publish(client, "devices/announce", payload, 0, 0, 0);
        ESP_LOGI(TAG, "Published: %s", payload);
        cJSON_Delete(msg);
        free(payload);
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "ESP32_Network",
            .password = "password123",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84)); // Moved after esp_wifi_start

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected, starting MQTT task");
    xTaskCreate(mqtt_task, "mqtt_task", 12288, NULL, 5, NULL);
}
