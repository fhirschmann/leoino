#pragma once

#include <stdint.h>

// WebDAV server: exposes the SD card as a read/write network drive so a PC/Mac can mount it in
// Finder/Explorer and copy audio files directly, without FTP. Runs in its own FreeRTOS task pinned
// to core 0 (so transfers never disturb the audio pipeline on core 1) on a dedicated port.

constexpr uint8_t webdavUserLength = 24u; // published n-1 as maxlength to the GUI
constexpr uint8_t webdavPasswordLength = 24u; // published n-1 as maxlength to the GUI
constexpr uint16_t webdavPort = 81u; // mgmt UI owns 80, so WebDAV listens on 81 (http://<ip>:81/)

void Webdav_Init(void);
void Webdav_Cyclic(void); // light: only auto-starts the server once WiFi is up; clients run in the task
void Webdav_Exit(void);
void Webdav_ReloadCredentials(void); // re-read user/password from NVS (takes effect on the next request)
void Webdav_EnableServer(void); // start the server task now
void Webdav_DisableServer(void); // stop the server task now
bool Webdav_IsServerRunning(void);
