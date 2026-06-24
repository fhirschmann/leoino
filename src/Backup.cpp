#include <Arduino.h>
#include "settings.h"

#include "Backup.h"

#include "Log.h"
#include "Net.h"
#include "Playstats.h"
#include "SdCard.h"
#include "StatusMessage.h"
#include "System.h"
#include "WebInternal.h"
#include "Wlan.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClient.h>
#include <memory>
#include <time.h>
#include <vector>

// Temporary on-SD file the backup is streamed into before it is uploaded. Lives next to the RFID
// backup file (/backup.txt) and is removed after a successful upload.
static constexpr const char BACKUP_TMP_FILE[] = "/.backup-upload.json";

// State of the backup upload, polled by the web interface / reported via MQTT.
static volatile uint8_t gBackupStatus = 0; // 0 = idle, 1 = running, 2 = done, 3 = failed

// gBackupMsg is written by the backup task and read by the web server / MQTT on another core;
// StatusMessage's spinlock guards the buffer so a reader never sees a half-written string.
static StatusMessage gBackupMsg;

void Backup_CopyMessage(char *dst, size_t dstLen) {
	gBackupMsg.copy(dst, dstLen);
}

uint8_t Backup_GetStatus(void) {
	return gBackupStatus;
}

const char *Backup_GetStatusText(void) {
	switch (gBackupStatus) {
		case 1:
			return "running";
		case 2:
			return "done";
		case 3:
			return "failed";
		default:
			return "idle";
	}
}

static void backupFail(const char *msg) {
	gBackupMsg.set(msg);
	Log_Printf(LOGLEVEL_ERROR, "Backup failed: %s", msg);
	gBackupStatus = 3;
}

void Backup_Init(void) {
	// Nothing to open: backup settings live in gPrefsSettings (shared with the rest of the web
	// settings) and the last-backup day in gPrefsSettings too. Kept for symmetry with the other
	// subsystems and as a hook for future state.
}

// ---------------------------------------------------------------------------
// Build the backup JSON onto the SD card, entry by entry, to stay within heap.
// ---------------------------------------------------------------------------

// Strip secrets from the settings object so an uploaded backup never leaks credentials in
// cleartext (mirrors the web export's "without credentials" mode). The WiFi sections are not
// emitted by settingsToJSON("") at all, so only the service passwords need clearing.
static void backupStripSecrets(JsonObject settings) {
	if (settings["sync"].is<JsonObject>()) {
		JsonObject s = settings["sync"];
		s["password"] = "";
		s["rfidPeerKey"] = "";
		if (s["rfidPeers"].is<const char *>()) {
			JsonDocument peers;
			if (deserializeJson(peers, s["rfidPeers"].as<const char *>()) == DeserializationError::Ok && peers.is<JsonArray>()) {
				for (JsonObject p : peers.as<JsonArray>()) {
					if (p["key"].is<const char *>()) {
						p["key"] = "";
					}
				}
				String out;
				serializeJson(peers, out);
				s["rfidPeers"] = out;
			}
		}
	}
	if (settings["mqtt"].is<JsonObject>() && settings["mqtt"]["password"].is<const char *>()) {
		settings["mqtt"]["password"] = "";
	}
	if (settings["ftp"].is<JsonObject>() && settings["ftp"]["password"].is<const char *>()) {
		settings["ftp"]["password"] = "";
	}
}

// Writes the complete backup JSON to destFile on SD. Returns true on success. The shape matches the
// web interface's exportBackup() so the file restores through the existing importBackup() path.
static bool backupWriteToSd(const char *destFile) {
	File file = gFSystem.open(destFile, FILE_WRITE);
	if (!file) {
		backupFail("cannot open SD file");
		return false;
	}

	file.print("{\"espuinoBackup\":1");

	// created timestamp (ISO 8601) when the clock is valid
	time_t now = time(nullptr);
	struct tm tmNow;
	if (now > 1700000000 && localtime_r(&now, &tmNow)) {
		char iso[32];
		strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tmNow);
		file.printf(",\"created\":\"%s\"", iso);
	}

	// settings (one bounded JsonDocument), with secrets stripped
	{
		JsonDocument doc;
		JsonObject root = doc.to<JsonObject>();
		settingsToJSON(root, "");
		root.remove("ssids"); // names only - not usable for a restore (matches the web export)
		backupStripSecrets(root);
		file.print(",\"settings\":");
		serializeJson(doc, file);
	}

	// rfid assignments, streamed one tag at a time (the list can be large)
	file.print(",\"rfid\":[");
	{
		std::vector<String> nvsKeys;
		listNVSKeys("rfidTags", &nvsKeys, DumpNvsToArrayCallback);
		bool first = true;
		for (const String &key : nvsKeys) {
			String entry = tagIdToJsonStr(key.c_str(), false);
			if (entry.length() == 0) {
				continue;
			}
			if (!first) {
				file.print(",");
			}
			file.print(entry);
			first = false;
			vTaskDelay(pdMS_TO_TICKS(1)); // yield so the watchdog / audio task stay happy
		}
	}
	file.print("]");

	// per-path equalizer rules (stored as a JSON array string in NVS)
	file.print(",\"eqRules\":");
	file.print(gPrefsSettings.getString("eqRules", "[]"));

	// listening statistics ring buffer
	file.print(",\"playStats\":{\"lastDay\":");
	file.print(Playstats_GetRingLastDay());
	file.print(",\"days\":[");
	{
		const uint16_t n = Playstats_GetRingSize();
		for (uint16_t i = 0; i < n; i++) {
			if (i > 0) {
				file.print(",");
			}
			file.print(Playstats_GetRingSlot(i));
		}
	}
	file.print("]}");

	file.print("}");
	// Detect a failed/partial write (SD full, sector error, card pulled): every print/serializeJson
	// above goes through the File's Print interface, which latches a sticky write-error flag. Without
	// this check the function returned true on a truncated file, which then got uploaded and even
	// suppressed the daily retry — a silently corrupt backup that only surfaces on a failed restore.
	const bool writeOk = (file.getWriteError() == 0);
	file.close();
	if (!writeOk) {
		backupFail("SD write error while building backup");
		gFSystem.remove(destFile); // don't keep a truncated backup around
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Upload the SD backup file to the server.
// ---------------------------------------------------------------------------

// Returns the local calendar day number (days since epoch) or 0 if the clock is not yet valid.
static uint32_t backupCurrentDay(void) {
	time_t now = time(nullptr);
	if (now <= 1700000000) {
		return 0;
	}
	struct tm tmNow;
	if (!localtime_r(&now, &tmNow)) {
		return 0;
	}
	tmNow.tm_hour = 12; // noon, to avoid DST edge effects
	tmNow.tm_min = 0;
	tmNow.tm_sec = 0;
	time_t local = mktime(&tmNow);
	return (uint32_t) (local / 86400);
}

static void backupTask(void *parameter) {
	const String url = gPrefsSettings.getString("backupUrl", "");
	if (url.length() == 0) {
		backupFail("no backup URL configured");
		vTaskDelete(NULL);
		return;
	}
	if (!Wlan_IsConnected()) {
		backupFail("no WiFi");
		vTaskDelete(NULL);
		return;
	}

	gBackupMsg.set("writing backup to SD");
	if (!backupWriteToSd(BACKUP_TMP_FILE)) {
		vTaskDelete(NULL); // backupFail already set the status/message
		return;
	}

	File file = gFSystem.open(BACKUP_TMP_FILE, FILE_READ);
	if (!file) {
		backupFail("cannot reopen backup file");
		vTaskDelete(NULL);
		return;
	}
	const size_t fileSize = file.size();

	gBackupMsg.set("uploading");
	String user, pass;
	Net_GetSyncCreds(user, pass);

	std::unique_ptr<WiFiClient> client = Net_MakeClient(url);
	HTTPClient http;
	Net_SetupHttp(http, user, pass, 20000); // larger read timeout for the (slow) upload
	if (!http.begin(*client, url)) {
		file.close();
		client.reset(); // vTaskDelete() never returns, so unwind the (TLS) client ourselves to avoid a leak
		backupFail("bad URL");
		vTaskDelete(NULL);
		return;
	}
	http.addHeader("Content-Type", "application/json");
	// Hostname so the server can store per-device backups under a stable name.
	http.addHeader("X-ESPuino-Host", Wlan_GetHostname());

	const int code = http.sendRequest("POST", &file, fileSize);
	file.close();
	http.end();
	gFSystem.remove(BACKUP_TMP_FILE);

	if (code == 200 || code == 201 || code == 204) {
		char msg[StatusMessage::Capacity];
		snprintf(msg, sizeof(msg), "uploaded %u bytes (HTTP %d)", (unsigned) fileSize, code);
		gBackupMsg.set(msg);
		gBackupStatus = 2;
		// remember the day so the daily auto-backup doesn't run again until tomorrow
		uint32_t day = backupCurrentDay();
		if (day > 0) {
			gPrefsSettings.putULong("backupLastDay", day);
		}
		Log_Printf(LOGLEVEL_NOTICE, "Backup uploaded (%u bytes, HTTP %d)", (unsigned) fileSize, code);
	} else {
		char msg[StatusMessage::Capacity];
		snprintf(msg, sizeof(msg), "upload failed (HTTP %d)", code);
		backupFail(msg);
	}
	vTaskDelete(NULL);
}

void Backup_Trigger(void) {
	if (gBackupStatus == 1) {
		return; // already running
	}
	gBackupStatus = 1;
	gBackupMsg.set("");
	xTaskCreatePinnedToCore(backupTask, "backup", 16384, NULL, 1, NULL, 1);
}

void Backup_Cyclic(void) {
	if (gBackupStatus == 1) {
		return; // a backup is already running
	}
	// throttle the (cheap) checks below to once a minute
	static uint32_t lastCheck = 0;
	const uint32_t nowMs = millis();
	if (lastCheck != 0 && (nowMs - lastCheck) < 60000) {
		return;
	}
	lastCheck = nowMs;

	if (!gPrefsSettings.getBool("backupAuto", false)) {
		return;
	}
	if (gPrefsSettings.getString("backupUrl", "").length() == 0) {
		return;
	}
	if (!Wlan_IsConnected()) {
		return;
	}
	const uint32_t today = backupCurrentDay();
	if (today == 0) {
		return; // clock not valid yet
	}
	const uint32_t lastDay = gPrefsSettings.getULong("backupLastDay", 0);
	if (today != lastDay) {
		Log_Println("Daily auto-backup: uploading to server", LOGLEVEL_NOTICE);
		Backup_Trigger();
	}
}
