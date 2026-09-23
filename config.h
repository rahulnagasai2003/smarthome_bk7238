#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

// ==========================================
// DEVICE CONFIGURATION
// ==========================================
#define SWITCH_COUNT 4
#define NUM_RELAYS   SWITCH_COUNT
#define RESET_BUTTON_PIN 23

// ==========================================
// HARDWARE DEFINITIONS
// ==========================================
extern const int relayPins[];
extern bool relayStates[];

// ==========================================
// WI-FI & MQTT GLOBALS
// ==========================================
extern String current_ssid;
extern String current_pass;
extern String saved_mqtt_host;
extern int    saved_mqtt_port;
extern String saved_mqtt_user;
extern String saved_mqtt_pass;

extern bool trigger_wifi_connection;
extern bool wifi_success_notified;
extern bool ap_mode_active;
extern bool mqtt_is_connected;

// ==========================================
// DEVICE IDENTITY & MQTT TOPICS
// ==========================================
extern String deviceID;

extern String topic_lwt;
extern String topic_cmd_state;
extern String topic_stat_state;
extern String topic_heartbeat;
extern String topic_ack;
extern String topic_cmd_wildcard;

// ==========================================
// OBJECTS
// ==========================================
extern PubSubClient mqtt_client;

#endif
