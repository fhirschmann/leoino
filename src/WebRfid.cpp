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
	uint32_t _mode = 1;

	char s_buf[512];
	strncpy(s_buf, s.c_str(), sizeof(s_buf) - 1);
	s_buf[sizeof(s_buf) - 1] = '\0';

	char *token = strtok(s_buf, stringDelimiter);
	uint8_t i = 1;
	while (token != NULL) { // Try to extract data from string after lookup
		if (i == 1) {
			strncpy(_file, token, sizeof(_file) - 1);
		} else if (i == 2) {
			_lastPlayPos = strtoul(token, NULL, 10);
		} else if (i == 3) {
			_mode = strtoul(token, NULL, 10);
		} else if (i == 4) {
			_trackLastPlayed = strtoul(token, NULL, 10);
		}
		i++;
		token = strtok(NULL, stringDelimiter);
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
	char buf[512];
	strncpy(buf, s.c_str(), sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	uint32_t mode = 0;
	uint8_t i = 1;
	for (char *token = strtok(buf, stringDelimiter); token != NULL; token = strtok(NULL, stringDelimiter), i++) {
		if (i == 3) {
			mode = strtoul(token, NULL, 10);
		}
	}
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

	if ((tagId != "") && gPrefsRfid.isKey(tagId.c_str())) {
		// return single RFID entry with details
		String json = tagIdToJsonStr(tagId.c_str(), false);
		request->send(200, "application/json", json);
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
		[nvsKeys = std::move(nvsKeys), idsOnly, nvsIndex = size_t(0)](uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t {
			maxLen = maxLen >> 1; // some sort of bug with actual size available, reduce the len
			size_t len = 0;
			String json;

			if (nvsIndex == 0) {
				// start, write first tag
				json = tagIdToJsonStr(nvsKeys[nvsIndex].c_str(), idsOnly);
				if (json.length() >= maxLen) {
					Log_Println("/rfid: Buffer too small", LOGLEVEL_ERROR);
					return len;
				}
				len += snprintf(((char *) buffer), maxLen - len, "[%s", json.c_str());
				nvsIndex++;
			}
			while (nvsIndex < nvsKeys.size()) {
				// write tags as long we have enough room
				json = tagIdToJsonStr(nvsKeys[nvsIndex].c_str(), idsOnly);
				if ((len + json.length()) >= maxLen) {
					break;
				}
				len += snprintf(((char *) buffer + len), maxLen - len, ",%s", json.c_str());
				nvsIndex++;
			}
			if (nvsIndex == nvsKeys.size()) {
				// finish
				len += snprintf(((char *) buffer + len), maxLen - len, "]");
				nvsIndex++;
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
		uint32_t localAssign = RfidSync_GetTagTimestamp(tagId.c_str());
		uint32_t localDel = RfidSync_GetDeleteTimestamp(tagId.c_str());
		uint32_t localNewest = (localAssign > localDel) ? localAssign : localDel;
		if (inTs == 0 || inTs > localNewest) {
			if (gPrefsRfid.isKey(tagId.c_str())) {
				gPrefsRfid.remove(tagId.c_str());
			}
			RfidSync_SetTagTimestamp(tagId.c_str(), 0);
			RfidSync_SetDeleteTimestamp(tagId.c_str(), inTs);
			Web_DumpNvsToSd("rfidTags", backupFile);
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
	if (jsonObj["modId"].is<u_int8_t>()) {
		_playModeOrModId = jsonObj["modId"];
	} else {
		_playModeOrModId = jsonObj["playMode"];
	}
	if (_playModeOrModId <= 0) {
		Log_Println("/rfid (POST): Invalid playMode or modId", LOGLEVEL_ERROR);
		request->send(500, "text/plain; charset=utf-8", "/rfid (POST): Invalid playMode or modId");
		return;
	}
	char rfidString[275];
	snprintf(rfidString, sizeof(rfidString) / sizeof(rfidString[0]), "%s%s%s0%s%u%s0", stringDelimiter, _fileOrUrlAscii, stringDelimiter, stringDelimiter, _playModeOrModId, stringDelimiter);
	gPrefsRfid.putString(tagId.c_str(), rfidString);

	String s = gPrefsRfid.getString(tagId.c_str(), "-1");
	if (s.compareTo(rfidString)) {
		request->send(500, "text/plain; charset=utf-8", "/rfid (POST): cannot save assignment to NVS");
		return;
	}
	// Record the sync timestamp: use an incoming "timestamp" if provided (a peer push preserves the
	// origin timestamp), else stamp now. This endpoint is the peer-push target, so it must NOT
	// re-push (no RfidSync_OnLearn here) to avoid sync loops between devices.
	if (jsonObj["timestamp"].is<uint32_t>() && jsonObj["timestamp"].as<uint32_t>() > 0) {
		RfidSync_SetTagTimestamp(tagId.c_str(), jsonObj["timestamp"].as<uint32_t>());
	} else {
		RfidSync_NoteLocalChange(tagId.c_str());
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
		if (gPrefsRfid.remove(tagId.c_str())) {
			RfidSync_OnDelete(tagId.c_str()); // record tombstone + propagate the deletion to server/peers
			Playstats_ClearCardPlays(tagId.c_str()); // drop the card's play counter too
			Playstats_ClearCardSeen(tagId.c_str()); // and its last-seen timestamp
			Log_Printf(LOGLEVEL_INFO, "/rfid (DELETE): tag %s removed successfuly", tagId);
			request->send(200, "text/plain; charset=utf-8", tagId + " removed successfuly");
		} else {
			Log_Println("/rfid (DELETE):error removing tag from NVS", LOGLEVEL_ERROR);
			request->send(500, "text/plain; charset=utf-8", "error removing tag from NVS");
		}
	} else {
		Log_Printf(LOGLEVEL_DEBUG, "/rfid (DELETE): tag %s not exists", tagId);
		request->send(404, "text/plain; charset=utf-8", "error removing tag from NVS: Tag not exists");
	}
}
