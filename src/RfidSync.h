#pragma once

#include <stdint.h>

// RFID-tag syncing across a central PHP server and other ESPuinos (peer-to-peer), at the same time.
//   - on learn: when a card is newly assigned, push it to the server AND to every configured peer
//   - full:     pull the server list and merge (newest-wins by timestamp), then push every local
//               tag to the server and to all peers
//   - catch-up: once after coming online, a full sync runs automatically so a device that was off
//               (and missed assignments) picks them up from the server
// Wire format matches the server (manifest.php): RFID entries are {id, timestamp, fileOrUrl,
// playMode} for music cards or {id, timestamp, modId} for modification cards.
// Settings (NVS): rfidSyncUrl (server endpoint, e.g. .../manifest.php), reuses syncUser/syncPwd for
// auth, rfidPeers (comma/space separated host[:port] of other ESPuinos), rfidSyncLearn (push on learn).

void RfidSync_Init(void); // open the timestamp NVS namespace (safe to call once at startup)
void RfidSync_Cyclic(void); // drives the one-time catch-up sync after coming online

// Record that a tag was changed locally now (sets its sync timestamp to the current epoch).
void RfidSync_NoteLocalChange(const char *tagId);
uint32_t RfidSync_GetTagTimestamp(const char *tagId);
void RfidSync_SetTagTimestamp(const char *tagId, uint32_t ts);

// Push a single just-learned tag to the server + peers (fire-and-forget). No-op if disabled.
void RfidSync_OnLearn(const char *tagId);

// Start a full bidirectional sync in the background (no-op if one is already running).
void RfidSync_TriggerFull(void);

uint8_t RfidSync_GetStatus(void); // 0 idle, 1 running, 2 done, 3 failed
const char *RfidSync_GetMessage(void);
