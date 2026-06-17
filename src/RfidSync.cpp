#include <Arduino.h>
#include "settings.h"

#include "RfidSync.h"

#include "Common.h"
#include "Log.h"
#include "System.h"
#include "Wlan.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <vector>

// listNVSKeys is defined in Web.cpp (enumerates all keys of an NVS namespace via a callback).
extern bool listNVSKeys(const char *_namespace, void *data, bool (*callback)(const char *key, void *data));

// Timestamps live in their own NVS namespace (the tag string itself must stay exactly 4 tokens,
// so we can't append a timestamp there). Keyed by the 12-digit tag id.
static Preferences gPrefsRfidTs;
static bool gTsReady = false;

static volatile uint8_t gRfidSyncStatus = 0; // 0 idle, 1 running, 2 done, 3 failed
static char gRfidSyncMsg[96] = "";
static bool gCatchupDone = false; // one-time catch-up sync after coming online

uint8_t RfidSync_GetStatus(void) {
	return gRfidSyncStatus;
}
const char *RfidSync_GetMessage(void) {
	return gRfidSyncMsg;
}
static void rfidSyncSetMsg(const char *msg) {
	snprintf(gRfidSyncMsg, sizeof(gRfidSyncMsg), "%s", msg);
}

void RfidSync_Init(void) {
	if (!gTsReady) {
		gPrefsRfidTs.begin("rfidTagsTs");
		gTsReady = true;
	}
}

static uint32_t rfidNowEpoch(void) {
	time_t now = time(nullptr);
	// ~2020-01-01; below this the clock is not yet NTP/RTC-synced, so report "unknown" (0).
	return (now > 1577836800L) ? (uint32_t) now : 0u;
}

uint32_t RfidSync_GetTagTimestamp(const char *tagId) {
	RfidSync_Init();
	return gPrefsRfidTs.getULong(tagId, 0);
}
void RfidSync_SetTagTimestamp(const char *tagId, uint32_t ts) {
	RfidSync_Init();
	gPrefsRfidTs.putULong(tagId, ts);
}
void RfidSync_NoteLocalChange(const char *tagId) {
	uint32_t ts = rfidNowEpoch();
	if (ts > 0) {
		RfidSync_SetTagTimestamp(tagId, ts);
	}
}

// --- config helpers ---
static bool rfidSyncConfigured(void) {
	return gPrefsSettings.getString("rfidSyncUrl", "").length() > 0 || gPrefsSettings.getString("rfidPeers", "").length() > 0;
}

// Server endpoint for the RFID list/store (a dedicated rfid.php: GET returns the list, POST merges).
static String rfidServerUrl(void) {
	return gPrefsSettings.getString("rfidSyncUrl", "");
}

// --- HTTP helpers (mirrors Sync.cpp) ---
static std::unique_ptr<WiFiClient> rfidMakeClient(const String &url) {
	if (url.startsWith("https://")) {
		auto *secure = new WiFiClientSecure;
		secure->setInsecure(); // no cert pinning on a LAN/self-hosted sync server
		return std::unique_ptr<WiFiClient>(secure);
	}
	return std::unique_ptr<WiFiClient>(new WiFiClient);
}
static void rfidSetupHttp(HTTPClient &http, const String &user, const String &pass) {
	http.setConnectTimeout(8000);
	http.setTimeout(15000);
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	if (user.length() > 0) {
		http.setAuthorization(user.c_str(), pass.c_str());
	}
}

// Parse a tag's NVS string "#fileOrUrl#lastPlayPos#mode#trackLastPlayed" into fileOrUrl + mode.
static bool rfidParseTag(const String &tagId, String &fileOrUrl, uint32_t &mode) {
	String s = gPrefsRfid.getString(tagId.c_str(), "");
	if (s.length() == 0 || s == "-1") {
		return false;
	}
	char buf[512];
	strncpy(buf, s.c_str(), sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	char *tok = strtok(buf, stringDelimiter);
	uint8_t i = 1;
	fileOrUrl = "";
	mode = 0;
	while (tok != NULL) {
		if (i == 1) {
			fileOrUrl = tok;
		} else if (i == 3) {
			mode = strtoul(tok, NULL, 10);
		}
		i++;
		tok = strtok(NULL, stringDelimiter);
	}
	return (mode > 0);
}

// Write an incoming tag into NVS in the canonical 4-token format and stamp its timestamp.
static void rfidWriteTag(const String &tagId, const String &fileOrUrl, uint32_t mode, uint32_t ts) {
	char rfidString[300];
	const char *f = fileOrUrl.length() ? fileOrUrl.c_str() : "0";
	snprintf(rfidString, sizeof(rfidString), "%s%s%s0%s%u%s0", stringDelimiter, f, stringDelimiter, stringDelimiter, (unsigned) mode, stringDelimiter);
	gPrefsRfid.putString(tagId.c_str(), rfidString);
	RfidSync_SetTagTimestamp(tagId.c_str(), ts ? ts : rfidNowEpoch());
}

// Fill a JSON object with one tag in the server's wire format (modId vs playMode + timestamp).
static void rfidTagToJson(JsonObject o, const String &id, const String &fileOrUrl, uint32_t mode, uint32_t ts) {
	o["id"] = id;
	o["timestamp"] = ts;
	if (mode >= 100) {
		o["modId"] = mode;
	} else {
		o["fileOrUrl"] = fileOrUrl;
		o["playMode"] = mode;
	}
}

// Collect all locally stored tags into a JSON array in the server wire format.
static bool rfidCollectCallback(const char *key, void *data) {
	auto *keys = (std::vector<String> *) data;
	keys->push_back(String(key));
	return true;
}
static void rfidCollectLocal(JsonArray arr) {
	std::vector<String> keys;
	listNVSKeys("rfidTags", &keys, rfidCollectCallback);
	for (const String &id : keys) {
		String fileOrUrl;
		uint32_t mode;
		if (!rfidParseTag(id, fileOrUrl, mode)) {
			continue;
		}
		rfidTagToJson(arr.add<JsonObject>(), id, fileOrUrl, mode, RfidSync_GetTagTimestamp(id.c_str()));
	}
}

// POST a JSON body to a URL (optionally with Basic Auth and/or an X-API-Key header).
static int rfidHttpPostJson(const String &url, const String &user, const String &pass, const String &apiKey, const String &body) {
	std::unique_ptr<WiFiClient> client = rfidMakeClient(url);
	HTTPClient http;
	rfidSetupHttp(http, user, pass);
	if (!http.begin(*client, url)) {
		return -1;
	}
	http.addHeader("Content-Type", "application/json");
	if (apiKey.length() > 0) {
		http.addHeader("X-API-Key", apiKey);
	}
	int code = http.POST((uint8_t *) body.c_str(), body.length());
	http.end();
	return code;
}

// A peer ESPuino: base URL + the X-API-Key (its web password) used to authenticate to it.
struct RfidPeer {
	String url;
	String key;
};

// Parse the rfidPeers setting into peers. Entries are comma/space/semicolon separated; each may
// carry its own key as "host|key". The key falls back to the shared "rfidPeerKey" setting and,
// if that is empty too, to this device's own web password (the common "same password" fleet case).
static void rfidGetPeers(std::vector<RfidPeer> &out) {
	const String peers = gPrefsSettings.getString("rfidPeers", "");
	const String sharedKey = gPrefsSettings.getString("rfidPeerKey", "");
	const String fallbackKey = sharedKey.length() > 0 ? sharedKey : gPrefsSettings.getString("wwwPassword", "");
	int start = 0;
	for (int i = 0; i <= (int) peers.length(); i++) {
		char c = (i < (int) peers.length()) ? peers[i] : ',';
		if (c == ',' || c == ';' || c == '\n' || c == '\t' || c == '\r') {
			if (i > start) {
				String token = peers.substring(start, i);
				token.trim();
				if (token.length() > 0) {
					String host = token, key = fallbackKey;
					int bar = token.indexOf('|');
					if (bar >= 0) {
						host = token.substring(0, bar);
						key = token.substring(bar + 1);
						host.trim();
						key.trim();
					}
					if (host.length() > 0) {
						if (!host.startsWith("http://") && !host.startsWith("https://")) {
							host = "http://" + host;
						}
						out.push_back({host, key});
					}
				}
			}
			start = i + 1;
		}
	}
}

// Push a single tag to every configured peer's /rfid endpoint (authenticated with the peer's key).
static void rfidPushTagToPeers(const String &id, const String &fileOrUrl, uint32_t mode, uint32_t ts) {
	std::vector<RfidPeer> peers;
	rfidGetPeers(peers);
	if (peers.empty()) {
		return;
	}
	JsonDocument doc;
	rfidTagToJson(doc.to<JsonObject>(), id, fileOrUrl, mode, ts);
	String body;
	serializeJson(doc, body);
	for (const RfidPeer &peer : peers) {
		int code = rfidHttpPostJson(peer.url + "/rfid", "", "", peer.key, body);
		Log_Printf(LOGLEVEL_NOTICE, "RFID-sync: pushed %s to peer %s (HTTP %d)", id.c_str(), peer.url.c_str(), code);
	}
}

void RfidSync_OnLearn(const char *tagId) {
	if (!gPrefsSettings.getBool("rfidSyncLearn", true)) {
		return;
	}
	String id(tagId);
	String fileOrUrl;
	uint32_t mode;
	if (!rfidParseTag(id, fileOrUrl, mode)) {
		return;
	}
	uint32_t ts = RfidSync_GetTagTimestamp(tagId);

	// Push to the server (single entry; server merges newest-wins by timestamp).
	const String serverUrl = rfidServerUrl();
	if (serverUrl.length() > 0) {
		JsonDocument doc;
		rfidTagToJson(doc.to<JsonObject>(), id, fileOrUrl, mode, ts);
		String body;
		serializeJson(doc, body);
		int code = rfidHttpPostJson(serverUrl, gPrefsSettings.getString("syncUser", ""), gPrefsSettings.getString("syncPwd", ""), "", body);
		Log_Printf(LOGLEVEL_NOTICE, "RFID-sync: pushed %s to server (HTTP %d)", id.c_str(), code);
	}

	// Push to peers (P2P), simultaneously.
	rfidPushTagToPeers(id, fileOrUrl, mode, ts);
}

// Read an entry's mode from the server wire format (modId takes precedence over playMode).
static uint32_t rfidModeFromJson(JsonObject t) {
	if (t["modId"].is<uint32_t>()) {
		return t["modId"].as<uint32_t>();
	}
	return t["playMode"].as<uint32_t>();
}

// --- Full bidirectional sync ---
static void rfidFullSyncTask(void *param) {
	gRfidSyncStatus = 1;
	rfidSyncSetMsg("syncing");

	const String serverUrl = rfidServerUrl();
	uint32_t merged = 0, pushed = 0;

	// 0) Heal offline learns: cards assigned while the clock was invalid (e.g. used outdoors with
	// no WiFi/NTP) have timestamp 0 and would lose every conflict. Now that we are online with a
	// valid clock, stamp them "now" so they count as the freshest and win the merge.
	uint32_t nowTs = rfidNowEpoch();
	if (nowTs > 0) {
		std::vector<String> healKeys;
		listNVSKeys("rfidTags", &healKeys, rfidCollectCallback);
		for (const String &id : healKeys) {
			if (RfidSync_GetTagTimestamp(id.c_str()) == 0 && gPrefsRfid.isKey(id.c_str())) {
				RfidSync_SetTagTimestamp(id.c_str(), nowTs);
			}
		}
	}

	// 1) Pull server tags and merge (newest-wins by timestamp).
	if (serverUrl.length() > 0) {
		std::unique_ptr<WiFiClient> client = rfidMakeClient(serverUrl);
		HTTPClient http;
		rfidSetupHttp(http, gPrefsSettings.getString("syncUser", ""), gPrefsSettings.getString("syncPwd", ""));
		String payload;
		if (http.begin(*client, serverUrl) && http.GET() == 200) {
			payload = http.getString();
		}
		http.end();

		if (payload.length() > 0) {
			JsonDocument doc;
			if (deserializeJson(doc, payload) == DeserializationError::Ok) {
				JsonArray tags = doc["rfid"].is<JsonArray>() ? doc["rfid"].as<JsonArray>() : doc.as<JsonArray>();
				for (JsonObject t : tags) {
					String id = t["id"].as<String>();
					if (id.length() == 0) {
						continue;
					}
					uint32_t inTs = t["timestamp"].as<uint32_t>();
					uint32_t localTs = RfidSync_GetTagTimestamp(id.c_str());
					bool localExists = gPrefsRfid.isKey(id.c_str());
					if (!localExists || inTs > localTs) {
						rfidWriteTag(id, t["fileOrUrl"].as<String>(), rfidModeFromJson(t), inTs);
						merged++;
					}
				}
			}
		}
	}

	// 2) Push all local tags to the server (server merges newest-wins).
	if (serverUrl.length() > 0) {
		JsonDocument doc;
		JsonArray arr = doc["rfid"].to<JsonArray>();
		rfidCollectLocal(arr);
		pushed = arr.size();
		String body;
		serializeJson(doc, body);
		rfidHttpPostJson(serverUrl, gPrefsSettings.getString("syncUser", ""), gPrefsSettings.getString("syncPwd", ""), "", body);
	}

	// 3) Push all local tags to peers (P2P).
	{
		std::vector<RfidPeer> peers;
		rfidGetPeers(peers);
		if (!peers.empty()) {
			std::vector<String> keys;
			listNVSKeys("rfidTags", &keys, rfidCollectCallback);
			for (const String &id : keys) {
				String fileOrUrl;
				uint32_t mode;
				if (rfidParseTag(id, fileOrUrl, mode)) {
					rfidPushTagToPeers(id, fileOrUrl, mode, RfidSync_GetTagTimestamp(id.c_str()));
				}
			}
		}
	}

	char done[96];
	snprintf(done, sizeof(done), "merged %u, pushed %u", (unsigned) merged, (unsigned) pushed);
	rfidSyncSetMsg(done);
	gRfidSyncStatus = 2;
	Log_Printf(LOGLEVEL_NOTICE, "RFID-sync full done: %s", done);
	vTaskDelete(NULL);
}

void RfidSync_TriggerFull(void) {
	if (gRfidSyncStatus == 1) {
		return; // already running
	}
	RfidSync_Init();
	xTaskCreatePinnedToCore(rfidFullSyncTask, "rfidSync", 16384, NULL, 1, NULL, 1);
}

// One-time catch-up: shortly after the device is online (WiFi up + clock valid), run a full sync so
// it picks up any assignments it missed while it was off. Driven from the main loop.
void RfidSync_Cyclic(void) {
	if (gCatchupDone) {
		return;
	}
	if (!Wlan_IsConnected() || rfidNowEpoch() == 0 || !rfidSyncConfigured()) {
		return;
	}
	gCatchupDone = true;
	Log_Println("RFID-sync: running catch-up sync after coming online", LOGLEVEL_NOTICE);
	RfidSync_TriggerFull();
}
