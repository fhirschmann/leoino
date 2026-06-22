#include <Arduino.h>
#include "settings.h"

#include "Sync.h"

#include "AudioPlayer.h"
#include "Common.h"
#include "Led.h"
#include "Log.h"
#include "Net.h"
#include "SdCard.h"
#include "StatusMessage.h"
#include "System.h"
#include "Wlan.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <algorithm>
#include <memory>

// Abort a single file download if no data arrives for this long (connection still
// "open" but stalled), so one bad/slow file can't hang the whole sync forever.
static constexpr uint32_t SYNC_STALL_TIMEOUT_MS = 20000;

// State of the HTTP sync, polled by the web interface via GET /sync.
// 0 = idle, 1 = running, 2 = done, 3 = failed, 4 = stopped (cancelled by user)
static volatile uint8_t gSyncStatus = 0;
static volatile uint8_t gSyncProgress = 0; // percent (files processed / total)
static volatile bool gSyncCancel = false; // cooperative cancel flag

// Status message shared across cores: the sync task (core 1) writes the current file/result, the
// web server (core 0) reads it for the progress UI. StatusMessage's spinlock keeps a reader from
// ever seeing a half-written string (which would show up as a brief garbage line in the UI).
static StatusMessage gSyncMsg;

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
	gSyncMsg.copy(dst, dstLen);
}

static void syncFail(const char *msg) {
	gSyncMsg.set(msg);
	Log_Printf(LOGLEVEL_ERROR, "Sync failed: %s", msg);
	gSyncStatus = 3;
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
	std::unique_ptr<WiFiClient> client = Net_MakeClient(url);
	HTTPClient http;
	Net_SetupHttp(http, user, pass);
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

// Set of 64-bit path hashes used only by the optional mirror-delete pass: it records every
// path the manifest listed so the later sweep can tell wanted files apart from orphans.
// Stored in PSRAM (8 bytes/entry) so even a large manifest (1000+ files) never touches the
// scarce internal heap. A 64-bit FNV-1a hash makes a collision astronomically unlikely, and
// the only effect a collision could have is sparing one orphan from deletion — it can never
// cause a wanted file to be deleted.
class SyncPathSet {
public:
	~SyncPathSet() {
		free(mData);
	}
	bool add(uint64_t hash) {
		if (mCount == mCapacity) {
			const size_t newCap = mCapacity ? (mCapacity * 2) : 256;
			uint64_t *grown = (uint64_t *) ps_realloc(mData, newCap * sizeof(uint64_t));
			if (!grown) {
				return false; // out of memory -> caller skips the mirror pass so nothing is wrongly deleted
			}
			mData = grown;
			mCapacity = newCap;
		}
		mData[mCount++] = hash;
		return true;
	}
	void finalize() {
		std::sort(mData, mData + mCount); // sort once so contains() can binary-search
	}
	bool contains(uint64_t hash) const {
		return std::binary_search(mData, mData + mCount, hash);
	}
	size_t size() const {
		return mCount;
	}

private:
	uint64_t *mData = nullptr;
	size_t mCount = 0;
	size_t mCapacity = 0;
};

// Case-insensitive (FAT is) FNV-1a hash of a full SD path. The exact same normalization must
// be used when recording manifest paths and when scanning local files, or they won't match.
static uint64_t syncHashPath(const String &path) {
	uint64_t hash = 1469598103934665603ULL; // FNV-1a offset basis
	for (size_t i = 0; i < path.length(); i++) {
		char c = path.charAt(i);
		if (c >= 'A' && c <= 'Z') {
			c += 'a' - 'A'; // lowercase ASCII; multibyte bytes (umlauts) pass through unchanged on both sides
		}
		hash ^= (uint8_t) c;
		hash *= 1099511628211ULL; // FNV-1a prime
	}
	return hash;
}

// Files/directories the mirror-delete pass must never touch: everything hidden (a leading
// dot — covers /.html, /.cache, the sync/backup temp files and macOS junk), the FAT system
// folder, and the firmware-managed root files that aren't part of the synced media library.
static bool syncIsProtected(const String &fullPath, const String &name) {
	if (name.length() == 0 || name.charAt(0) == '.') {
		return true;
	}
	if (name.equalsIgnoreCase("System Volume Information")) {
		return true;
	}
	return (fullPath == "/manifest.json") || (fullPath == "/stats.csv") || (fullPath == backupFile);
}

// Recursively deletes every file under `dir` whose path the manifest did NOT list, then prunes
// the directories this empties. `keep` must be the COMPLETE set of manifest paths — the caller
// only runs this when the manifest parsed fully — otherwise wanted files would be deleted.
static void syncMirrorDir(File dir, const SyncPathSet &keep, size_t &deleted) {
	File entry = dir.openNextFile();
	while (entry) {
		if (gSyncCancel) { // user pressed stop -> leave the rest of the card untouched
			entry.close();
			break;
		}
		const String path = entry.path();
		const String name = entry.name();
		const bool isDir = entry.isDirectory();
		if (syncIsProtected(path, name)) {
			entry.close();
		} else if (isDir) {
			syncMirrorDir(entry, keep, deleted);
			entry.close();
			gFSystem.rmdir(path); // only succeeds once the directory has been emptied
		} else {
			entry.close();
			if (!keep.contains(syncHashPath(path)) && gFSystem.remove(path)) {
				deleted++;
				Log_Printf(LOGLEVEL_INFO, "Sync: deleted %s", path.c_str());
			}
		}
		entry = dir.openNextFile();
		vTaskDelay(pdMS_TO_TICKS(1)); // yield between entries (matches the download loop)
	}
}

static void syncTask(void *parameter) {
	gSyncProgress = 0;
	gSyncMsg.set("");

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
	String user, pass;
	Net_GetSyncCreds(user, pass);

	// Mirror mode (opt-in, off by default): after pulling the manifest's files, delete local
	// files the manifest didn't list. We collect every listed path into keepSet during the
	// download pass; if it can't be built completely the mirror pass is skipped (see below).
	const bool mirror = gPrefsSettings.getBool("syncDelete", false);
	SyncPathSet keepSet;
	bool keepSetOk = true;

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
		std::unique_ptr<WiFiClient> client = Net_MakeClient(manifestUrl);
		HTTPClient http;
		Net_SetupHttp(http, user, pass);
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
		// reject path-traversal: a malicious/compromised manifest server must never be able to
		// write outside the SD root via ".." segments (leading "/" already stripped above).
		const bool unsafePath = (path == "..") || path.startsWith("../") || (path.indexOf("/../") >= 0) || path.endsWith("/..");
		if (unsafePath) {
			Log_Printf(LOGLEVEL_ERROR, "Sync: rejecting unsafe path %s", path.c_str());
			failed++;
		}
		if ((path.length() > 0) && !unsafePath) {
			const String localPath = "/" + path;

			// remember every listed path so the optional mirror pass keeps it (regardless of
			// whether it gets downloaded now or already exists locally)
			if (mirror && !keepSet.add(syncHashPath(localPath))) {
				keepSetOk = false;
			}

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
				gSyncMsg.set(path.c_str());
				const String fileUrl = baseUrl + Url_EncodePath(path);
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

	// Optional mirror pass: delete local files the manifest didn't list. Strictly opt-in
	// (off by default) and only when we have a complete manifest to compare against — if the
	// user cancelled, the keep-set couldn't be fully built (OOM), or the manifest had no files,
	// we skip it so a failed/empty manifest can never wipe the card. Runs while the other tasks
	// are still paused so the SD isn't accessed concurrently.
	size_t deleted = 0;
	if (mirror && !cancelled && keepSetOk && (keepSet.size() > 0)) {
		keepSet.finalize();
		gSyncMsg.set("removing files not in manifest");
		File root = gFSystem.open("/");
		if (root && root.isDirectory()) {
			syncMirrorDir(root, keepSet, deleted);
			root.close();
		}
		if (gSyncCancel) { // stopped during the sweep
			cancelled = true;
		}
	}

	System_PauseTasksDuringUpload(false);

	// Resume playback only if we paused it and the user didn't already take over by
	// pressing play (which cancels the sync and resumes playback itself).
	if (resumePlaybackAfter && !cancelled && gPlayProperties.pausePlay) {
		AudioPlayer_SetTrackControl(PAUSEPLAY);
	}

	if (mirror) {
		gSyncMsg.setf("%u downloaded, %u deleted, %u failed, %u total", (unsigned) downloaded, (unsigned) deleted, (unsigned) failed, (unsigned) processed);
	} else {
		gSyncMsg.setf("%u downloaded, %u failed, %u total", (unsigned) downloaded, (unsigned) failed, (unsigned) processed);
	}
	char summary[StatusMessage::Capacity];
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
	gSyncMsg.set("");
	xTaskCreatePinnedToCore(syncTask, "httpSync", 16384, NULL, 1, NULL, 1);
}
