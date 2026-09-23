#include "config.h"

// ==========================================
// HARDWARE DEFINITIONS
// ==========================================
const int relayPins[] = {9, 15, 0, 1};
bool relayStates[]    = {false, false, false, false};

// ==========================================
// WI-FI & MQTT GLOBALS
// ==========================================
String current_ssid = "";
String current_pass = "";
String saved_mqtt_host = "";
int    saved_mqtt_port = 0;
String saved_mqtt_user = "";
String saved_mqtt_pass = "";

bool trigger_wifi_connection = false;
bool wifi_success_notified   = false;
bool ap_mode_active          = false;
bool mqtt_is_connected       = false;

// ==========================================
// DEVICE IDENTITY & MQTT TOPICS
// ==========================================
String deviceID = "";

String topic_lwt          = "";
String topic_cmd_state    = "";
String topic_stat_state   = "";
String topic_heartbeat    = "";
String topic_ack          = "";
String topic_cmd_wildcard = "";
