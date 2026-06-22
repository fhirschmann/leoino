#pragma once

// HTTP(S) one-way file sync: pulls files listed in a remote JSON manifest
// (served by e.g. nginx) onto the SD card. Additive only — never deletes local
// files. Optional HTTP Basic Auth (user + password stored in NVS).

// Starts a sync in the background. No-op if a sync is already running. Reusable
// by the web endpoint, a bindable command and/or MQTT.
void Sync_Trigger(void);

// Starts a dry run: same diff as a real sync, but nothing is downloaded or deleted. Writes a
// human-readable report of what a real sync would download/delete; retrieve it via the path
// returned by Sync_GetDryReportPath(). No-op if a sync is already running.
void Sync_TriggerDryRun(void);

// Path of the dry-run report file on the SD card (served by the web interface as GET /syncreport).
const char *Sync_GetDryReportPath(void);

// Requests a running sync to stop as soon as possible (cooperative cancel).
void Sync_Cancel(void);

// Current state, polled by the web interface / reported via MQTT.
uint8_t Sync_GetStatus(void); // 0 = idle, 1 = syncing, 2 = done, 3 = failed
uint8_t Sync_GetProgress(void); // percent (files processed / total)
const char *Sync_GetStatusText(void); // "idle" / "syncing" / "done" / "failed"

// Copies the short human-readable result/error message into the caller-provided buffer.
// Thread-safe: the message is written by the sync task on core 1 and read by the web
// server on core 0, so it is guarded by a spinlock to avoid reading a half-written string.
void Sync_CopyMessage(char *dst, size_t dstLen);
