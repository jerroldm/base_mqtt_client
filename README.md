# Base MQTT Client

ESP32-S3 MQTT client using ESP-IDF v5.3.1. Connects to a broker at `mqtt://192.168.4.1:1883`, publishes `Message X from Client1` to `test/data` every 10 seconds, and subscribes to `test/data` and `broker/to/client1`.

## Setup
- SSID: ESP32_Network
- Password: password123
- Broker: mqtt://192.168.4.1:1883
- Client ID: Client1
- No authentication

## Dependencies
- ESP-IDF v5.3.1

## Build
```bash
source $HOME/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB1 flash monitor
