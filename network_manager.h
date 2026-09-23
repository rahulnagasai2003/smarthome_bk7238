#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include "config.h"
#include "hardware_manager.h"

extern "C" {
#include "ble_api_5_x.h"
void custom_ble_cmd_cb(ble_cmd_t cmd, ble_cmd_param_t *param);
void custom_ble_notice_cb(ble_notice_t notice, void *param);
}

// AP Mode + HTTP Server (same as ESP12F wifi_manager)
void startAPMode();
void handleHTTPClients();

// Wi-Fi Connection
void handleWiFiConnection();

// MQTT
void initMQTTTopics();
void connectMQTT();
void publishTelemetry();
void publishHeartbeat();
void requestTelemetryPublish();

// BLE Provisioning
void setupBLE();
void notifyApp(String msg);

#endif
