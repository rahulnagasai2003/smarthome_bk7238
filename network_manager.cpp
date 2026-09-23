#include "network_manager.h"
#include "hardware_manager.h"
#include "config.h"
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>

extern "C" {
  int wifi_set_mac_address(char *mac);
}

// lwip sockets for raw TCP server (WiFiServer is disabled in BK7238 SDK)
extern "C" {
  #include <lwip/sockets.h>
  #include <lwip/netdb.h>
  bk_err_t bk_wifi_start_softap(const char *ssid, const char *pwd);
}
// Undefine lwip macros that clash with C++ methods (PubSubClient::connect, etc.)
#undef connect
#undef write
#undef read
#undef close
#undef bind
#undef send
#undef recv

// ==========================================
// NETWORK OBJECTS
// ==========================================
static int http_server_fd = -1;  // Raw socket (replaces WiFiServer)
WiFiClient wifiClient;
WiFiClientSecure wifiClientSecure;
PubSubClient mqtt_client; // Initialize empty, setClient later

// ==========================================
// TELEMETRY DEBOUNCE (matches ESP12F telemetry_task)
// ==========================================
static bool telemetry_pending = false;
static unsigned long telemetry_pending_time = 0;
#define TELEMETRY_DEBOUNCE_MS 100

// ==========================================
// HEARTBEAT TIMER (matches ESP12F 9-second interval)
// ==========================================
static unsigned long lastHeartbeatTime = 0;
#define HEARTBEAT_INTERVAL_MS 9000

// ==========================================
// WIFI RETRY TRACKING
// ==========================================
static unsigned long lastWifiRetry = 0;
static int wifiRetries = 0;
#define WIFI_RETRY_INTERVAL_MS 15000
#define WIFI_MAX_RETRIES 12

// ==========================================
// MQTT RETRY TRACKING
// ==========================================
static unsigned long lastMqttRetry = 0;
#define MQTT_RETRY_INTERVAL_MS 10000

// ==========================================
// GLOBAL DEVICE INFO
// ==========================================
static char global_ap_ssid[32] = {0};
static char global_mac_str[18] = {0};

// ==========================================
// BLE GLOBALS (from existing BK7238 code)
// ==========================================
uint8_t ble_actv_idx = 0;
#define UNKNOW_ACT_IDX 0xFF

enum {
  PROV_IDX_SVC,
  PROV_IDX_RX_CHAR,
  PROV_IDX_RX_VALUE,
  PROV_IDX_TX_CHAR,
  PROV_IDX_TX_VALUE,
  PROV_IDX_NB
};

#define BK_ATT_DECL_PRIMARY_SERVICE_128 \
  {0x00, 0x28, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, \
   0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0, 0x0}
#define BK_ATT_DECL_CHARACTERISTIC_128 \
  {0x03, 0x28, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, \
   0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0, 0x0}
#define WRITE_REQ_CHARACTERISTIC_128 \
  {0x01, 0xFF, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, \
   0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0, 0x0}
#define NOTIFY_CHARACTERISTIC_128 \
  {0x02, 0xFF, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, \
   0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0, 0x0}

static const uint8_t prov_svc_uuid[16] = {
    0xFF, 0xFF, 0, 0, 0x34, 0x56, 0, 0, 0, 0, 0x28, 0x37, 0, 0, 0, 0};

// BLE advertisement: "NLX 4S"
uint8_t ble_adv_data[] = {0x08, 0x09, 'N', 'L', 'X', ' ', '4', 'S'};

bk_attm_desc_t prov_att_db[PROV_IDX_NB] = {
    [PROV_IDX_SVC]      = {BK_ATT_DECL_PRIMARY_SERVICE_128, PROP(RD), 0},
    [PROV_IDX_RX_CHAR]  = {BK_ATT_DECL_CHARACTERISTIC_128, PROP(RD), 0},
    [PROV_IDX_RX_VALUE] = {WRITE_REQ_CHARACTERISTIC_128,
                           PROP(WR) | ATT_UUID(128), 128},
    [PROV_IDX_TX_CHAR]  = {BK_ATT_DECL_CHARACTERISTIC_128, PROP(RD), 0},
    [PROV_IDX_TX_VALUE] = {NOTIFY_CHARACTERISTIC_128,
                           PROP(N) | ATT_UUID(128), 128},
};

// ==========================================
// BLE CALLBACKS (extended with MQTT credentials)
// ==========================================
void notifyApp(String msg) {
  bk_ble_send_ntf_value(msg.length(), (uint8_t *)msg.c_str(), 0,
                        PROV_IDX_TX_VALUE);
  Serial.println("[BLE] --> SENT TO APP: " + msg);
}

extern "C" void custom_ble_cmd_cb(ble_cmd_t cmd, ble_cmd_param_t *param) {
  if (param->status == 0) {
    if (cmd == 1)
      bk_ble_set_adv_data(ble_actv_idx, (uint8_t *)ble_adv_data,
                          sizeof(ble_adv_data), custom_ble_cmd_cb);
    else if (cmd == 2)
      bk_ble_start_advertising(ble_actv_idx, 0, custom_ble_cmd_cb);
  }
}

extern "C" void custom_ble_notice_cb(ble_notice_t notice, void *param) {
  if (notice == BLE_5_CREATE_DB) {
    ble_actv_idx = app_ble_get_idle_actv_idx_handle();
    if (ble_actv_idx != UNKNOW_ACT_IDX) {
      bk_ble_create_advertising(ble_actv_idx, 7, 0x120, 0x160,
                                custom_ble_cmd_cb);
    }
  } else if (notice == BLE_5_WRITE_EVENT) {
    write_req_t *req = (write_req_t *)param;
    String payload = "";
    for (int i = 0; i < req->len; i++)
      payload += (char)req->value[i];

    Serial.println("[BLE] Received JSON: " + payload);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload.c_str());

    if (!error) {
      if (doc["op"] == "PROV") {
        current_ssid = doc["ssid"].as<const char *>();
        current_pass = doc["pass"].as<const char *>();

        // Also accept MQTT credentials via BLE (extended from ESP12F)
        if (doc.containsKey("mqttHost")) {
          const char *mh = doc["mqttHost"].as<const char*>();
          if (mh) saved_mqtt_host = String(mh);
        }
        if (doc.containsKey("mqttPort")) {
          saved_mqtt_port = doc["mqttPort"].as<int>();
        }
        if (doc.containsKey("mqttUser")) {
          const char *mu = doc["mqttUser"].as<const char*>();
          if (mu) saved_mqtt_user = String(mu);
        }
        if (doc.containsKey("mqttPass")) {
          const char *mp = doc["mqttPass"].as<const char*>();
          if (mp) saved_mqtt_pass = String(mp);
        }
        if (doc.containsKey("deviceId")) {
          const char *did = doc["deviceId"].as<const char*>();
          if (did) deviceID = String(did);
        }

        trigger_wifi_connection = true;
        wifi_success_notified = false;
        Serial.println("[BLE] Provisioning received for SSID: " + current_ssid);
      }
    } else {
      Serial.println("[BLE] JSON Parse Error");
    }
  }
}

void setupBLE() {
  ble_set_notice_cb(custom_ble_notice_cb);
  struct bk_ble_db_cfg cfg;
  cfg.att_db = prov_att_db;
  cfg.att_db_nb = PROV_IDX_NB;
  cfg.prf_task_id = 0;
  cfg.start_hdl = 0;
  cfg.svc_perm = BK_PERM_SET(SVC_UUID_LEN, UUID_128);
  memcpy(&(cfg.uuid[0]), &prov_svc_uuid, 16);
  bk_ble_create_db(&cfg);
}

// ==========================================
// HTTP HELPER: Send HTTP Response via raw socket
// ==========================================
static void sendHTTPResponse(int client_fd, int code,
                             const char *contentType, const char *body) {
  char resp[1024];
  const char *codeStr = (code == 200) ? "200 OK" :
                        (code == 404) ? "404 Not Found" : "400 Bad Request";
  int bodyLen = strlen(body);
  int len = snprintf(resp, sizeof(resp),
      "HTTP/1.1 %s\r\n"
      "Server: ESP32 HTTP Server\r\n"
      "Content-Type: %s\r\n"
      "Connection: close\r\n"
      "Content-Length: %d\r\n"
      "\r\n"
      "%s",
      codeStr, contentType, bodyLen, body);
  lwip_send(client_fd, resp, len, 0);
}

// ==========================================
// AP MODE + HTTP SERVER (raw lwip sockets)
// Same as ESP12F wifi_manager.c start_ap_mode()
// ==========================================
void startAPMode() {
  uint8_t mac[6];
  WiFi.macAddress(mac);

  snprintf(global_ap_ssid, sizeof(global_ap_ssid), "NLX-AP%dS-%02X%02X",
           SWITCH_COUNT, mac[4], mac[5]);
           
  // Spoof Espressif MAC OUI (A0:B7:65) so the app doesn't reject Beken MACs
  snprintf(global_mac_str, sizeof(global_mac_str), "A0:B7:65:%02X:%02X:%02X",
           mac[3], mac[4], mac[5]);

  // ACTUALLY CHANGE THE HARDWARE MAC ADDRESS SO BSSID MATCHES!
  uint8_t spoofed_mac[6] = {0xA0, 0xB7, 0x65, mac[3], mac[4], mac[5]};
  wifi_set_mac_address((char *)spoofed_mac);

  // FORCE BK7238 AP IP to 192.168.4.1 BEFORE starting AP!
  bk_netif_ip_str_info_t ap_ip;
  memset(&ap_ip, 0, sizeof(ap_ip));
  strcpy(ap_ip.ip, "192.168.4.1");
  strcpy(ap_ip.netmask, "255.255.255.0");
  strcpy(ap_ip.gw, "192.168.4.1");
  strcpy(ap_ip.dns1, "192.168.4.1");
  bk_netif_set_ip_info(WIFI_MODE_AP, &ap_ip);

  // Start AP
  WiFi.enableAP(true);
  bk_wifi_start_softap(global_ap_ssid, "");

  delay(500);  // Allow AP to start

  // Create raw TCP server socket on port 80
  http_server_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (http_server_fd < 0) {
    Serial.println("[AP] ERROR: Failed to create server socket!");
    return;
  }
  int enable = 1;
  lwip_setsockopt(http_server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = lwip_htons(80);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  int bind_ret = lwip_bind(http_server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
  if (bind_ret < 0) {
    Serial.println("[AP] ERROR: Failed to bind server socket! Port 80 might be in use.");
    lwip_close(http_server_fd);
    http_server_fd = -1;
    return;
  }

  if (lwip_listen(http_server_fd, 4) < 0) {
    Serial.println("[AP] ERROR: Failed to listen!");
    lwip_close(http_server_fd);
    http_server_fd = -1;
    return;
  }

  // Set non-blocking so handleHTTPClients() doesn't block the loop
  lwip_fcntl(http_server_fd, F_SETFL, O_NONBLOCK);

  ap_mode_active = true;

  Serial.printf("[AP] Started AP: %s\n", global_ap_ssid);
  
  // Wait a moment for IP to configure, then print it
  delay(100);
  Serial.println("[AP] HTTP Server IP: 192.168.4.1");
  Serial.println("[AP] HTTP Server on port 80");
}

// ==========================================
// HTTP REQUEST HANDLER
// Endpoints: GET /, POST /api/wifi/configure, POST /reset
// Same as ESP12F wifi_manager.c handlers
// ==========================================
void handleHTTPClients() {
  if (!ap_mode_active || http_server_fd < 0) return;

  // Accept new client (non-blocking)
  struct sockaddr_in client_addr;
  int cs = sizeof(struct sockaddr_in);
  int client_fd = lwip_accept(http_server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&cs);
  if (client_fd < 0) return;  // No client waiting

  Serial.println("\n[HTTP] New client connected!");

  // Set receive timeout on client socket
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  lwip_setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  delay(10); // Wait a tiny bit for the client to send bytes

  // Read the entire HTTP request into a buffer
  char reqBuf[1024];
  int totalRead = 0;
  unsigned long readStart = millis();
  while (totalRead < (int)sizeof(reqBuf) - 1 && millis() - readStart < 5000) {
    int n = lwip_recv(client_fd, reqBuf + totalRead, sizeof(reqBuf) - 1 - totalRead, MSG_DONTWAIT);
    if (n > 0) {
      totalRead += n;
      // Check if we've received the full request (headers end with \r\n\r\n)
      reqBuf[totalRead] = '\0';
      char *headerEnd = strstr(reqBuf, "\r\n\r\n");
      if (headerEnd) {
        // Case-insensitive check for Content-Length
        char reqLower[1024];
        for (int i = 0; i < totalRead && i < 1023; i++) {
          reqLower[i] = tolower((unsigned char)reqBuf[i]);
        }
        reqLower[totalRead] = '\0';
        
        char *clHeader = strstr(reqLower, "content-length:");
        if (clHeader) {
          int cl = atoi(clHeader + 15);
          int bodyReceived = totalRead - (int)(headerEnd + 4 - reqBuf);
          if (bodyReceived >= cl) break;  // Got full body
        } else {
          break;  // No body expected
        }
      }
    } else if (n == 0) {
      break;  // Client disconnected
    } else {
      // EWOULDBLOCK or waiting for data
      delay(10);
      bk_wdg_reload();
    }
  }
  reqBuf[totalRead] = '\0';

  if (totalRead == 0) {
    lwip_close(client_fd);
    return; // Client closed connection or timed out
  }

  // Parse method and path from first line
  char method[8] = {0};
  char path[128] = {0};
  sscanf(reqBuf, "%7s %127s", method, path);

  // Find body (after \r\n\r\n)
  char *body = strstr(reqBuf, "\r\n\r\n");
  if (body) body += 4; else body = (char *)"";

  Serial.printf("[HTTP] %s %s\n", method, path);

  // ---- ROUTE: GET / ----
  if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
    char json_resp[256];
    snprintf(json_resp, sizeof(json_resp),
             "{\"device\":\"Neuro_Touch\",\"switch_count\":%d,"
             "\"mac_address\":\"%s\",\"firmware\":\"1.0.0\","
             "\"status\":\"awaiting_config\"}",
             SWITCH_COUNT, global_mac_str);

    Serial.printf("[HTTP] GET / response MAC: %s\n", global_mac_str);
    sendHTTPResponse(client_fd, 200, "application/json", json_resp);
  }

  // ---- ROUTE: POST /api/wifi/configure ----
  else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wifi/configure") == 0) {
    Serial.printf("[HTTP] Raw POST body: %s\n", body);

    char ssid[64] = {0};
    char pass[64] = {0};
    char dev_id[64] = {0};
    char mqtt_host[128] = {0};
    int  mqtt_port = 0;
    char mqtt_user[64] = {0};
    char mqtt_pass[64] = {0};

    if (body[0] == '{') {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, body);
      if (!err) {
        if (doc.containsKey("ssid")) strncpy(ssid, doc["ssid"].as<const char*>(), 63);
        if (doc.containsKey("password")) strncpy(pass, doc["password"].as<const char*>(), 63);
        if (doc.containsKey("deviceId")) strncpy(dev_id, doc["deviceId"].as<const char*>(), 63);
        if (doc.containsKey("mqttHost")) strncpy(mqtt_host, doc["mqttHost"].as<const char*>(), 127);
        if (doc.containsKey("mqttPort")) mqtt_port = doc["mqttPort"].as<int>();
        if (doc.containsKey("mqttUsername")) strncpy(mqtt_user, doc["mqttUsername"].as<const char*>(), 63);
        if (doc.containsKey("mqttPassword")) strncpy(mqtt_pass, doc["mqttPassword"].as<const char*>(), 63);
      }
    } else {
      char bodyBuf[512];
      strncpy(bodyBuf, body, sizeof(bodyBuf) - 1);
      bodyBuf[sizeof(bodyBuf) - 1] = '\0';

      char *token = strtok(bodyBuf, "&");
      while (token) {
        if (strncmp(token, "ssid=", 5) == 0) strncpy(ssid, token + 5, 63);
        else if (strncmp(token, "password=", 9) == 0) strncpy(pass, token + 9, 63);
        else if (strncmp(token, "deviceId=", 9) == 0) strncpy(dev_id, token + 9, 63);
        else if (strncmp(token, "mqttHost=", 9) == 0) strncpy(mqtt_host, token + 9, 127);
        else if (strncmp(token, "mqttPort=", 9) == 0) mqtt_port = atoi(token + 9);
        else if (strncmp(token, "mqttUsername=", 13) == 0) strncpy(mqtt_user, token + 13, 63);
        else if (strncmp(token, "mqttPassword=", 13) == 0) strncpy(mqtt_pass, token + 13, 63);
        token = strtok(NULL, "&");
      }
    }

    if (strlen(ssid) == 0) {
      sendHTTPResponse(client_fd, 400, "application/json",
                       "{\"status\":\"error\",\"message\":\"SSID required\"}");
      lwip_close(client_fd);
      return;
    }

    if (strlen(dev_id) == 0) {
      char *existing_id = ef_get_env("device_id");
      if (existing_id != NULL && strlen(existing_id) > 0) {
        strncpy(dev_id, existing_id, 63);
      }
    }

    sendHTTPResponse(client_fd, 200, "application/json",
                     "{\"status\":\"received\"}");
    Serial.println("[HTTP] => SUCCESS: Sent {\"status\":\"received\"} to App!");

    saveCredentials(ssid, pass, dev_id, mqtt_host, mqtt_port, mqtt_user, mqtt_pass);
    
    lwip_close(client_fd);
    Serial.println("[HTTP] Credentials saved. Rebooting in 2.5s...");
    delay(2500);
    bk_reboot();
    return;
  }

  // ---- ROUTE: POST /reset ----
  else if (strcmp(method, "POST") == 0 && strcmp(path, "/reset") == 0) {
    sendHTTPResponse(client_fd, 200, "application/json",
                     "{\"success\":true,\"message\":\"Resetting device...\"}");
    
    lwip_close(client_fd);
    clearAllFlash();
    delay(500);
    bk_reboot();
    return;
  }

  // ---- ROUTE: GET /test (For manual browser testing) ----
  else if (strcmp(method, "GET") == 0 && strcmp(path, "/test") == 0) {
    const char *html = 
      "<html><head><title>Test Config</title></head><body>"
      "<h2>BK7238 Manual Config Test</h2>"
      "<form action='/api/wifi/configure' method='POST'>"
      "SSID: <input type='text' name='ssid'><br><br>"
      "Pass: <input type='text' name='password'><br><br>"
      "Device ID: <input type='text' name='deviceId' value='NLX-123456'><br><br>"
      "MQTT Host: <input type='text' name='mqttHost' value='broker.hivemq.com'><br><br>"
      "MQTT Port: <input type='text' name='mqttPort' value='1883'><br><br>"
      "MQTT User: <input type='text' name='mqttUsername'><br><br>"
      "MQTT Pass: <input type='text' name='mqttPassword'><br><br>"
      "<input type='submit' value='Send Details to BK7238!'>"
      "</form></body></html>";
      
    sendHTTPResponse(client_fd, 200, "text/html", html);
  }

  // ---- 404 ----
  else {
    sendHTTPResponse(client_fd, 404, "text/plain", "Not Found");
  }

  lwip_close(client_fd);
}

// ==========================================
// MQTT TOPICS (same structure as ESP12F mqtt_handler.c)
// ==========================================
void initMQTTTopics() {
  topic_lwt          = "devices/" + deviceID + "/lwt";
  topic_cmd_state    = "devices/" + deviceID + "/set";
  topic_cmd_wildcard = "devices/" + deviceID + "/#";
  topic_stat_state   = "nt/v1/" + deviceID + "/stat/telemetry";
  topic_heartbeat    = "devices/" + deviceID + "/heartbeat";
  topic_ack          = "devices/" + deviceID + "/ack";

  Serial.println("[MQTT] Topics initialized for device: " + deviceID);
}

// ==========================================
// PUBLISH TELEMETRY (same JSON format as ESP12F)
// {"deviceId":"xxx","switches":{"1":true,"2":false,...}}
// ==========================================
void publishTelemetry() {
  if (!mqtt_client.connected()) return;

  JsonDocument doc;
  doc["deviceId"] = deviceID;
  JsonObject switches = doc["switches"].to<JsonObject>();
  for (int i = 0; i < NUM_RELAYS; i++) {
    switches[String(i + 1)] = relayStates[i];
  }

  char buf[256];
  serializeJson(doc, buf);

  Serial.println("[MQTT] Publishing telemetry: " + String(buf));
  mqtt_client.publish(topic_stat_state.c_str(), (uint8_t *)buf, strlen(buf), false);
}

// Request telemetry with debounce (same as ESP12F debounce mechanism)
void requestTelemetryPublish() {
  telemetry_pending = true;
  telemetry_pending_time = millis();
}

// ==========================================
// PUBLISH HEARTBEAT (same as ESP12F telemetry_task heartbeat)
// {"deviceId":"xxx","rssi":-65}
// ==========================================
void publishHeartbeat() {
  if (!mqtt_client.connected()) return;

  char payload[128];
  snprintf(payload, sizeof(payload), "{\"deviceId\":\"%s\",\"rssi\":%d}",
           deviceID.c_str(), WiFi.RSSI());

  Serial.println("[MQTT] Heartbeat: " + String(payload));
  mqtt_client.publish(topic_heartbeat.c_str(), (uint8_t *)payload,
                      strlen(payload), true);
}

// ==========================================
// MQTT CALLBACK (same as ESP12F mqtt_event_handler MQTT_EVENT_DATA)
// Handles: devices/{id}/set
// ==========================================
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String payloadStr = "";
  for (unsigned int i = 0; i < length; i++) payloadStr += (char)payload[i];

  Serial.println("[MQTT] Received on: " + String(topic));
  Serial.println("[MQTT] Payload: " + payloadStr);

  // Handle relay commands on topic: devices/{id}/set
  // Same JSON format as ESP12F: {"msgId":"x","v":1,"ts":"x","payload":{"1":true,"2":false}}
  if (String(topic) == topic_cmd_state) {
    JsonDocument doc;
    if (deserializeJson(doc, payloadStr.c_str())) return;

    JsonObject cmd_payload = doc["payload"];
    if (cmd_payload.isNull()) return;

    bool changed = false;
    for (int i = 1; i <= NUM_RELAYS; i++) {
      String key = String(i);
      if (cmd_payload.containsKey(key)) {
        bool state = cmd_payload[key].as<bool>();
        // Direct GPIO relay control (replaces ESP12F's uart_bridge_send_command)
        setRelay(i - 1, state);
        changed = true;
      }
    }

    if (changed) {
      saveRelayStatesToFlash();
      requestTelemetryPublish();
    }

    // Send ACK if msgId is present (same as ESP12F)
    if (doc.containsKey("msgId") && doc["msgId"].is<const char*>()) {
      JsonDocument ackDoc;
      if (doc.containsKey("v"))  ackDoc["v"]  = doc["v"];
      if (doc.containsKey("ts")) ackDoc["ts"] = doc["ts"];
      ackDoc["msgId"] = doc["msgId"];

      JsonObject ackPayload = ackDoc["payload"].to<JsonObject>();
      ackPayload["status"] = "ok";

      char ackBuf[256];
      serializeJson(ackDoc, ackBuf);

      Serial.println("[MQTT] Publishing ACK: " + String(ackBuf));
      mqtt_client.publish(topic_ack.c_str(), (uint8_t *)ackBuf,
                          strlen(ackBuf), false);
    }
  }
}

// ==========================================
// CONNECT MQTT (same as ESP12F mqtt_init_and_start)
// ==========================================
void connectMQTT() {
  if (saved_mqtt_host.length() == 0 || saved_mqtt_port == 0) {
    Serial.println("[MQTT] No MQTT credentials configured. Skipping.");
    return;
  }

  initMQTTTopics();

  if (saved_mqtt_port == 8883 || saved_mqtt_port == 8884) {
    wifiClientSecure.setInsecure();
    mqtt_client.setClient(wifiClientSecure);
  } else {
    mqtt_client.setClient(wifiClient);
  }

  mqtt_client.setBufferSize(1024);
  mqtt_client.setKeepAlive(60);
  mqtt_client.setServer(saved_mqtt_host.c_str(), saved_mqtt_port);
  mqtt_client.setCallback(mqttCallback);

  Serial.printf("[MQTT] Connecting to %s:%d as %s...\n",
                saved_mqtt_host.c_str(), saved_mqtt_port, deviceID.c_str());

  // LWT: same as ESP12F
  char willPayload[] = "{\"status\": \"offline\", \"reason\": \"connection_lost\"}";

  bool connected = false;
  if (saved_mqtt_user.length() > 0) {
    connected = mqtt_client.connect(
        deviceID.c_str(),
        saved_mqtt_user.c_str(),
        saved_mqtt_pass.c_str(),
        topic_lwt.c_str(), 1, true, willPayload);
  } else {
    connected = mqtt_client.connect(
        deviceID.c_str(),
        NULL, NULL,
        topic_lwt.c_str(), 1, true, willPayload);
  }

  if (connected) {
    mqtt_is_connected = true;
    Serial.println("[MQTT] Connected!");

    // Subscribe to command wildcard (same as ESP12F)
    mqtt_client.subscribe(topic_cmd_wildcard.c_str(), 1);
    Serial.println("[MQTT] Subscribed to: " + topic_cmd_wildcard);

    // Publish online status (same as ESP12F)
    mqtt_client.publish(topic_lwt.c_str(),
                        "{\"status\":\"online\"}", true);

    // Publish initial relay state
    publishTelemetry();
    lastHeartbeatTime = millis();
  } else {
    mqtt_is_connected = false;
    Serial.printf("[MQTT] Connection FAILED. State=%d\n",
                  mqtt_client.state());
  }
}

// ==========================================
// WIFI + MQTT CONNECTION HANDLER
// Called every loop() iteration
// Same flow as ESP12F connect_wifi_task + wifi_event_handler
// ==========================================
void handleWiFiConnection() {
  // --- CONNECTED STATE ---
  if (WiFi.status() == WL_CONNECTED) {
    // First time connected (after BLE provisioning)
    if (!wifi_success_notified) {
      Serial.println("[WIFI] Connected! IP: " + WiFi.localIP().toString());
      wifi_success_notified = true;

      // Save credentials to flash (for BLE provisioning path)
      saveCredentials(current_ssid.c_str(), current_pass.c_str(),
                      deviceID.c_str(), saved_mqtt_host.c_str(),
                      saved_mqtt_port, saved_mqtt_user.c_str(),
                      saved_mqtt_pass.c_str());

      // Notify BLE app
      notifyApp("{\"status\": \"connected\", \"id\": \"" + deviceID + "\"}");

      // Connect MQTT
      if (saved_mqtt_host.length() > 0) {
        connectMQTT();
      }
    }

    // MQTT reconnection (if disconnected)
    if (!mqtt_client.connected() && saved_mqtt_host.length() > 0) {
      mqtt_is_connected = false;
      if (millis() - lastMqttRetry > MQTT_RETRY_INTERVAL_MS) {
        lastMqttRetry = millis();
        connectMQTT();
      }
    } else if (mqtt_client.connected()) {
      mqtt_is_connected = true;
      mqtt_client.loop();

      // Debounced telemetry publishing (same as ESP12F telemetry_task)
      if (telemetry_pending &&
          millis() - telemetry_pending_time >= TELEMETRY_DEBOUNCE_MS) {
        telemetry_pending = false;
        publishTelemetry();
      }

      // Heartbeat every 9 seconds (same as ESP12F: 450 * 20ms)
      if (millis() - lastHeartbeatTime > HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatTime = millis();
        publishHeartbeat();
      }
    }
  }
  // --- CONNECTING STATE (triggered by BLE or boot) ---
  else if (trigger_wifi_connection) {
    if (millis() - lastWifiRetry > WIFI_RETRY_INTERVAL_MS) {
      lastWifiRetry = millis();
      WiFi.begin(current_ssid.c_str(), current_pass.c_str());
      wifiRetries++;

      Serial.printf("[WIFI] Connection attempt %d to %s\n",
                    wifiRetries, current_ssid.c_str());

      if (!wifi_success_notified) {
        notifyApp("{\"status\": \"connecting\", \"attempt\": " +
                  String(wifiRetries) + "}");
      }

      // After max retries, fall back to AP mode (same as ESP12F)
      if (wifiRetries >= WIFI_MAX_RETRIES) {
        Serial.println("[WIFI] Max retries reached. Falling back to AP mode.");
        if (!wifi_success_notified) {
          notifyApp("{\"status\": \"failed\"}");
        }
        trigger_wifi_connection = false;
        wifiRetries = 0;
        startAPMode();
        setupBLE();
      }
    }
  }
}
