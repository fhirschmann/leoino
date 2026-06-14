#pragma once

// HTTP(S) one-way file sync: pulls files listed in a remote JSON manifest
// (served by e.g. nginx) onto the SD card. Additive only — never deletes local
// files. Optional HTTP Basic Auth (user + password stored in NVS).

// Starts a sync in the background. No-op if a sync is already running. Reusable
// by the web endpoint, a bindable command and/or MQTT.
void Sync_Trigger(void);

// Requests a running sync to stop as soon as possible (cooperative cancel).
void Sync_Cancel(void);

// Current state, polled by the web interface / reported via MQTT.
uint8_t Sync_GetStatus(void); // 0 = idle, 1 = syncing, 2 = done, 3 = failed
uint8_t Sync_GetProgress(void); // percent (files processed / total)
const char *Sync_GetStatusText(void); // "idle" / "syncing" / "done" / "failed"
const char *Sync_GetMessage(void); // short human-readable result/error message
