// ==========================================
// BK7238 Smart Home - 4 Channel Relay Controller
// Ported from ESP12F NeuroTouch project
//
// Key difference: BK7238 controls relays directly via GPIO
// (no external MG51 controller, no UART bridge)
//
// Provisioning: AP Mode HTTP + BLE (same endpoints as ESP12F)
// MQTT: Generic broker with username/password
// ==========================================

#include "config.h"
#include "hardware_manager.h"
#include "network_manager.h"

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  bk_wdg_initialize(30000);
  easyflash_init();

  // Generate Device ID from MAC address
  applyHardwareIdentity();

  Serial.println("\n[BOOT] === BK7238 Smart Home Booting ===");
  Serial.printf("[BOOT] Device ID: %s\n", deviceID.c_str());
  Serial.printf("[BOOT] Switch Count: %d\n", SWITCH_COUNT);

  // Beken specific: Disable UART2 interrupts to avoid conflicts
  volatile uint32_t *u_cfg = (volatile uint32_t *)0x00802200;
  *u_cfg = 0;
  volatile uint32_t *uart2_int_en = (volatile uint32_t *)0x00802204;
  *uart2_int_en = 0;

  // Initialize Reset Button
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  // Initialize Relay Pins as OUTPUT
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }

  // Load saved relay states from flash
  loadRelayStatesFromFlash();

  // Load saved WiFi + MQTT credentials
  bool has_credentials = loadCredentials();

  if (has_credentials) {
    Serial.println("[BOOT] Credentials found. Connecting WiFi...");
    trigger_wifi_connection = true;
    wifi_success_notified = true;  // Skip BLE notification on boot
  } else {
    Serial.println("[BOOT] No credentials. Starting AP + BLE provisioning...");
    startAPMode();
    setupBLE();
  }

  Serial.println("[BOOT] === Boot Sequence Complete ===\n");
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  bk_wdg_reload();

  handleResetButton();
  handleWiFiConnection();
  handleHTTPClients();

  delay(5);
}
