idf_component_register(SRCS "main.c" "led_control.c" "mqtt_handler.c"
                      INCLUDE_DIRS "."
                      REQUIRES esp_wifi esp_event esp_netif mqtt nvs_flash json driver led_strip cjson)
