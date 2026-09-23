# Neuro Touch Smart Home (BK7238 Migration)

This project contains the custom firmware designed to migrate the Neuro Touch Smart Home ecosystem from the Espressif ESP12F chipset to the Beken BK7238 chipset.

## Overview
The goal of this project was to replicate the ESP12F firmware exactly so that the **Neuro Touch Mobile App** could seamlessly configure and control the new BK7238 hardware without knowing the difference. 

The firmware includes a highly optimized Captive Portal, an HTTP server for receiving Wi-Fi credentials via a local network, a hardware-level MAC address spoofer to bypass vendor lock-in, and an encrypted MQTT client (MQTTS) for real-time control.

## Key Features

1. **Hardware MAC Spoofing (Vendor Lock-in Bypass):**
   The Neuro Touch mobile app contains security checks that expect an Espressif OUI (`A0:B7:65`). This firmware dynamically overrides the Beken chip's hardware MAC address in RAM before starting the SoftAP. This forces the broadcasted BSSID to perfectly match the Espressif MAC address in the JSON payload, fooling the App's strict security checks.

2. **Custom AP Subnet Fix:**
   The Beken SDK natively hardcodes its SoftAP to `192.168.1.1`. However, the Neuro Touch App blindly sends its HTTP `POST /api/wifi/configure` payload to `192.168.4.1` (the default for ESP32). This firmware intercepts the Beken SDK's initialization routine and forcefully injects `192.168.4.1` into the DHCP server and AP configuration structs before starting the AP.

3. **MQTT over SSL/TLS:**
   The MQTT broker expects a secure TLS connection on port `8883`. Because standard `PubSubClient` relies on raw TCP, the broker was disconnecting the Beken chip. This firmware integrates `WiFiClientSecure` with `.setInsecure()` to bypass certificate validation, ensuring a flawless TLS handshake prior to MQTT connection.

4. **Flash Storage & State Persistence:**
   The device actively persists its state to non-volatile flash memory:
   - Wi-Fi Credentials (SSID, Password)
   - MQTT Configurations (Broker, Port, User, Pass, Device ID)
   - Hardware Relay states are saved immediately upon change, ensuring they restore to their previous state after a power loss.

## Project Structure
* `bk7238_smarthome.ino`: The main Arduino sketch file containing `setup()` and `loop()`.
* `config.h`: Central configuration file for definitions like MQTT topic strings, max lengths, and timeouts.
* `hardware_manager.cpp` / `.h`: Handles GPIO initialization, setting relay states, and hardware-specific logic.
* `network_manager.cpp` / `.h`: The brain of the connectivity. Handles the SoftAP setup, MAC spoofing, HTTP Server for the Captive Portal, Wi-Fi connection, Flash Storage, and MQTT (TLS) connection.

## Compilation & Flashing
This project is built using the **Arduino IDE** with the Beken Core (`beken:bk7238:bk7238`) installed.
1. Open `bk7238_smarthome.ino` in the Arduino IDE.
2. Select your BK7238 board from the Boards Manager.
3. Ensure the libraries `ArduinoJson` and `PubSubClient` are installed.
4. Compile and Upload.

## How it works (Initial Setup Flow)
1. **First Boot:** The device powers on. If no Wi-Fi credentials exist in the flash memory, it overrides its MAC address and starts a SoftAP named `NLX-AP4S-...` on `192.168.4.1`.
2. **App Connection:** The user connects their phone to the AP and opens the Neuro Touch App. The App fetches `GET /` and verifies the JSON payload matches the BSSID.
3. **Configuration:** The App sends `POST /api/wifi/configure` containing the user's home Wi-Fi and MQTT credentials.
4. **Reboot:** The device saves the credentials to flash memory and reboots.
5. **Connection:** On reboot, the device connects to the home Wi-Fi and establishes a secure TLS connection to the MQTT broker, ready to receive commands!
