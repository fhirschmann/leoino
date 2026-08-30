#include <Arduino.h>
#include "settings.h"

#include "Sync.h"

#include "AudioPlayer.h"
#include "Common.h"
#include "JsonPsram.h"
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
#include <atomic>
#include <climits>
#include <mbedtls/sha256.h>
#include <memory>

// Abort a single file download if no data arrives for this long (connection still
// "open" but stalled), so one bad/slow file can't hang the whole sync forever.
static constexpr uint32_t SYNC_STALL_TIMEOUT_MS = 20000;

// State of the HTTP sync, polled by the web interface via GET /sync.
// 0 = idle, 1 = running, 2 = done, 3 = failed, 4 = stopped (cancelled by user)
static volatile uint8_t gSyncStatus = 0;
static volatile uint8_t gSyncProgress = 0; // percent (files processed / total)
static volatile bool gSyncCancel = false; // cooperative cancel flag

// Dry-run: when set, the task does the exact same diff but downloads/deletes nothing and instead
// writes a human-readable report of what it *would* do to the SD card (so the user can review a
// mirror-delete before it actually removes files). The report is served via GET /syncreport.
static volatile bool gSyncDryRun = false;
static const char *kSyncDryReport = "/.sync_dryrun.txt"; // hidden -> excluded from the manifest and from mirror-delete
static const char *kPlaylistSyncState = "/.playlist_sync_state.json";
static std::atomic_flag gPlaylistFileLock = ATOMIC_FLAG_INIT;

void Sync_LockPlaylistFiles(void) {
	while (gPlaylistFileLock.test_and_set(std::memory_order_acquire)) {
		vTaskDelay(pdMS_TO_TICKS(5));
	}
}

void Sync_UnlockPlaylistFiles(void) {
	gPlaylistFileLock.clear(std::memory_order_release);
}

class SyncPlaylistFileGuard {
public:
	SyncPlaylistFileGuard() {
		Sync_LockPlaylistFiles();
	}
	~SyncPlaylistFileGuard() {
		Sync_UnlockPlaylistFiles();
	}
};

const char *Sync_GetDryReportPath(void) {
	return kSyncDryReport;
}

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
	// Force HTTP/1.0 so the server can't reply with chunked transfer-encoding: the read loop below
	// streams the raw socket straight to disk and does not de-chunk, so chunk framing would end up
	// written into the file. HTTP/1.0 responses are always identity/close-delimited (read-until-close),
	// which is exactly what the remaining == -1 branch already handles.
	http.useHTTP10(true);
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

static bool syncIsPlaylistPath(const String &path) {
	String lower = path;
	lower.toLowerCase();
	return lower.startsWith("/playlists/") && path.lastIndexOf('/') == 10 && (lower.endsWith(".m3u") || lower.endsWith(".m3u8"));
}

static bool syncHashFile(const String &path, String &hashOut) {
	File file = gFSystem.open(path, "r");
	if (!file || file.isDirectory()) {
		if (file) {
			file.close();
		}
		return false;
	}
	mbedtls_sha256_context context;
	mbedtls_sha256_init(&context);
	bool ok = mbedtls_sha256_starts(&context, 0) == 0;
	uint8_t buffer[1024];
	while (ok && file.available()) {
		const size_t read = file.read(buffer, sizeof(buffer));
		if (read == 0) {
			ok = false;
			break;
		}
		ok = mbedtls_sha256_update(&context, buffer, read) == 0;
	}
	unsigned char digest[32];
	if (ok) {
		ok = mbedtls_sha256_finish(&context, digest) == 0;
	}
	mbedtls_sha256_free(&context);
	file.close();
	if (!ok) {
		return false;
	}
	static const char *hex = "0123456789abcdef";
	char encoded[65];
	for (size_t i = 0; i < sizeof(digest); i++) {
		encoded[i * 2] = hex[digest[i] >> 4];
		encoded[i * 2 + 1] = hex[digest[i] & 0x0f];
	}
	encoded[64] = '\0';
	hashOut = encoded;
	return true;
}

static JsonObject syncFindPlaylistState(JsonDocument &state, const String &path, bool create) {
	if (!state["items"].is<JsonArray>()) {
		state["items"].to<JsonArray>();
	}
	JsonArray items = state["items"].as<JsonArray>();
	for (JsonObject item : items) {
		const String storedPath = item["path"].as<String>();
		if (storedPath.equalsIgnoreCase(path)) {
			return item;
		}
	}
	if (!create) {
		return JsonObject();
	}
	JsonObject item = items.add<JsonObject>();
	item["path"] = path;
	item["revision"] = 0;
	item["sha256"] = "";
	item["deleted"] = false;
	return item;
}

static void syncSetPlaylistState(JsonObject item, const String &path, uint32_t revision, const String &hash, bool deleted) {
	item["path"] = path;
	item["revision"] = revision;
	item["sha256"] = deleted ? "" : hash;
	item["deleted"] = deleted;
	item["_seen"] = true;
}

static void syncLoadPlaylistState(JsonDocument &state) {
	state.clear();
	File file = gFSystem.open(kPlaylistSyncState, "r");
	if (file) {
		if (deserializeJson(state, file) != DeserializationError::Ok) {
			state.clear();
		}
		file.close();
	}
	state["version"] = 1;
	if (!state["items"].is<JsonArray>()) {
		state["items"].to<JsonArray>();
	}
	for (JsonObject item : state["items"].as<JsonArray>()) {
		item["_seen"] = false;
	}
}

static bool syncSavePlaylistState(JsonDocument &state) {
	for (JsonObject item : state["items"].as<JsonArray>()) {
		item.remove("_seen");
	}
	const String temporaryPath = String(kPlaylistSyncState) + ".tmp";
	const String backupPath = String(kPlaylistSyncState) + ".bak";
	gFSystem.remove(temporaryPath);
	File file = gFSystem.open(temporaryPath, "w", true);
	if (!file) {
		return false;
	}
	const size_t expected = measureJson(state);
	const size_t written = serializeJson(state, file);
	file.flush();
	file.close();
	if (written != expected) {
		gFSystem.remove(temporaryPath);
		return false;
	}
	const bool existed = gFSystem.exists(kPlaylistSyncState);
	gFSystem.remove(backupPath);
	if ((existed && !gFSystem.rename(kPlaylistSyncState, backupPath)) || !gFSystem.rename(temporaryPath, kPlaylistSyncState)) {
		gFSystem.remove(temporaryPath);
		if (existed && gFSystem.exists(backupPath)) {
			gFSystem.rename(backupPath, kPlaylistSyncState);
		}
		return false;
	}
	gFSystem.remove(backupPath);
	return true;
}

struct SyncPlaylistHttpResult {
	int code = -1;
	uint32_t revision = 0;
	String hash;
	bool deleted = false;
	bool ok = false;
	bool conflict = false;
};

static String syncPlaylistMutationUrl(const String &manifestUrl, const String &path, uint32_t baseRevision) {
	String url = manifestUrl;
	url += manifestUrl.indexOf('?') >= 0 ? '&' : '?';
	String relativePath = path;
	while (relativePath.startsWith("/")) {
		relativePath.remove(0, 1);
	}
	url += "playlist=" + Url_EncodePath(relativePath) + "&baseRevision=" + String(baseRevision);
	return url;
}

static SyncPlaylistHttpResult syncParsePlaylistMutationResponse(HTTPClient &http, int code) {
	SyncPlaylistHttpResult result;
	result.code = code;
	result.ok = code == 200 || code == 201;
	result.conflict = code == 409;
	if (!result.ok && !result.conflict) {
		return result;
	}
	const String body = http.getString(); // playlist API responses are deliberately tiny metadata JSON
	JsonDocument response;
	if (deserializeJson(response, body) == DeserializationError::Ok) {
		result.revision = response["revision"] | 0;
		result.hash = response["sha256"].as<String>();
		result.deleted = response["deleted"] | false;
	}
	if (result.revision == 0) {
		result.ok = false;
		result.conflict = false;
	}
	return result;
}

static SyncPlaylistHttpResult syncUploadPlaylist(const String &manifestUrl, const String &user, const String &pass, const String &path, uint32_t baseRevision) {
	SyncPlaylistHttpResult result;
	File file = gFSystem.open(path, "r");
	if (!file || file.isDirectory()) {
		if (file) {
			file.close();
		}
		return result;
	}
	const size_t fileSize = file.size();
	const String url = syncPlaylistMutationUrl(manifestUrl, path, baseRevision);
	std::unique_ptr<WiFiClient> client = Net_MakeClient(url);
	HTTPClient http;
	Net_SetupHttp(http, user, pass, 20000);
	if (!http.begin(*client, url)) {
		file.close();
		return result;
	}
	http.addHeader("Content-Type", "audio/x-mpegurl");
	http.addHeader("X-ESPuino-Host", Wlan_GetHostname());
	const int code = http.sendRequest("POST", &file, fileSize);
	file.close();
	result = syncParsePlaylistMutationResponse(http, code);
	http.end();
	return result;
}

static SyncPlaylistHttpResult syncDeletePlaylistRemote(const String &manifestUrl, const String &user, const String &pass, const String &path, uint32_t baseRevision) {
	SyncPlaylistHttpResult result;
	const String url = syncPlaylistMutationUrl(manifestUrl, path, baseRevision);
	std::unique_ptr<WiFiClient> client = Net_MakeClient(url);
	HTTPClient http;
	Net_SetupHttp(http, user, pass, 20000);
	if (!http.begin(*client, url)) {
		return result;
	}
	http.addHeader("X-ESPuino-Host", Wlan_GetHostname());
	const int code = http.sendRequest("DELETE");
	result = syncParsePlaylistMutationResponse(http, code);
	http.end();
	return result;
}

static bool syncCopyFile(const String &sourcePath, const String &destinationPath) {
	File source = gFSystem.open(sourcePath, "r");
	if (!source || source.isDirectory()) {
		if (source) {
			source.close();
		}
		return false;
	}
	File destination = gFSystem.open(destinationPath, "w", true);
	if (!destination) {
		source.close();
		return false;
	}
	uint8_t buffer[1024];
	bool ok = true;
	while (source.available()) {
		const size_t read = source.read(buffer, sizeof(buffer));
		if (read == 0 || destination.write(buffer, read) != read) {
			ok = false;
			break;
		}
	}
	destination.flush();
	source.close();
	destination.close();
	if (!ok) {
		gFSystem.remove(destinationPath);
	}
	return ok;
}

static String syncCreatePlaylistConflictCopy(const String &path) {
	const int dot = path.lastIndexOf('.');
	const String extension = dot > 10 ? path.substring(dot) : ".m3u";
	const String stem = dot > 10 ? path.substring(0, dot) : path;
	String host = Wlan_GetHostname();
	host.replace("/", "-");
	host.replace("\\", "-");
	for (uint8_t attempt = 0; attempt < 100; attempt++) {
		String suffix = " (Konflikt " + host;
		if (attempt > 0) {
			suffix += " " + String(attempt + 1);
		}
		suffix += ")";
		const size_t maxStemLength = suffix.length() + extension.length() < 240 ? 240 - suffix.length() - extension.length() : 11;
		size_t stemLength = std::min<size_t>(stem.length(), maxStemLength);
		while (stemLength > 11 && stemLength < stem.length() && (((uint8_t) stem.charAt(stemLength)) & 0xc0) == 0x80) {
			stemLength--;
		}
		const String candidate = stem.substring(0, stemLength) + suffix + extension;
		if (!gFSystem.exists(candidate) && syncCopyFile(path, candidate)) {
			return candidate;
		}
	}
	return "";
}

static bool syncDownloadPlaylistAtomic(const String &url, const String &user, const String &pass, const String &path, const String &expectedHash) {
	const String temporaryPath = "/.playlist_sync_download.tmp";
	const String backupPath = "/.playlist_sync_download.bak";
	gFSystem.remove(temporaryPath);
	gFSystem.remove(backupPath);
	if (!syncDownloadFile(url, user, pass, temporaryPath)) {
		return false;
	}
	String downloadedHash;
	if (!syncHashFile(temporaryPath, downloadedHash) || !downloadedHash.equalsIgnoreCase(expectedHash)) {
		gFSystem.remove(temporaryPath);
		return false;
	}
	const bool existed = gFSystem.exists(path);
	if ((existed && !gFSystem.rename(path, backupPath)) || !gFSystem.rename(temporaryPath, path)) {
		gFSystem.remove(temporaryPath);
		if (existed && gFSystem.exists(backupPath)) {
			gFSystem.rename(backupPath, path);
		}
		return false;
	}
	gFSystem.remove(backupPath);
	return true;
}

static uint8_t syncReadManifestVersion(const char *manifestPath) {
	File manifest = gFSystem.open(manifestPath, "r");
	if (!manifest) {
		return 1;
	}
	uint8_t version = 1;
	if (manifest.find("\"version\"") && manifest.find(':')) {
		const long parsed = manifest.parseInt();
		if (parsed > 0 && parsed <= UINT8_MAX) {
			version = (uint8_t) parsed;
		}
	}
	manifest.close();
	return version;
}

struct SyncPlaylistCounts {
	size_t pushed = 0;
	size_t pulled = 0;
	size_t deleted = 0;
	size_t conflicts = 0;
	size_t failed = 0;
};

static void syncPlaylistDryLine(File *report, const char *action, const String &path) {
	if (report && *report) {
		report->printf("PL %-8s %s\n", action, path.c_str());
	}
}

static String syncPlaylistDownloadUrl(const String &baseUrl, const String &path) {
	String relative = path;
	while (relative.startsWith("/")) {
		relative.remove(0, 1);
	}
	return baseUrl + Url_EncodePath(relative);
}

static bool syncResolvePlaylistHttpConflict(const SyncPlaylistHttpResult &result, JsonObject stateItem, const String &baseUrl, const String &user, const String &pass, const String &path,
	SyncPlaylistCounts &counts) {
	counts.conflicts++;
	const String conflictPath = gFSystem.exists(path) ? syncCreatePlaylistConflictCopy(path) : "";
	if (gFSystem.exists(path) && conflictPath.isEmpty()) {
		counts.failed++;
		return false;
	}
	if (result.deleted) {
		if (gFSystem.exists(path) && !gFSystem.remove(path)) {
			counts.failed++;
			return false;
		}
		syncSetPlaylistState(stateItem, path, result.revision, "", true);
		Log_Printf(LOGLEVEL_NOTICE, "Playlist sync: server conflict on %s; preserved local copy as %s", path.c_str(), conflictPath.c_str());
		return true;
	}
	if (result.hash.length() != 64 || !syncDownloadPlaylistAtomic(syncPlaylistDownloadUrl(baseUrl, path), user, pass, path, result.hash)) {
		counts.failed++;
		return false;
	}
	syncSetPlaylistState(stateItem, path, result.revision, result.hash, false);
	counts.pulled++;
	Log_Printf(LOGLEVEL_NOTICE, "Playlist sync: server conflict on %s; preserved local copy as %s", path.c_str(), conflictPath.c_str());
	return true;
}

static bool syncPullPlaylist(const String &baseUrl, const String &user, const String &pass, JsonObject stateItem, const String &path, uint32_t revision, const String &hash,
	SyncPlaylistCounts &counts) {
	if (!syncDownloadPlaylistAtomic(syncPlaylistDownloadUrl(baseUrl, path), user, pass, path, hash)) {
		counts.failed++;
		return false;
	}
	syncSetPlaylistState(stateItem, path, revision, hash, false);
	counts.pulled++;
	return true;
}

static void syncReconcileRemotePlaylist(const String &manifestUrl, const String &baseUrl, const String &user, const String &pass, JsonDocument &state, const String &path,
	uint32_t remoteRevision, const String &remoteHash, bool dryRun, File *dryReport, SyncPlaylistCounts &counts) {
	SyncPlaylistFileGuard fileGuard;
	JsonObject stateItem = syncFindPlaylistState(state, path, false);
	const bool hadState = !stateItem.isNull();
	if (!hadState) {
		stateItem = syncFindPlaylistState(state, path, true);
	}
	const uint32_t baseRevision = stateItem["revision"] | 0;
	const String baseHash = stateItem["sha256"].as<String>();
	const bool baseDeleted = stateItem["deleted"] | false;
	stateItem["_seen"] = true;

	const bool localExists = gFSystem.exists(path);
	String localHash;
	if (localExists && !syncHashFile(path, localHash)) {
		counts.failed++;
		return;
	}
	if (localExists && localHash.equalsIgnoreCase(remoteHash)) {
		// Same content independently reached both sides (or an upload response was lost): just adopt
		// the server revision and avoid manufacturing a false conflict.
		syncSetPlaylistState(stateItem, path, remoteRevision, remoteHash, false);
		return;
	}

	if (!localExists) {
		const bool localDelete = hadState && !baseDeleted && baseRevision == remoteRevision && baseHash.equalsIgnoreCase(remoteHash);
		if (localDelete) {
			counts.deleted++;
			if (dryRun) {
				syncPlaylistDryLine(dryReport, "DELETE", path);
				return;
			}
			const SyncPlaylistHttpResult result = syncDeletePlaylistRemote(manifestUrl, user, pass, path, baseRevision);
			if (result.ok && result.deleted) {
				syncSetPlaylistState(stateItem, path, result.revision, "", true);
			} else if (result.conflict) {
				// The remote edit won over a simultaneous local delete; keep it locally on this device.
				counts.conflicts++;
				if (!result.deleted && result.hash.length() == 64) {
					syncPullPlaylist(baseUrl, user, pass, stateItem, path, result.revision, result.hash, counts);
				} else {
					syncSetPlaylistState(stateItem, path, result.revision, "", true);
				}
			} else {
				counts.failed++;
			}
			return;
		}
		if (hadState && !baseDeleted) {
			counts.conflicts++; // delete vs a newer remote edit; remote wins without data loss
			syncPlaylistDryLine(dryReport, "CONFLICT", path);
		}
		counts.pulled++;
		if (dryRun) {
			syncPlaylistDryLine(dryReport, "PULL", path);
			return;
		}
		counts.pulled--; // syncPullPlaylist increments only after a successful atomic replacement
		syncPullPlaylist(baseUrl, user, pass, stateItem, path, remoteRevision, remoteHash, counts);
		return;
	}

	const bool localChanged = !hadState || baseDeleted || !localHash.equalsIgnoreCase(baseHash);
	const bool remoteChanged = !hadState || baseDeleted || baseRevision != remoteRevision || !remoteHash.equalsIgnoreCase(baseHash);
	if (localChanged && !remoteChanged) {
		counts.pushed++;
		if (dryRun) {
			syncPlaylistDryLine(dryReport, "PUSH", path);
			return;
		}
		const SyncPlaylistHttpResult result = syncUploadPlaylist(manifestUrl, user, pass, path, baseRevision);
		if (result.ok) {
			syncSetPlaylistState(stateItem, path, result.revision, result.hash.length() == 64 ? result.hash : localHash, false);
		} else if (result.conflict) {
			syncResolvePlaylistHttpConflict(result, stateItem, baseUrl, user, pass, path, counts);
		} else {
			counts.failed++;
		}
		return;
	}
	if (!localChanged && remoteChanged) {
		counts.pulled++;
		if (dryRun) {
			syncPlaylistDryLine(dryReport, "PULL", path);
			return;
		}
		counts.pulled--;
		syncPullPlaylist(baseUrl, user, pass, stateItem, path, remoteRevision, remoteHash, counts);
		return;
	}

	// Both sides changed from the last common revision. Preserve the local edit under a conflict
	// name, then put the server version at the canonical path. The conflict copy is uploaded in the
	// local-only pass later in this same sync.
	counts.conflicts++;
	if (dryRun) {
		syncPlaylistDryLine(dryReport, "CONFLICT", path);
		return;
	}
	const String conflictPath = syncCreatePlaylistConflictCopy(path);
	if (conflictPath.isEmpty()) {
		counts.failed++;
		return;
	}
	if (!syncPullPlaylist(baseUrl, user, pass, stateItem, path, remoteRevision, remoteHash, counts)) {
		return;
	}
	Log_Printf(LOGLEVEL_NOTICE, "Playlist sync: concurrent edit on %s; preserved local copy as %s", path.c_str(), conflictPath.c_str());
}

static void syncReconcilePlaylistTombstone(const String &manifestUrl, const String &baseUrl, const String &user, const String &pass, JsonDocument &state, const String &path,
	uint32_t remoteRevision, bool dryRun, File *dryReport, SyncPlaylistCounts &counts) {
	SyncPlaylistFileGuard fileGuard;
	JsonObject stateItem = syncFindPlaylistState(state, path, false);
	const bool hadState = !stateItem.isNull();
	if (!hadState) {
		stateItem = syncFindPlaylistState(state, path, true);
	}
	const uint32_t baseRevision = stateItem["revision"] | 0;
	const String baseHash = stateItem["sha256"].as<String>();
	const bool baseDeleted = stateItem["deleted"] | false;
	stateItem["_seen"] = true;
	if (!gFSystem.exists(path)) {
		syncSetPlaylistState(stateItem, path, remoteRevision, "", true);
		return;
	}

	String localHash;
	if (!syncHashFile(path, localHash)) {
		counts.failed++;
		return;
	}
	// Re-creating a playlist after observing this exact tombstone is an intentional local change.
	if (hadState && baseDeleted && baseRevision == remoteRevision) {
		counts.pushed++;
		if (dryRun) {
			syncPlaylistDryLine(dryReport, "PUSH", path);
			return;
		}
		const SyncPlaylistHttpResult result = syncUploadPlaylist(manifestUrl, user, pass, path, baseRevision);
		if (result.ok) {
			syncSetPlaylistState(stateItem, path, result.revision, result.hash.length() == 64 ? result.hash : localHash, false);
		} else if (result.conflict) {
			syncResolvePlaylistHttpConflict(result, stateItem, baseUrl, user, pass, path, counts);
		} else {
			counts.failed++;
		}
		return;
	}

	const bool localChanged = !hadState || baseDeleted || !localHash.equalsIgnoreCase(baseHash);
	const bool remoteChanged = !hadState || !baseDeleted || baseRevision != remoteRevision;
	if (!localChanged && remoteChanged) {
		counts.deleted++;
		if (dryRun) {
			syncPlaylistDryLine(dryReport, "REMOVE", path);
			return;
		}
		if (!gFSystem.remove(path)) {
			counts.failed++;
			return;
		}
		syncSetPlaylistState(stateItem, path, remoteRevision, "", true);
		return;
	}

	// A remote delete raced a local edit or a same-named playlist unknown to this device. Preserve
	// the local content as a conflict copy and honor the tombstone at the canonical path.
	counts.conflicts++;
	if (dryRun) {
		syncPlaylistDryLine(dryReport, "CONFLICT", path);
		return;
	}
	const String conflictPath = syncCreatePlaylistConflictCopy(path);
	if (conflictPath.isEmpty() || !gFSystem.remove(path)) {
		counts.failed++;
		return;
	}
	syncSetPlaylistState(stateItem, path, remoteRevision, "", true);
	Log_Printf(LOGLEVEL_NOTICE, "Playlist sync: delete/edit conflict on %s; preserved local copy as %s", path.c_str(), conflictPath.c_str());
}

static bool syncProcessPlaylistTombstones(File &manifest, const String &manifestUrl, const String &baseUrl, const String &user, const String &pass, JsonDocument &state, bool dryRun,
	File *dryReport, SyncPlaylistCounts &counts) {
	manifest.seek(0);
	if (!manifest.find("\"playlistTombstones\"") || !manifest.find('[')) {
		return true; // version-2 servers may legitimately have no tombstone field yet
	}
	while (manifest.peek() == ' ' || manifest.peek() == '\n' || manifest.peek() == '\r' || manifest.peek() == '\t') {
		manifest.read();
	}
	if (manifest.peek() == ']') {
		manifest.read();
		return true;
	}
	bool more = true;
	while (more) {
		if (gSyncCancel) {
			return true;
		}
		JsonDocument entryDoc;
		const DeserializationError error = deserializeJson(entryDoc, manifest);
		if (error == DeserializationError::EmptyInput) {
			break;
		}
		if (error) {
			return false;
		}
		String path = entryDoc["path"].as<String>();
		if (!path.startsWith("/")) {
			path = "/" + path;
		}
		const uint32_t revision = entryDoc["revision"] | 0;
		if (syncIsPlaylistPath(path) && revision > 0) {
			syncReconcilePlaylistTombstone(manifestUrl, baseUrl, user, pass, state, path, revision, dryRun, dryReport, counts);
		}
		more = manifest.findUntil(",", "]");
	}
	return true;
}

static void syncUploadLocalOnlyPlaylists(const String &manifestUrl, const String &baseUrl, const String &user, const String &pass, JsonDocument &state, bool dryRun, File *dryReport,
	SyncPlaylistCounts &counts) {
	SyncPlaylistFileGuard fileGuard;
	File directory = gFSystem.open("/Playlists");
	if (!directory || !directory.isDirectory()) {
		if (directory) {
			directory.close();
		}
		return;
	}
	File entry = directory.openNextFile();
	while (entry) {
		if (gSyncCancel) {
			entry.close();
			break;
		}
		const bool regularFile = !entry.isDirectory();
		String path = entry.path();
		entry.close();
		if (regularFile && syncIsPlaylistPath(path)) {
			JsonObject stateItem = syncFindPlaylistState(state, path, false);
			const bool hadState = !stateItem.isNull();
			if (!hadState) {
				stateItem = syncFindPlaylistState(state, path, true);
			}
			if (!(stateItem["_seen"] | false)) {
				String localHash;
				if (!syncHashFile(path, localHash)) {
					counts.failed++;
				} else {
					const uint32_t baseRevision = stateItem["revision"] | 0;
					counts.pushed++;
					if (dryRun) {
						syncPlaylistDryLine(dryReport, "PUSH", path);
					} else {
						const SyncPlaylistHttpResult result = syncUploadPlaylist(manifestUrl, user, pass, path, baseRevision);
						if (result.ok) {
							syncSetPlaylistState(stateItem, path, result.revision, result.hash.length() == 64 ? result.hash : localHash, false);
						} else if (result.conflict) {
							syncResolvePlaylistHttpConflict(result, stateItem, baseUrl, user, pass, path, counts);
						} else {
							counts.failed++;
						}
					}
				}
			}
		}
		entry = directory.openNextFile();
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	directory.close();
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
	// Never mirror-delete the system folder or the user's playlists (or anything inside them):
	// returning true on the top-level directory makes the sweep skip the whole subtree.
	if (fullPath.equalsIgnoreCase("/System") || fullPath.equalsIgnoreCase("/Playlists")) {
		return true;
	}
	return (fullPath == "/manifest.json") || (fullPath == "/stats.csv") || (fullPath == backupFile);
}

// /System is fully excluded from sync: besides being kept by syncIsProtected() (no mirror-delete),
// the download pass also ignores any manifest entry that targets /System or anything inside it, so
// the device's own system files are never overwritten by what the manifest server happens to list.
static bool syncIsExcludedFromPull(const String &localPath) {
	return localPath.equalsIgnoreCase("/System") || ((localPath.length() >= 8) && localPath.substring(0, 8).equalsIgnoreCase("/System/"));
}

// Recursively handles every file under `dir` whose path the manifest did NOT list. In a real run
// it deletes them and prunes the directories this empties; in a dry run (`dryRun`) it changes
// nothing and instead appends each would-be-deleted path to `report`. `keep` must be the COMPLETE
// set of manifest paths — the caller only runs this when the manifest parsed fully — otherwise
// wanted files would be deleted.
static void syncMirrorDir(File dir, const SyncPathSet &keep, size_t &deleted, bool dryRun, File *report, uint8_t depth = 0) {
	// Bound the recursion: the sync task has a 16 kB stack and each frame holds a File + two
	// Strings, so a pathologically deep tree could otherwise overflow it. Real media trees are
	// shallow (artist/album/track), so 20 levels is far beyond anything legitimate.
	if (depth >= 20) {
		Log_Printf(LOGLEVEL_ERROR, "Sync: mirror sweep too deep, skipping %s", dir.path());
		return;
	}
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
			syncMirrorDir(entry, keep, deleted, dryRun, report, depth + 1);
			entry.close();
			if (!dryRun) {
				gFSystem.rmdir(path); // only succeeds once the directory has been emptied
			}
		} else {
			entry.close();
			if (!keep.contains(syncHashPath(path))) {
				if (dryRun) {
					deleted++;
					if (report) {
						report->printf("RM   %s\n", path.c_str());
					}
				} else if (gFSystem.remove(path)) {
					deleted++;
					Log_Printf(LOGLEVEL_INFO, "Sync: deleted %s", path.c_str());
				}
			}
		}
		entry = dir.openNextFile();
		vTaskDelay(pdMS_TO_TICKS(1)); // yield between entries (matches the download loop)
	}
}

// Downloads the manifest to a temp file on SD. Returns the number of body bytes written (>= 0) on
// success, or a negative HTTPClient error code if the transfer dropped mid-stream (so a truncated
// manifest is never silently trusted). On a setup failure (connect/HTTP/SD) it sets the failure
// message itself and returns INT_MIN. Being a normal function, every return path unwinds the
// stack, so the WiFiClient/HTTPClient are freed — the task body used vTaskDelete() and leaked them.
static int syncFetchManifest(const String &url, const String &user, const String &pass, const char *tmpPath) {
	std::unique_ptr<WiFiClient> client = Net_MakeClient(url);
	HTTPClient http;
	Net_SetupHttp(http, user, pass);
	if (!http.begin(*client, url)) {
		syncFail("manifest connection failed");
		return INT_MIN;
	}
	const int code = http.GET();
	if (code != 200) {
		char msg[64];
		snprintf(msg, sizeof(msg), "manifest HTTP %d", code);
		syncFail(msg);
		http.end();
		return INT_MIN;
	}
	File mf = gFSystem.open(tmpPath, "w", true);
	if (!mf) {
		syncFail("manifest: cannot buffer to SD");
		http.end();
		return INT_MIN;
	}
	const int written = http.writeToStream(&mf); // decodes chunked transfer-encoding, minimal RAM
	mf.close();
	http.end();
	return written; // >= 0 on a complete transfer, < 0 (e.g. CONNECTION_LOST) if it dropped mid-stream
}

static void syncRun(void) {
	gSyncProgress = 0;
	gSyncMsg.set("");

	if (!Wlan_IsConnected()) {
		syncFail("no WiFi connection");
		return;
	}

	const String manifestUrl = gPrefsSettings.getString("syncUrl", "");
	if (manifestUrl.length() == 0) {
		syncFail("no sync URL configured");
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

	// Dry run: open the report file up front. The task downloads/deletes nothing and writes
	// "DL"/"RM" lines here instead, so the user can preview a (mirror-)sync before committing.
	const bool dryRun = gSyncDryRun;
	File dryReport;
	if (dryRun) {
		dryReport = gFSystem.open(kSyncDryReport, "w", true);
		if (!dryReport) {
			syncFail("dry run: cannot write report to SD");
			return;
		}
		dryReport.print("# DRY RUN - nothing was changed. DL = would download, RM = would delete.\n");
	}

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
	const int manifestWritten = syncFetchManifest(manifestUrl, user, pass, manifestTmp);
	if (manifestWritten == INT_MIN) {
		// setup failure (connect/HTTP/SD); syncFetchManifest already set the failure message
		if (dryReport) {
			dryReport.close();
		}
		return;
	}
	if (manifestWritten < 0) {
		// the transfer dropped mid-stream -> the on-SD manifest is truncated. Never trust a partial
		// manifest: trusting it would, in mirror mode, delete every file listed past the cut-off.
		syncFail("manifest download incomplete");
		if (dryReport) {
			dryReport.close();
		}
		gFSystem.remove(manifestTmp);
		return;
	}
	const uint8_t manifestVersion = syncReadManifestVersion(manifestTmp);
	const bool playlistSyncSupported = manifestVersion >= 2;

	// Re-open the buffered manifest and seek to the start of the "files" array; from
	// here entries are deserialized one object at a time.
	File manifest = gFSystem.open(manifestTmp, "r");
	if (!manifest) {
		syncFail("manifest: reopen failed");
		if (dryReport) {
			dryReport.close();
		}
		gFSystem.remove(manifestTmp);
		return;
	}
	const size_t manifestBytes = manifest.size();
	if (!manifest.find("\"files\"") || !manifest.find('[')) {
		syncFail("manifest has no \"files\" array");
		manifest.close();
		if (dryReport) {
			dryReport.close();
		}
		gFSystem.remove(manifestTmp);
		return;
	}

	// base URL = manifest URL up to (and including) the last '/'
	String baseUrl = manifestUrl;
	const int lastSlash = baseUrl.lastIndexOf('/');
	if (lastSlash >= 0) {
		baseUrl = baseUrl.substring(0, lastSlash + 1);
	}
	SpiRamAllocator playlistStateAllocator;
	JsonDocument playlistState(&playlistStateAllocator);
	SyncPlaylistCounts playlistCounts;

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
	if (playlistSyncSupported) {
		SyncPlaylistFileGuard playlistGuard;
		syncLoadPlaylistState(playlistState);
	}

	bool cancelled = false;
	bool more = true;
	bool parseFailed = false; // a mid-manifest deserialize error (OOM / bad byte), not a clean end-of-array
	bool sawArrayEnd = false; // findUntil actually consumed the array terminator ']'
	while (more) {
		if (gSyncCancel) {
			cancelled = true;
			break;
		}
		JsonDocument entryDoc; // holds a single manifest entry -> stays tiny
		DeserializationError err = deserializeJson(entryDoc, manifest);
		if (err) {
			// EmptyInput at the very start is a legitimately empty array; any other error (NoMemory under
			// heap pressure, InvalidInput on a bad byte) is a real parse failure that must NOT look like a
			// clean end-of-array, otherwise the mirror pass could delete every file listed after this entry.
			if (err != DeserializationError::EmptyInput) {
				parseFailed = true;
			}
			break;
		}
		JsonObject entry = entryDoc.as<JsonObject>();
		String path = entry["path"].as<String>();
		const long size = entry["size"] | -1;
		const uint32_t playlistRevision = entry["revision"] | 0;
		const String playlistHash = entry["sha256"].as<String>();
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
			if (syncIsExcludedFromPull(localPath)) {
				// /System is off-limits to sync entirely (pull + mirror): never download into it.
				if (dryRun) {
					dryReport.printf("SK   %s\n", localPath.c_str());
				}
			} else {
				// remember every listed path so the optional mirror pass keeps it (regardless of
				// whether it gets downloaded now or already exists locally)
				if (mirror && !keepSet.add(syncHashPath(localPath))) {
					keepSetOk = false;
				}

				const bool managedPlaylist = playlistSyncSupported && syncIsPlaylistPath(localPath) && playlistRevision > 0 && playlistHash.length() == 64;
				if (managedPlaylist) {
					syncReconcileRemotePlaylist(manifestUrl, baseUrl, user, pass, playlistState, localPath, playlistRevision, playlistHash, dryRun, dryRun ? &dryReport : nullptr, playlistCounts);
				} else {
					// additive diff: skip if a local file of the same size already exists. Version-2
					// playlists take the hash/revision path above; audio retains this cheap size check.
					const bool localExists = gFSystem.exists(localPath);
					bool needDownload = true;
					if ((size >= 0) && localExists) {
						File existing = gFSystem.open(localPath, "r");
						if (existing) {
							if ((long) existing.size() == size) {
								needDownload = false;
							}
							existing.close();
						}
					}

					if (needDownload) {
						if (dryRun) {
							// record only what a real run would fetch (new = missing locally, chg = size differs)
							downloaded++;
							dryReport.printf("DL %s %s\n", localExists ? "chg" : "new", localPath.c_str());
						} else {
							// expose the file currently being downloaded so the web UI can show it
							gSyncMsg.set(path.c_str());
							const String fileUrl = baseUrl + Url_EncodePath(path);
							bool downloadedOk = false;
							if (syncIsPlaylistPath(localPath)) {
								SyncPlaylistFileGuard playlistGuard;
								downloadedOk = syncDownloadFile(fileUrl, user, pass, localPath);
							} else {
								downloadedOk = syncDownloadFile(fileUrl, user, pass, localPath);
							}
							if (downloadedOk) {
								downloaded++;
								Log_Printf(LOGLEVEL_INFO, "Sync: downloaded %s", localPath.c_str());
							} else {
								failed++;
								Log_Printf(LOGLEVEL_ERROR, "Sync: failed %s", localPath.c_str());
							}
						}
					}
				}
			}
		}

		processed++;
		// total entry count is unknown while streaming, so track progress by how far
		// we are through the manifest file instead
		gSyncProgress = (manifestBytes > 0) ? (uint8_t) (((uint32_t) manifest.position() * 100) / manifestBytes) : 100;
		vTaskDelay(pdMS_TO_TICKS(1));

		more = manifest.findUntil(",", "]"); // step to the next entry, or stop at ']'
		if (!more) {
			sawArrayEnd = true; // stopped at the array terminator, not mid-stream
		}
	}

	// Verify the buffered manifest actually ends cleanly: the last non-whitespace byte must be the
	// array/object terminator (']' or '}'). A truncated download that slipped past the writeToStream
	// check (e.g. a server that closed the socket after a valid-looking prefix) would otherwise be
	// indistinguishable from a clean end-of-array and could trigger a mirror-delete of the tail.
	bool endsClean = false;
	{
		size_t scan = manifest.size();
		while (scan > 0) {
			manifest.seek(scan - 1);
			const char c = (char) manifest.read();
			if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
				scan--;
				continue;
			}
			endsClean = (c == ']' || c == '}');
			break;
		}
	}
	// Requiring sawArrayEnd (the loop actually consumed the ']' terminator) and !parseFailed on top of
	// endsClean closes two holes: a mid-file parse error leaving a truncated keep-set with the file's
	// last byte still ']', and endsClean accepting a bare '}' tail the array loop never reached.
	const bool manifestComplete = !cancelled && endsClean && !parseFailed && sawArrayEnd;
	if (!cancelled && (parseFailed || !sawArrayEnd)) {
		Log_Printf(LOGLEVEL_ERROR, "Sync: manifest incomplete (%s) -> skipping mirror-delete pass",
			parseFailed ? "parse error mid-manifest" : "array not terminated");
	}
	if (playlistSyncSupported && manifestComplete && !cancelled) {
		if (!syncProcessPlaylistTombstones(manifest, manifestUrl, baseUrl, user, pass, playlistState, dryRun, dryRun ? &dryReport : nullptr, playlistCounts)) {
			playlistCounts.failed++;
		}
		if (!gSyncCancel) {
			syncUploadLocalOnlyPlaylists(manifestUrl, baseUrl, user, pass, playlistState, dryRun, dryRun ? &dryReport : nullptr, playlistCounts);
		}
		if (!dryRun) {
			SyncPlaylistFileGuard playlistGuard;
			if (!syncSavePlaylistState(playlistState)) {
				playlistCounts.failed++;
				Log_Println("Playlist sync: cannot persist local state", LOGLEVEL_ERROR);
			}
		}
		if (gSyncCancel) {
			cancelled = true;
		}
	}
	failed += playlistCounts.failed;

	manifest.close();
	gFSystem.remove(manifestTmp);

	// Optional mirror pass: delete local files the manifest didn't list. Strictly opt-in
	// (off by default) and only when we have a complete manifest to compare against — if the
	// user cancelled, the keep-set couldn't be fully built (OOM), or the manifest had no files,
	// we skip it so a failed/empty manifest can never wipe the card. Runs while the other tasks
	// are still paused so the SD isn't accessed concurrently.
	size_t deleted = 0;
	if (mirror && !cancelled && manifestComplete && keepSetOk && (keepSet.size() > 0)) {
		keepSet.finalize();
		gSyncMsg.set(dryRun ? "scanning for files to delete" : "removing files not in manifest");
		File root = gFSystem.open("/");
		if (root && root.isDirectory()) {
			syncMirrorDir(root, keepSet, deleted, dryRun, dryRun ? &dryReport : nullptr);
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

	if (dryRun) {
		if (mirror) {
			dryReport.printf("# %u files to download, %u files to delete, playlists: %u push/%u pull/%u delete/%u conflict, %u total\n", (unsigned) downloaded, (unsigned) deleted,
				(unsigned) playlistCounts.pushed, (unsigned) playlistCounts.pulled, (unsigned) playlistCounts.deleted, (unsigned) playlistCounts.conflicts, (unsigned) processed);
			gSyncMsg.setf("dry: %u dl/%u rm; PL %u up/%u down/%u conflict", (unsigned) downloaded, (unsigned) deleted, (unsigned) playlistCounts.pushed, (unsigned) playlistCounts.pulled,
				(unsigned) playlistCounts.conflicts);
		} else {
			dryReport.printf("# %u files to download, playlists: %u push/%u pull/%u delete/%u conflict, %u total (mirror-delete is off)\n", (unsigned) downloaded,
				(unsigned) playlistCounts.pushed, (unsigned) playlistCounts.pulled, (unsigned) playlistCounts.deleted, (unsigned) playlistCounts.conflicts, (unsigned) processed);
			gSyncMsg.setf("dry: %u dl; PL %u up/%u down/%u conflict", (unsigned) downloaded, (unsigned) playlistCounts.pushed, (unsigned) playlistCounts.pulled,
				(unsigned) playlistCounts.conflicts);
		}
		dryReport.close();
	} else if (mirror) {
		gSyncMsg.setf("%u dl/%u rm/%u fail; PL %u up/%u down/%u conflict", (unsigned) downloaded, (unsigned) deleted, (unsigned) failed, (unsigned) playlistCounts.pushed,
			(unsigned) playlistCounts.pulled, (unsigned) playlistCounts.conflicts);
	} else {
		gSyncMsg.setf("%u dl/%u fail; PL %u up/%u down/%u conflict", (unsigned) downloaded, (unsigned) failed, (unsigned) playlistCounts.pushed, (unsigned) playlistCounts.pulled,
			(unsigned) playlistCounts.conflicts);
	}
	char summary[StatusMessage::Capacity];
	Sync_CopyMessage(summary, sizeof(summary));
	Log_Printf(LOGLEVEL_NOTICE, "Sync %s%s: %s", dryRun ? "dry run " : "", cancelled ? "stopped" : "finished", summary);
	gSyncStatus = cancelled ? 4 : ((!dryRun && failed > 0) ? 3 : 2);
}

static void syncTask(void *parameter) {
	// Thin wrapper: syncRun() has many early-bail exit points, so run it to completion (its RAII locals
	// unwind as the stack does), THEN release the shared net slot and delete the task. This guarantees
	// the slot is freed on EVERY path without threading a release before each of syncRun's returns.
	// vTaskDelete(NULL) never returns, so it must come last, after syncRun() has already cleaned up.
	syncRun();
	Net_ReleaseBgJob(); // free the shared net slot on EVERY exit path of syncRun (only reached because we claimed it)
	vTaskDelete(NULL);
}

static void syncStart(bool dryRun) {
	// Atomically claim the idle->running transition: this is reachable from the web, Cmd and MQTT
	// tasks (different cores), so a plain check-then-set could double-start the task.
	static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
	portENTER_CRITICAL(&mux);
	if (gSyncStatus == 1) {
		portEXIT_CRITICAL(&mux);
		return; // already running
	}
	gSyncStatus = 1;
	portEXIT_CRITICAL(&mux);
	// Serialize against the other heavy net tasks (Backup/RfidSync/OTA): the sync task needs a 16 KB
	// stack plus a TLS client, so it must not run concurrently. Claim the shared slot AFTER the local
	// idle->running claim (so a double-start doesn't grab the global slot only to bail); Net_TryClaimBgJob()
	// may take a semaphore, so it must be OUTSIDE the portMUX critical section above. If another net job
	// holds it, reset our status back to idle and defer — the caller (web/Cmd/MQTT) retries, so a deferred
	// sync must not look "running" forever.
	if (!Net_TryClaimBgJob()) {
		gSyncStatus = 0; // back to idle, not failed: this is a deferral, not an error
		gSyncMsg.set("another network job is running, deferred");
		Log_Printf(LOGLEVEL_NOTICE, "Sync: another network job is running, deferring");
		return;
	}
	gSyncDryRun = dryRun;
	gSyncCancel = false;
	gSyncProgress = 0;
	gSyncMsg.set("");
	if (xTaskCreatePinnedToCore(syncTask, "httpSync", 16384, NULL, 1, NULL, 1) != pdPASS) {
		Net_ReleaseBgJob(); // task never started, so it can't release the slot -> do it here
		gSyncStatus = 3; // couldn't spawn -> release the slot as failed
	}
}

void Sync_Trigger(void) {
	syncStart(false);
}

// Same diff as a real sync but downloads/deletes nothing; writes a report of what it *would* do
// to kSyncDryReport (served via GET /syncreport). Lets the user preview a mirror-delete first.
void Sync_TriggerDryRun(void) {
	syncStart(true);
}
