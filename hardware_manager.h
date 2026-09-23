#ifndef HARDWARE_MANAGER_H
#define HARDWARE_MANAGER_H

#include "config.h"

extern "C" {
#include "easyflash.h"
extern void bk_reboot(void);
extern void bk_wdg_initialize(uint32_t timeout_ms);
extern void bk_wdg_reload(void);
}

void applyHardwareIdentity();
void saveRelayStatesToFlash();
void loadRelayStatesFromFlash();
void saveCredentials(const char *ssid, const char *pass,
                     const char *deviceId, const char *mqttHost,
                     int mqttPort, const char *mqttUser,
                     const char *mqttPass);
bool loadCredentials();
void clearAllFlash();
void handleResetButton();
void setRelay(int index, bool state);

#endif
