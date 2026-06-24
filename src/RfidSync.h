#pragma once

#include <stddef.h>
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

// Deletion tombstones (so a delete wins the newest-wins merge and propagates instead of resurrecting).
uint32_t RfidSync_GetDeleteTimestamp(const char *tagId);
void RfidSync_SetDeleteTimestamp(const char *tagId, uint32_t ts);

// Push a single just-learned tag to the server + peers (fire-and-forget). No-op if disabled.
void RfidSync_OnLearn(const char *tagId);
// Record + propagate a local deletion (tombstone) to the server + peers. No-op if disabled.
void RfidSync_OnDelete(const char *tagId);

// Start a full bidirectional sync in the background (no-op if one is already running).
void RfidSync_TriggerFull(void);

uint8_t RfidSync_GetStatus(void); // 0 idle, 1 running, 2 done, 3 failed
// Copy the current status message into a caller buffer under a spinlock (cross-core safe).
void RfidSync_CopyMessage(char *dst, size_t dstLen);

// Serialize an RFID-NVS read-modify-write against the background sync task. Hold across each
// logical RMW unit in the web tag handlers; never hold across an HTTP/network call.
void RfidSync_Lock(void);
void RfidSync_Unlock(void);
