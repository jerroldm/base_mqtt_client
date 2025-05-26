#include <string.h>
#include <inttypes.h> // For PRIi32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

static const char *TAG = "MQTT_CLIENT";
static esp_mqtt_client_handle_t mqtt_handle;
static EventGroupHandle_t wifi_event_group;
static const int CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "Retrying WiFi connection...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "Got IP address");
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to MQTT Broker");
            esp_mqtt_client_subscribe(mqtt_handle, "test/data", 0);
            ESP_LOGI(TAG, "Subscribed to test/data");
            esp_mqtt_client_subscribe(mqtt_handle, "broker/to/client1", 0);
            ESP_LOGI(TAG, "Subscribed to broker/to/client1");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected from MQTT Broker");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT data received, topic: %.*s, data: %.*s",
                     event->topic_len, event->topic, event->data_len, event->data);
            break;
        default:
            ESP_LOGI(TAG, "Other MQTT event id: %" PRIi32, event_id);
            break;
    }
}

void wifi_init_station(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "ESP32_Network",
            .password = "password123",
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi station initialized, connecting to AP: %s", wifi_config.sta.ssid);
}

void mqtt_task(void *pvParameters) {
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri     = "mqtt://192.168.4.1",
        .broker.address.port    = 1883,
        .credentials.client_id  = "Client1" // Unique Client ID
    };
    mqtt_handle = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_handle, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    esp_mqtt_client_start(mqtt_handle);

    int counter = 0;
    char data[50];
    while (true) {
        snprintf(data, sizeof(data), "Message %d from Client1", counter++); // Changed to Client1
        esp_mqtt_client_publish(mqtt_handle, "test/data", data, 0, 1, 0);
        ESP_LOGI(TAG, "Published: %s", data);
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_station();

    xTaskCreate(mqtt_task, "mqtt_task", 4096, NULL, 5, NULL);
}
