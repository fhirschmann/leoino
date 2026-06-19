#pragma once

#include <stddef.h>
#include <stdint.h>

// Auto-backup: assemble the full configuration backup (settings + RFID assignments + per-path EQ
// rules + listening statistics) into a JSON file on the SD card and upload it to a server via HTTP
// POST. Reuses the file-sync server URL/credentials (one server for files, RFID-tags and backups).
//
// The backup is the same JSON shape the web interface's "export backup" button produces, so a file
// uploaded here can be restored through the existing "import backup" path. To stay within the
// ESP32's limited heap (see the chunked /rfid response) the file is written entry-by-entry to SD
// first and then streamed to the server, instead of being held in one big JsonDocument.
//
// Settings (NVS): backupUrl (server endpoint, empty = disabled), backupAuto (daily auto-backup
// on/off). Authentication reuses syncUser/syncPwd. Trigger from the web (Tools tab), command 189 or
// MQTT topic "backup".

void Backup_Init(void); // open the NVS namespace (safe to call once at startup)

// Drives the daily auto-backup (no-op unless enabled, a URL is set, the clock is valid and the day
// has rolled over since the last successful upload). Call cyclically from the main loop.
void Backup_Cyclic(void);

// Start a backup upload in the background. No-op if one is already running. Reusable by the web
// endpoint, the bindable command and MQTT. Credentials are never written to SD/server (passwords
// are stripped from the settings section), matching the web export's "without credentials" mode.
void Backup_Trigger(void);

uint8_t Backup_GetStatus(void); // 0 = idle, 1 = running, 2 = done, 3 = failed
const char *Backup_GetStatusText(void); // "idle" / "running" / "done" / "failed"

// Copies the short human-readable result/error message into the caller-provided buffer. Thread-safe
// (guarded by a spinlock) because the message is written by the backup task and read by the web
// server / MQTT on another core.
void Backup_CopyMessage(char *dst, size_t dstLen);
