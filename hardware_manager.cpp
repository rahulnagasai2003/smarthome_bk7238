#include "hardware_manager.h"

// ==========================================
// RESET BUTTON STATE
// ==========================================
static unsigned long resetPressStartTime = 0;
static bool isResetPressed = false;

// ==========================================
// DEVICE IDENTITY (from MAC address)
// Same approach as ESP12F: NLX-AP{N}S-{MAC}
// ==========================================
void applyHardwareIdentity() {
  WiFi.mode(WIFI_STA);
  uint8_t mac[6];
  WiFi.macAddress(mac);

  // +1 on last byte to match device registration convention
  mac[5] = mac[5] + 1;

  char cleanMac[13];
  snprintf(cleanMac, sizeof(cleanMac), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  deviceID = String(cleanMac);
  Serial.println("[IDENTITY] Device ID: " + deviceID);
}

// ==========================================
// RELAY STATE PERSISTENCE (EasyFlash)
// ==========================================
void saveRelayStatesToFlash() {
  char stateStr[NUM_RELAYS + 1];
  for (int i = 0; i < NUM_RELAYS; i++) {
    stateStr[i] = relayStates[i] ? '1' : '0';
  }
  stateStr[NUM_RELAYS] = '\0';
  ef_set_env("relay_states", stateStr);
  ef_save_env();
}

void loadRelayStatesFromFlash() {
  char *saved = ef_get_env("relay_states");
  if (saved != NULL && strlen(saved) == NUM_RELAYS) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      relayStates[i] = (saved[i] == '1');
      digitalWrite(relayPins[i], relayStates[i] ? HIGH : LOW);
    }
    Serial.println("[HW] Relay states loaded from flash.");
  } else {
    // Default all relays OFF
    for (int i = 0; i < NUM_RELAYS; i++) {
      relayStates[i] = false;
      digitalWrite(relayPins[i], LOW);
    }
    Serial.println("[HW] No saved relay states. All OFF.");
  }
}

// ==========================================
// CREDENTIAL PERSISTENCE (matches ESP12F NVS)
// Saves: WiFi SSID/pass, device_id, MQTT host/port/user/pass
// ==========================================
void saveCredentials(const char *ssid, const char *pass,
                     const char *deviceId_param, const char *mqttHost,
                     int mqttPort, const char *mqttUser,
                     const char *mqttPass) {
  ef_set_env("my_ssid", ssid);
  ef_set_env("my_pass", pass);

  if (deviceId_param != NULL && strlen(deviceId_param) > 0) {
    ef_set_env("device_id", deviceId_param);
  }

  if (mqttHost != NULL && strlen(mqttHost) > 0) {
    ef_set_env("mqtt_host", mqttHost);
  }

  char portStr[8];
  snprintf(portStr, sizeof(portStr), "%d", mqttPort);
  ef_set_env("mqtt_port", portStr);

  if (mqttUser != NULL) ef_set_env("mqtt_user", mqttUser);
  if (mqttPass != NULL) ef_set_env("mqtt_pass", mqttPass);

  ef_set_env("valid", "AB");
  ef_save_env();

  Serial.println("[FLASH] All credentials saved.");
}

bool loadCredentials() {
  char *valid = ef_get_env("valid");
  if (valid == NULL || strcmp(valid, "AB") != 0) {
    return false;
  }

  char *s = ef_get_env("my_ssid");
  char *p = ef_get_env("my_pass");
  if (s == NULL || strlen(s) < 1) return false;

  current_ssid = String(s);
  current_pass = (p != NULL) ? String(p) : "";

  // Load device_id (if saved from provisioning, overrides MAC-based ID)
  char *did = ef_get_env("device_id");
  if (did != NULL && strlen(did) > 0) {
    deviceID = String(did);
    Serial.println("[FLASH] Overriding Device ID from flash: " + deviceID);
  }

  // Load MQTT credentials
  char *mh = ef_get_env("mqtt_host");
  if (mh != NULL) saved_mqtt_host = String(mh);

  char *mp = ef_get_env("mqtt_port");
  if (mp != NULL) saved_mqtt_port = atoi(mp);

  char *mu = ef_get_env("mqtt_user");
  if (mu != NULL) saved_mqtt_user = String(mu);

  char *mpass = ef_get_env("mqtt_pass");
  if (mpass != NULL) saved_mqtt_pass = String(mpass);

  Serial.printf("[FLASH] Loaded: SSID=%s, MQTT=%s:%d\n",
                current_ssid.c_str(), saved_mqtt_host.c_str(), saved_mqtt_port);
  return true;
}

// ==========================================
// CLEAR ALL STORED CREDENTIALS
// ==========================================
void clearAllFlash() {
  ef_set_env("my_ssid", "");
  ef_set_env("my_pass", "");
  ef_set_env("device_id", "");
  ef_set_env("mqtt_host", "");
  ef_set_env("mqtt_port", "");
  ef_set_env("mqtt_user", "");
  ef_set_env("mqtt_pass", "");
  ef_set_env("valid", "");
  ef_save_env();
  Serial.println("[FLASH] All credentials cleared.");
}

// ==========================================
// SET SINGLE RELAY (direct GPIO, no UART)
// ==========================================
void setRelay(int index, bool state) {
  if (index < 0 || index >= NUM_RELAYS) return;
  relayStates[index] = state;
  digitalWrite(relayPins[index], state ? HIGH : LOW);
  Serial.printf("[HW] Relay %d -> %s\n", index + 1, state ? "ON" : "OFF");
}

// ==========================================
// RESET BUTTON (5 second hold = factory reset)
// Same as ESP12F config_button_task
// ==========================================
void handleResetButton() {
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!isResetPressed) {
      isResetPressed = true;
      resetPressStartTime = millis();
    } else if (millis() - resetPressStartTime > 5000) {
      Serial.println("[RESET] Button held 5s. Factory reset...");
      clearAllFlash();
      delay(500);
      bk_reboot();
    }
  } else {
    isResetPressed = false;
  }
}
