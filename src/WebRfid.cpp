#include <Arduino.h>
#include "settings.h"

#include "ArduinoJson.h"
#include "AsyncJson.h"
#include "AudioPlayer.h"
#include "Cmd.h"
#include "Common.h"
#include "ESPAsyncWebServer.h"
#include "Log.h"
#include "Playstats.h"
#include "Rfid.h"
#include "RfidSync.h"
#include "System.h"
#include "Web.h"
#include "WebInternal.h"

#include <vector>

// RFID tag-assignment endpoints, split out of Web.cpp. The route table in
// Web.cpp::webserverStart registers the handlers declared in WebInternal.h; this file owns the
// NVS <-> JSON (de)serialisation of tag assignments and the GET/POST/DELETE + reset-position
// handlers. It only talks back to the rest of the web server through helpers declared in
// WebInternal.h (listNVSKeys / DumpNvsToArrayCallback / Web_DumpNvsToSd).

// Serialise a single tag assignment from NVS into a JSON object. The stored value has the form
// "#<file/folder>#<playPos>#<playMode>#<trackLastPlayed>"; a play mode >= 100 is a modification card.
static bool tagIdToJSON(const String tagId, JsonObject entry) {
	String s = gPrefsRfid.getString(tagId.c_str(), ""); // Try to lookup rfidId in NVS
	if (s.length() == 0 || s == "-1") {
		return false;
	}
	char _file[256] = {0};
	uint32_t _lastPlayPos = 0;
	uint16_t _trackLastPlayed = 0;
	uint32_t _mode = 0;
	if (!Rfid_ParseAssignment(s.c_str(), _file, sizeof(_file), &_lastPlayPos, &_mode, &_trackLastPlayed)) {
		return false;
	}
	entry["id"] = tagId;
	if (_mode >= 100) {
		entry["modId"] = _mode;
	} else {
		entry["fileOrUrl"] = _file;
		entry["playMode"] = _mode;
		entry["lastPlayPos"] = _lastPlayPos;
		entry["trackLastPlayed"] = _trackLastPlayed;
	}
	return true;
}

String tagIdToJsonStr(const char *key, const bool nameOnly) {
	if (nameOnly) {
		return "\"" + String(key) + "\"";
	} else {
		JsonDocument doc;
		JsonObject entry = doc[key].to<JsonObject>();
		if (!tagIdToJSON(key, entry)) {
			return "";
		}
		String serializedJsonString;
		serializeJson(entry, serializedJsonString);
		return serializedJsonString;
	}
}

// Resets an audiobook tag's saved play-position (and last-played track) back to the start,
// so a finished or abandoned book can be restarted from the beginning without re-assigning
// the card. Preserves the folder/file and play mode. POST /rfidresetpos?id=<tagId>.
void handleResetRfidPos(AsyncWebServerRequest *request) {
	if (!request->hasParam("id")) {
		request->send(400, "application/json", "{\"error\":\"missing id\"}");
		return;
	}
	const String tagId = request->getParam("id")->value();
	String s = gPrefsRfid.getString(tagId.c_str(), "");
	if (s.length() == 0 || s == "-1") {
		request->send(404, "application/json", "{\"error\":\"unknown tag\"}");
		return;
	}
	// stored format: #<file/folder>#<playPos>#<playMode>#<trackLastPlayed>; extract the mode
	uint32_t mode = 0;
	Rfid_ParseAssignment(s.c_str(), NULL, 0, NULL, &mode, NULL);
	if (mode == 0 || mode >= 100) {
		// NO_PLAYLIST or a modification-card -- there is no play-position to reset
		request->send(409, "application/json", "{\"error\":\"not a playlist tag\"}");
		return;
	}
	AudioPlayer_ResetRfidPos(tagId.c_str(), (uint8_t) mode);
	request->send(200, "application/json", "{}");
}

// Handles rfid-assignments requests (GET)
// /rfid returns an array of tag-ids and details. Optional GET param "id" to list only a single assignment.
// /rfid/ids-only returns an array of tag-id keys
void handleGetRFIDRequest(AsyncWebServerRequest *request) {

	String tagId = "";

	if (request->hasParam("id")) {
		tagId = request->getParam("id")->value();
	}

	if (tagId != "") {
		// A requested ID is a lookup, not a filter fallback. Returning every assignment for an unknown
		// ID was surprising to API callers and exposed the complete list after a simple typo.
		if (gPrefsRfid.isKey(tagId.c_str())) {
			String json = tagIdToJsonStr(tagId.c_str(), false);
			if (json.length() > 0) {
				request->send(200, "application/json", json);
				return;
			}
		}
		request->send(404, "application/json", "{\"error\":\"unknown tag\"}");
		return;
	}
	// get tag details or just an array of id's
	bool idsOnly = request->hasParam("ids-only");

	std::vector<String> nvsKeys {};
	nvsKeys.clear();
	// Dumps all RFID-keys from NVS into key array
	listNVSKeys("rfidTags", &nvsKeys, DumpNvsToArrayCallback);
	if (nvsKeys.size() == 0) {
		// no entries
		request->send(200, "application/json", "[]");
		return;
	}
	// construct chunked repsonse
	AsyncWebServerResponse *response = request->beginChunkedResponse("application/json",
		[nvsKeys = std::move(nvsKeys), idsOnly, nvsIndex = size_t(0), started = false, sentAny = false, finished = false](uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t {
			(void) index;
			if (finished) {
				return 0;
			}
			maxLen = maxLen >> 1; // some sort of bug with actual size available, reduce the len
			size_t len = 0;
			if (!started) {
				buffer[len++] = '[';
				started = true;
			}
			while (nvsIndex < nvsKeys.size()) {
				// A tag can disappear after the key snapshot was taken. Skip it instead of emitting an empty
				// element ("[,"), which made the whole chunked response invalid JSON during concurrent deletes.
				String json = tagIdToJsonStr(nvsKeys[nvsIndex].c_str(), idsOnly);
				if (json.length() == 0) {
					nvsIndex++;
					continue;
				}
				const size_t needed = json.length() + (sentAny ? 1u : 0u);
				if (needed > maxLen) {
					Log_Println("/rfid: entry exceeds response buffer", LOGLEVEL_ERROR);
					nvsIndex++; // preserve valid JSON even if a corrupt/oversized NVS entry is encountered
					continue;
				}
				if (len + needed > maxLen) {
					break;
				}
				if (sentAny) {
					buffer[len++] = ',';
				}
				memcpy(buffer + len, json.c_str(), json.length());
				len += json.length();
				sentAny = true;
				nvsIndex++;
			}
			if (nvsIndex == nvsKeys.size() && len < maxLen) {
				buffer[len++] = ']';
				finished = true;
			}
			return len;
		});
	request->send(response);
}

void handlePostRFIDRequest(AsyncWebServerRequest *request, JsonVariant &json) {
	const JsonObject &jsonObj = json.as<JsonObject>();

	String tagId = jsonObj["id"];
	if (tagId.isEmpty()) {
		Log_Println("/rfid (POST): Missing tag id", LOGLEVEL_ERROR);
		request->send(500, "text/plain; charset=utf-8", "/rfid (POST): Missing tag id");
		return;
	}
	// Incoming deletion tombstone from a peer/server: drop the tag locally if the deletion is newer
	// than what we have, record the tombstone, and do NOT re-push (avoids sync loops).
	if (jsonObj["deleted"].is<bool>() && jsonObj["deleted"].as<bool>()) {
		uint32_t inTs = jsonObj["timestamp"].is<uint32_t>() ? jsonObj["timestamp"].as<uint32_t>() : 0;
		// serialize the compare-then-write against a concurrent full-sync merge (cross-core RMW)
		RfidSync_Lock();
		uint32_t localAssign = RfidSync_GetTagTimestamp(tagId.c_str());
		uint32_t localDel = RfidSync_GetDeleteTimestamp(tagId.c_str());
		uint32_t localNewest = (localAssign > localDel) ? localAssign : localDel;
		if (inTs == 0 || inTs > localNewest) {
			if (gPrefsRfid.isKey(tagId.c_str())) {
				gPrefsRfid.remove(tagId.c_str());
			}
			RfidSync_SetDeleteTimestamp(tagId.c_str(), inTs);
			RfidSync_Unlock();
			Web_DumpNvsToSd("rfidTags", backupFile);
		} else {
			RfidSync_Unlock();
		}
		request->send(200, "text/plain; charset=utf-8", "ok");
		return;
	}
	String fileOrUrl = jsonObj["fileOrUrl"];
	if (fileOrUrl.isEmpty()) {
		fileOrUrl = "0";
	}
	const char *_fileOrUrlAscii = fileOrUrl.c_str();
	uint8_t _playModeOrModId;
	bool isModId;
	if (jsonObj["modId"].is<u_int8_t>()) {
		_playModeOrModId = jsonObj["modId"];
		isModId = true;
	} else {
		_playModeOrModId = jsonObj["playMode"];
		isModId = false;
	}
	if (_playModeOrModId <= 0) {
		Log_Println("/rfid (POST): Invalid playMode or modId", LOGLEVEL_ERROR);
		request->send(500, "text/plain; charset=utf-8", "/rfid (POST): Invalid playMode or modId");
		return;
	}
	// A modification-card (modId) has no resume position / last track; keep those 0. For a normal
	// file/URL assignment (playMode) preserve the resume position and last track a backup-restore /
	// import / peer-sync push sends (absent -> 0, matching the previous re-assign behavior).
	uint32_t pos = isModId ? 0 : (jsonObj["lastPlayPos"] | 0);
	uint16_t track = isModId ? 0 : (jsonObj["trackLastPlayed"] | 0);
	char rfidString[275];
	const int rfidLen = snprintf(rfidString, sizeof(rfidString), "%s%s%s%lu%s%u%s%u", stringDelimiter, _fileOrUrlAscii, stringDelimiter, (unsigned long) pos, stringDelimiter, _playModeOrModId, stringDelimiter, track);
	if (rfidLen < 0 || static_cast<size_t>(rfidLen) >= sizeof(rfidString)) {
		request->send(400, "application/json", "{\"error\":\"assignment too long\"}");
		return;
	}
	// serialize the write + timestamp against a concurrent full-sync merge (cross-core RMW)
	RfidSync_Lock();
	const uint32_t incomingTs = jsonObj["timestamp"].is<uint32_t>() ? jsonObj["timestamp"].as<uint32_t>() : 0;
	const uint32_t localAssign = RfidSync_GetTagTimestamp(tagId.c_str());
	const uint32_t localDelete = RfidSync_GetDeleteTimestamp(tagId.c_str());
	const uint32_t localNewest = (localAssign > localDelete) ? localAssign : localDelete;
	if (incomingTs > 0 && incomingTs <= localNewest) {
		// Peer pushes carry their source timestamp. Ignore stale/equal assignments so an offline peer
		// cannot overwrite a newer local edit or resurrect a tag protected by a newer tombstone.
		RfidSync_Unlock();
		request->send(200, "text/plain; charset=utf-8", "ok");
		return;
	}
	gPrefsRfid.putString(tagId.c_str(), rfidString);
	String s = gPrefsRfid.getString(tagId.c_str(), "-1");
	const bool saveOk = (s.compareTo(rfidString) == 0);
	if (saveOk) {
		// Record the sync timestamp: use an incoming "timestamp" if provided (a peer push preserves
		// the origin timestamp), else stamp now. This endpoint is the peer-push target, so it must
		// NOT re-push (no RfidSync_OnLearn here) to avoid sync loops between devices.
		if (incomingTs > 0) {
			RfidSync_SetTagTimestamp(tagId.c_str(), incomingTs);
			RfidSync_ClearDeleteTimestamp(tagId.c_str());
		} else {
			RfidSync_NoteLocalChange(tagId.c_str());
		}
	}
	RfidSync_Unlock();
	if (!saveOk) {
		request->send(500, "text/plain; charset=utf-8", "/rfid (POST): cannot save assignment to NVS");
		return;
	}
	Web_DumpNvsToSd("rfidTags", backupFile); // Store backup-file every time when a new rfid-tag is programmed
	// return the new/modified RFID assignment
	AsyncJsonResponse *response = new AsyncJsonResponse(false);
	JsonObject obj = response->getRoot();
	tagIdToJSON(tagId, obj);
	response->setLength();
	request->send(response);
}

void handleDeleteRFIDRequest(AsyncWebServerRequest *request) {
	String tagId = "";
	if (request->hasParam("id")) {
		tagId = request->getParam("id")->value();
	}
	if (tagId.isEmpty()) {
		Log_Println("/rfid (DELETE): Missing tag id", LOGLEVEL_ERROR);
		request->send(500, "text/plain; charset=utf-8", "/rfid (DELETE): Missing tag id");
		return;
	}
	if (gPrefsRfid.isKey(tagId.c_str())) {
		if (tagId.equals(gCurrentRfidTagId)) {
			// stop playback, tag to delete is in use
			Cmd_Action(CMD_STOP);
		}
		// OnDelete removes the tag and writes its deletion tombstone atomically (single lock), then propagates.
		const bool removed = RfidSync_OnDelete(tagId.c_str());
		if (removed) {
			Playstats_ClearCardPlays(tagId.c_str()); // drop the card's play counter too
			Playstats_ClearCardSeen(tagId.c_str()); // and its last-seen timestamp
			Log_Printf(LOGLEVEL_INFO, "/rfid (DELETE): tag %s removed successfuly", tagId.c_str());
			request->send(200, "text/plain; charset=utf-8", tagId + " removed successfuly");
		} else {
			Log_Println("/rfid (DELETE):error removing tag from NVS", LOGLEVEL_ERROR);
			request->send(500, "text/plain; charset=utf-8", "error removing tag from NVS");
		}
	} else {
		Log_Printf(LOGLEVEL_DEBUG, "/rfid (DELETE): tag %s not exists", tagId.c_str());
		request->send(404, "text/plain; charset=utf-8", "error removing tag from NVS: Tag not exists");
	}
}
