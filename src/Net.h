#pragma once

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <memory>

// Shared outbound-HTTP helpers for the cloud-sync subsystems (Sync, Backup, RFID-sync). These
// collapse the insecure-TLS client factory + the HTTPClient timeout/redirect/auth setup that were
// previously copy-pasted into each module. The sync server is a LAN/self-hosted endpoint, so https
// uses an insecure TLS client (no bundled CA store), mirroring the GitHub OTA path.

// Creates a plain WiFiClient (http://) or an insecure WiFiClientSecure (https://) matching the URL
// scheme. The handshake timeout is bounded so a stuck connection fails instead of hanging forever.
std::unique_ptr<WiFiClient> Net_MakeClient(const String &url);

// Applies the common connect/read timeouts + strict redirect following + optional HTTP Basic Auth.
// readTimeoutMs defaults to 15000; pass a larger value for slow uploads (the backup upload uses 20000).
void Net_SetupHttp(HTTPClient &http, const String &user, const String &pass, uint32_t readTimeoutMs = 15000);

// Reads the shared sync credentials (syncUser/syncPwd) from NVS into user/pass.
void Net_GetSyncCreds(String &user, String &pass);

// Global single-slot gate for the heap-heavy background network tasks (file Sync, Backup, RFID-sync,
// GitHub OTA + version check). Each spawns an 8-16 KB-stack task plus a TLS client (~40 KB), and the
// board has only ~22 KB free internal DRAM, so two at once can OOM (the mDNS/AsyncTCP allocators then
// abort). A trigger must call Net_TryClaimBgJob() before spawning and only proceed if it returns true;
// the spawned task calls Net_ReleaseBgJob() exactly once when it finishes (and the trigger releases it
// if the task-create itself fails). The cyclic callers (daily backup, sync catch-up, minutely version
// check) simply retry on their next tick when the slot is busy.
bool Net_TryClaimBgJob(void);
void Net_ReleaseBgJob(void);
