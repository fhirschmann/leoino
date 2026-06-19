#include <Arduino.h>
#include "settings.h"

#include "Sync.h"

#include "AudioPlayer.h"
#include "Led.h"
#include "Log.h"
#include "SdCard.h"
#include "System.h"
#include "Wlan.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <memory>
#include <stdarg.h>

// Abort a single file download if no data arrives for this long (connection still
// "open" but stalled), so one bad/slow file can't hang the whole sync forever.
static constexpr uint32_t SYNC_STALL_TIMEOUT_MS = 20000;

// State of the HTTP sync, polled by the web interface via GET /sync.
// 0 = idle, 1 = running, 2 = done, 3 = failed, 4 = stopped (cancelled by user)
static volatile uint8_t gSyncStatus = 0;
static volatile uint8_t gSyncProgress = 0; // percent (files processed / total)
static volatile bool gSyncCancel = false; // cooperative cancel flag

// gSyncMsg is written by the sync task (core 1) and read by the web server (core 0).
// A short spinlock guards the buffer so the web server can never read a half-written
// string (which would otherwise show up as a brief garbage line in the sync progress UI).
static char gSyncMsg[96] = "";
static portMUX_TYPE gSyncMsgMux = portMUX_INITIALIZER_UNLOCKED;

// Thread-safe setter: format into a stack buffer first, then copy under the lock so the
// critical section stays as short as possible (a single memcpy, no formatting/IO).
static void syncSetMessage(const char *msg) {
	char tmp[sizeof(gSyncMsg)];
	snprintf(tmp, sizeof(tmp), "%s", msg ? msg : "");
	taskENTER_CRITICAL(&gSyncMsgMux);
	memcpy(gSyncMsg, tmp, sizeof(gSyncMsg));
	taskEXIT_CRITICAL(&gSyncMsgMux);
}

static void syncSetMessagef(const char *fmt, ...) {
	char tmp[sizeof(gSyncMsg)];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	taskENTER_CRITICAL(&gSyncMsgMux);
	memcpy(gSyncMsg, tmp, sizeof(gSyncMsg));
	taskEXIT_CRITICAL(&gSyncMsgMux);
}

uint8_t Sync_GetStatus(void) {
	return gSyncStatus;
}

uint8_t Sync_GetProgress(void) {
	return gSyncProgress;
}

const char *Sync_GetStatusText(void) {
	switch (gSyncStatus) {
		case 1:
			return "syncing";
		case 2:
			return "done";
		case 3:
			return "failed";
		case 4:
			return "stopped";
		default:
			return "idle";
	}
}

void Sync_Cancel(void) {
	if (gSyncStatus == 1) {
		gSyncCancel = true;
	}
}

void Sync_CopyMessage(char *dst, size_t dstLen) {
	if (!dst || dstLen == 0) {
		return;
	}
	taskENTER_CRITICAL(&gSyncMsgMux);
	size_t n = strnlen(gSyncMsg, sizeof(gSyncMsg) - 1);
	if (n >= dstLen) {
		n = dstLen - 1;
	}
	memcpy(dst, gSyncMsg, n);
	taskEXIT_CRITICAL(&gSyncMsgMux);
	dst[n] = '\0';
}

static void syncFail(const char *msg) {
	syncSetMessage(msg);
	Log_Printf(LOGLEVEL_ERROR, "Sync failed: %s", msg);
	gSyncStatus = 3;
}

// Creates a HTTP(S) client matching the URL scheme. https uses an insecure TLS
// client (no bundled CA store), mirroring the GitHub OTA path.
static std::unique_ptr<WiFiClient> syncMakeClient(const String &url) {
	if (url.startsWith("https://")) {
		auto *secure = new WiFiClientSecure;
		secure->setInsecure();
		secure->setHandshakeTimeout(20);
		return std::unique_ptr<WiFiClient>(secure);
	}
	return std::unique_ptr<WiFiClient>(new WiFiClient);
}

static void syncSetupHttp(HTTPClient &http, const String &user, const String &pass) {
	http.setConnectTimeout(8000);
	http.setTimeout(15000);
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	if (user.length() > 0) {
		http.setAuthorization(user.c_str(), pass.c_str()); // HTTP Basic Auth
	}
}

// Percent-encodes a path for use in a URL, leaving '/' and unreserved characters intact.
static String syncUrlEncodePath(const String &path) {
	static const char *hex = "0123456789ABCDEF";
	String out;
	out.reserve(path.length() * 2);
	for (size_t i = 0; i < path.length(); i++) {
		const char c = path[i];
		if (isalnum((unsigned char) c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
			out += c;
		} else {
			out += '%';
			out += hex[(c >> 4) & 0xF];
			out += hex[c & 0xF];
		}
	}
	return out;
}

// Creates every missing parent directory of a "/dir/sub/file" path on the SD card.
static void syncEnsureParentDirs(const String &path) {
	int slash = path.indexOf('/', 1);
	while (slash > 0) {
		const String dir = path.substring(0, slash);
		if (dir.length() > 0 && !gFSystem.exists(dir)) {
			gFSystem.mkdir(dir);
		}
		slash = path.indexOf('/', slash + 1);
	}
}

// Downloads a single file to the SD card, streaming in small chunks so the whole
// file never has to fit in RAM. Returns true only on a complete download.
static bool syncDownloadFile(const String &url, const String &user, const String &pass, const String &localPath) {
	std::unique_ptr<WiFiClient> client = syncMakeClient(url);
	HTTPClient http;
	syncSetupHttp(http, user, pass);
	if (!http.begin(*client, url)) {
		return false;
	}
	const int code = http.GET();
	if (code != 200) {
		http.end();
		return false;
	}

	syncEnsureParentDirs(localPath);
	File file = gFSystem.open(localPath, "w", true);
	if (!file) {
		http.end();
		return false;
	}

	int remaining = http.getSize(); // -1 if the server didn't send a length
	uint8_t buf[1024];
	WiFiClient *stream = http.getStreamPtr();
	bool ok = true;
	uint32_t lastDataMs = millis(); // for the stall watchdog below
	while (http.connected() && (remaining > 0 || remaining == -1)) {
		if (gSyncCancel) { // user pressed stop
			ok = false;
			break;
		}
		const size_t avail = stream->available();
		if (avail) {
			const int read = stream->readBytes(buf, (avail > sizeof(buf)) ? sizeof(buf) : avail);
			if (file.write(buf, read) != (size_t) read) {
				ok = false;
				break;
			}
			if (remaining > 0) {
				remaining -= read;
			}
			lastDataMs = millis();
		} else {
			// abort a stalled download (connection still "open" but no data) so one
			// bad/slow file can't hang the whole sync forever; it is marked failed and
			// the sync moves on. A slow-but-flowing download keeps resetting the timer.
			if (millis() - lastDataMs > SYNC_STALL_TIMEOUT_MS) {
				Log_Println("Sync: download stalled, aborting file", LOGLEVEL_ERROR);
				ok = false;
				break;
			}
			vTaskDelay(pdMS_TO_TICKS(1)); // yield while waiting for more data
		}
	}
	file.close();
	http.end();
	// if the download was aborted or a known length wasn't fully received, drop the
	// partial file so a later sync re-fetches it (the size mismatch triggers a redownload)
	const bool complete = ok && (remaining <= 0);
	if (!complete) {
		gFSystem.remove(localPath);
	}
	return complete;
}

static void syncTask(void *parameter) {
	gSyncProgress = 0;
	syncSetMessage("");

	if (!Wlan_IsConnected()) {
		syncFail("no WiFi connection");
		vTaskDelete(NULL);
		return;
	}

	const String manifestUrl = gPrefsSettings.getString("syncUrl", "");
	if (manifestUrl.length() == 0) {
		syncFail("no sync URL configured");
		vTaskDelete(NULL);
		return;
	}
	const String user = gPrefsSettings.getString("syncUser", "");
	const String pass = gPrefsSettings.getString("syncPwd", "");

	// fetch + parse the manifest. The JSON is streamed straight from the network into
	// the parser (deserializeJson over the WiFiClient stream) instead of first buffering
	// the whole payload in a String — this roughly halves peak RAM during the parse, which
	// matters for large manifests (hundreds of file entries) on the heap-constrained ESP32.
	// Download the manifest to a temp file on SD, then parse it one entry at a time.
	// Loading the whole manifest into a JsonDocument fails on two counts: a large
	// manifest (this fork syncs 1000+ files) needs far more than the ~90 kB of free
	// heap available at runtime (NoMemory -> "parse error"), and the PHP-generated
	// manifest is sent "Transfer-Encoding: chunked", whose chunk framing corrupts a
	// parse that reads the raw socket. writeToStream() decodes the chunking for us;
	// streaming the entries back off SD keeps only one file object in RAM at a time.
	const char *manifestTmp = "/.sync_manifest.json";
	{
		std::unique_ptr<WiFiClient> client = syncMakeClient(manifestUrl);
		HTTPClient http;
		syncSetupHttp(http, user, pass);
		if (!http.begin(*client, manifestUrl)) {
			syncFail("manifest connection failed");
			vTaskDelete(NULL);
			return;
		}
		const int code = http.GET();
		if (code != 200) {
			char msg[64];
			snprintf(msg, sizeof(msg), "manifest HTTP %d", code);
			syncFail(msg);
			http.end();
			vTaskDelete(NULL);
			return;
		}
		File mf = gFSystem.open(manifestTmp, "w", true);
		if (!mf) {
			syncFail("manifest: cannot buffer to SD");
			http.end();
			vTaskDelete(NULL);
			return;
		}
		http.writeToStream(&mf); // decodes chunked transfer-encoding, minimal RAM
		mf.close();
		http.end();
	}

	// Re-open the buffered manifest and seek to the start of the "files" array; from
	// here entries are deserialized one object at a time.
	File manifest = gFSystem.open(manifestTmp, "r");
	if (!manifest) {
		syncFail("manifest: reopen failed");
		gFSystem.remove(manifestTmp);
		vTaskDelete(NULL);
		return;
	}
	const size_t manifestBytes = manifest.size();
	if (!manifest.find("\"files\"") || !manifest.find('[')) {
		syncFail("manifest has no \"files\" array");
		manifest.close();
		gFSystem.remove(manifestTmp);
		vTaskDelete(NULL);
		return;
	}

	// base URL = manifest URL up to (and including) the last '/'
	String baseUrl = manifestUrl;
	const int lastSlash = baseUrl.lastIndexOf('/');
	if (lastSlash >= 0) {
		baseUrl = baseUrl.substring(0, lastSlash + 1);
	}

	size_t processed = 0;
	size_t downloaded = 0;
	size_t failed = 0;

	// If playback is running, pause it for the SD-writing phase so the card isn't
	// read and written at the same time. Pressing play again cancels the sync (see
	// AudioPlayer_SetTrackControl), and we resume here only if it finished on its own.
	bool resumePlaybackAfter = false;
	if (gPlayProperties.playMode != NO_PLAYLIST && !gPlayProperties.pausePlay) {
		AudioPlayer_SetTrackControl(PAUSEPLAY);
		for (uint8_t i = 0; i < 50 && !gPlayProperties.pausePlay; i++) {
			vTaskDelay(pdMS_TO_TICKS(20)); // wait until the audio task has actually paused
		}
		resumePlaybackAfter = true;
	}

	System_PauseTasksDuringUpload(true); // free SD/CPU and stop RFID from starting playback mid-sync
	Led_ShowSyncColor(); // indicate the running sync with a solid blue (single transmission, no repeated show())

	bool cancelled = false;
	bool more = true;
	while (more) {
		if (gSyncCancel) {
			cancelled = true;
			break;
		}
		JsonDocument entryDoc; // holds a single manifest entry -> stays tiny
		if (deserializeJson(entryDoc, manifest)) {
			break; // no (more) entries -> we reached the closing ']'
		}
		JsonObject entry = entryDoc.as<JsonObject>();
		String path = entry["path"].as<String>();
		const long size = entry["size"] | -1;
		while (path.startsWith("/")) {
			path = path.substring(1);
		}
		if (path.length() > 0) {
			const String localPath = "/" + path;

			// additive diff: skip if a local file of the same size already exists
			bool needDownload = true;
			if ((size >= 0) && gFSystem.exists(localPath)) {
				File existing = gFSystem.open(localPath, "r");
				if (existing) {
					if ((long) existing.size() == size) {
						needDownload = false;
					}
					existing.close();
				}
			}

			if (needDownload) {
				// expose the file currently being downloaded so the web UI can show it
				syncSetMessage(path.c_str());
				const String fileUrl = baseUrl + syncUrlEncodePath(path);
				if (syncDownloadFile(fileUrl, user, pass, localPath)) {
					downloaded++;
					Log_Printf(LOGLEVEL_INFO, "Sync: downloaded %s", localPath.c_str());
				} else {
					failed++;
					Log_Printf(LOGLEVEL_ERROR, "Sync: failed %s", localPath.c_str());
				}
			}
		}

		processed++;
		// total entry count is unknown while streaming, so track progress by how far
		// we are through the manifest file instead
		gSyncProgress = (manifestBytes > 0) ? (uint8_t) (((uint32_t) manifest.position() * 100) / manifestBytes) : 100;
		vTaskDelay(pdMS_TO_TICKS(1));

		more = manifest.findUntil(",", "]"); // step to the next entry, or stop at ']'
	}

	manifest.close();
	gFSystem.remove(manifestTmp);

	System_PauseTasksDuringUpload(false);

	// Resume playback only if we paused it and the user didn't already take over by
	// pressing play (which cancels the sync and resumes playback itself).
	if (resumePlaybackAfter && !cancelled && gPlayProperties.pausePlay) {
		AudioPlayer_SetTrackControl(PAUSEPLAY);
	}

	syncSetMessagef("%u downloaded, %u failed, %u total", (unsigned) downloaded, (unsigned) failed, (unsigned) processed);
	char summary[sizeof(gSyncMsg)];
	Sync_CopyMessage(summary, sizeof(summary));
	Log_Printf(LOGLEVEL_NOTICE, "Sync %s: %s", cancelled ? "stopped" : "finished", summary);
	gSyncStatus = cancelled ? 4 : ((failed > 0) ? 3 : 2);
	vTaskDelete(NULL);
}

void Sync_Trigger(void) {
	if (gSyncStatus == 1) {
		return; // already running
	}
	gSyncCancel = false;
	gSyncStatus = 1;
	gSyncProgress = 0;
	syncSetMessage("");
	xTaskCreatePinnedToCore(syncTask, "httpSync", 16384, NULL, 1, NULL, 1);
}
