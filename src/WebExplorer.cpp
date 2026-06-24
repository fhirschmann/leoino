#include <Arduino.h>
#include "settings.h"

#include "ArduinoJson.h"
#include "AsyncJson.h"
#include "AudioPlayer.h"
#include "Cmd.h"
#include "Common.h"
#include "ESPAsyncWebServer.h"
#include "Led.h"
#include "Log.h"
#include "MemX.h"
#include "Mqtt.h"
#include "Playstats.h"
#include "Rfid.h"
#include "SdCard.h"
#include "System.h"
#include "Web.h"
#include "WebInternal.h"

#include <atomic>
#include <esp_task_wdt.h>

// SD file-browser + chunked upload subsystem, split out of Web.cpp. The route table in
// Web.cpp registers the handlers declared in WebInternal.h; this file owns the upload
// double-buffer and talks back to the rest of the web server only through the public
// Web_* API (Web.h).

// NVS backup-restore entry, parsed out of an uploaded backup file by Web_DumpSdToNvs.
typedef struct {
	char nvsKey[cardIdStringSize];
	char nvsEntry[512];
} nvs_t;

static const uint32_t start_chunk_size = 16384; // bigger chunks increase write-performance to SD-Card
static constexpr uint32_t nr_of_buffers = 3; // at least two buffers. No speed improvement yet with more than two.

static constexpr size_t retry_count = 3; // how often we retry is a malloc fails (also the times we halfe the chunk_size)

uint8_t *buffer[nr_of_buffers];
size_t chunk_size;
std::atomic<uint32_t> size_in_buffer[nr_of_buffers];
std::atomic<bool> buffer_full[nr_of_buffers];
uint32_t index_buffer_write = 0;
uint32_t index_buffer_read = 0;

static SemaphoreHandle_t explorerFileUploadFinished;
static TaskHandle_t fileStorageTaskHandle;
static std::atomic<bool> uploadAborted = false;

// Aborts a running upload storage task and waits (briefly) for it to release its file
// handles. Called from Web_Exit during shutdown.
void WebExplorer_AbortUpload(void) {
	if (fileStorageTaskHandle != NULL) {
		uploadAborted = true;
		xTaskNotify(fileStorageTaskHandle, 2u, eSetValueWithOverwrite);
		uint32_t startWait = millis();
		while (fileStorageTaskHandle != NULL && (millis() - startWait < 2000)) {
			vTaskDelay(pdMS_TO_TICKS(50));
		}
	}
}

void destroyDoubleBuffer() {
	for (size_t i = 0; i < nr_of_buffers; i++) {
		free(buffer[i]);
		buffer[i] = nullptr;
	}
}

bool allocateDoubleBuffer() {
	const auto checkAndAlloc = [](uint8_t *&ptr, const size_t memSize) -> bool {
		if (ptr) {
			// memory is there, so nothing to do
			return true;
		}
		// try to allocate buffer in faster internal RAM, not in PSRAM
		// ptr = (uint8_t *) malloc(memSize);
		ptr = (uint8_t *) heap_caps_aligned_alloc(32, memSize, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
		return (ptr != nullptr);
	};

	chunk_size = start_chunk_size;
	size_t retries = retry_count;
	while (retries) {
		if (chunk_size < 256) {
			// give up, since there is not even 256 bytes of memory left
			break;
		}
		bool success = true;
		for (size_t i = 0; i < nr_of_buffers; i++) {
			success &= checkAndAlloc(buffer[i], chunk_size);
		}
		if (success) {
			return true;
		} else {
			// one of our buffer went OOM --> free all buffer and retry with less chunk size
			destroyDoubleBuffer();
			chunk_size /= 2;
			retries--;
		}
	}
	destroyDoubleBuffer();
	return false;
}

void handleUploadError(AsyncWebServerRequest *request, int code) {
	if (request->_tempObject) {
		// we already have an error entered
		return;
	}
	// send the error to the client and record it in the request
	request->_tempObject = new int(code);
	request->send(code);
}

// Handles file upload request from the explorer
// requires a GET parameter path, as directory path to the file
void explorerHandleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {

	// This upload callback runs while the body streams in, before the password middleware. Refuse
	// to write anything to SD for an unauthenticated client; the completion handler returns 401.
	if (!Web_RequestAuthorized(request)) {
		uploadAborted = true;
		return;
	}

	System_UpdateActivityTimer();

	// New File
	if (!index) {
		// reset abort flag
		uploadAborted = false;

		String utf8Folder = "/";
		String utf8FilePath;
		if (request->hasParam("path")) {
			const AsyncWebParameter *param = request->getParam("path");
			utf8Folder = param->value() + "/";
		}
		utf8FilePath = utf8Folder + filename;

		const char *filePath = utf8FilePath.c_str();

		Log_Printf(LOGLEVEL_INFO, writingFile, filePath);

		if (!allocateDoubleBuffer()) {
			// we failed to allocate enough memory
			Log_Println(unableToAllocateMem, LOGLEVEL_ERROR);
			handleUploadError(request, 500);
			return;
		}

		// Create Queue for receiving a signal from the store task as synchronisation
		if (explorerFileUploadFinished == NULL) {
			explorerFileUploadFinished = xSemaphoreCreateBinary();
		} else {
			// make sure semaphore is empty
			xSemaphoreTake(explorerFileUploadFinished, 0);
		}

		// reset buffers
		index_buffer_write = 0;
		index_buffer_read = 0;
		for (uint32_t i = 0; i < nr_of_buffers; i++) {
			size_in_buffer[i] = 0;
			buffer_full[i] = false;
		}

		// Create Task for handling the storage of the data
		const char *filePathCopy = x_strdup(filePath);
		xTaskCreatePinnedToCore(
			explorerHandleFileStorageTask, /* Function to implement the task */
			"fileStorageTask", /* Name of the task */
			4000, /* Stack size in words */
			(void *) filePathCopy, /* Task input parameter */
			2 | portPRIVILEGE_BIT, /* Priority of the task */
			&fileStorageTaskHandle, /* Task handle. */
			1 /* Core where the task should run */
		);

		// register for early disconnect events
		request->onDisconnect([]() {
			// client went away before we were finished...
			// trigger task suicide, since we can not use Log_Println here
			xTaskNotify(fileStorageTaskHandle, 2u, eSetValueWithOverwrite);
		});
	}

	if (uploadAborted) {
		if (!request->_tempObject) {
			handleUploadError(request, 500);
		}
		return;
	}

	if (len) {
		// wait till buffer is ready
		while (buffer_full[index_buffer_write]) {
			if (uploadAborted) {
				return;
			}
			vTaskDelay(2u);
		}

		size_t len_to_write = len;
		size_t space_left = chunk_size - size_in_buffer[index_buffer_write];
		if (space_left < len_to_write) {
			len_to_write = space_left;
		}
		// write content to buffer
		memcpy(buffer[index_buffer_write] + size_in_buffer[index_buffer_write], data, len_to_write);
		size_in_buffer[index_buffer_write] = size_in_buffer[index_buffer_write] + len_to_write;

		// check if buffer is filled. If full, signal that ready and change buffers
		if (size_in_buffer[index_buffer_write] == chunk_size) {
			// signal, that buffer is ready. Increment index
			buffer_full[index_buffer_write] = true;
			index_buffer_write = (index_buffer_write + 1) % nr_of_buffers;

			// if still content left, put it into next buffer
			if (len_to_write < len) {
				// wait till new buffer is ready
				while (buffer_full[index_buffer_write]) {
					if (uploadAborted) {
						return;
					}
					vTaskDelay(2u);
				}
				size_t len_left_to_write = len - len_to_write;
				memcpy(buffer[index_buffer_write], data + len_to_write, len_left_to_write);
				size_in_buffer[index_buffer_write] = len_left_to_write;
			}
		}
	}

	if (final) {
		if (uploadAborted) {
			handleUploadError(request, 500);
			return;
		}
		// if file not completely done yet, signal that buffer is filled
		if (size_in_buffer[index_buffer_write] > 0) {
			buffer_full[index_buffer_write] = true;
		}
		// notify storage task that last data was stored on the ring buffer
		xTaskNotify(fileStorageTaskHandle, 1u, eSetValueWithOverwrite);
		// watit until the storage task is sending the signal to finish
		if (xSemaphoreTake(explorerFileUploadFinished, pdMS_TO_TICKS(30000)) != pdTRUE) {
			// timeout, something went wrong
			Log_Println(webTxCanceled, LOGLEVEL_ERROR);
			handleUploadError(request, 500);
			return;
		}
	}
}

// task for writing uploaded data from buffer to SD
// parameter contains the target file path and must be freed by the task.
void explorerHandleFileStorageTask(void *parameter) {
	const char *filePath = (const char *) parameter;
	File uploadFile;
	size_t bytesOk = 0;
	uint32_t chunkCount = 0;
	uint32_t transferStartTimestamp = millis();
	uint32_t lastUpdateTimestamp = millis();
	uint32_t maxUploadDelay = 30; // After this delay (in seconds) task will be deleted as transfer is considered to be finally broken

	BaseType_t uploadFileNotification;
	uint32_t uploadFileNotificationValue;

	// pause some tasks to get more free CPU time for the upload
	System_PauseTasksDuringUpload(true);

	uploadFile = gFSystem.open(filePath, "w", true); // open file with create=true to make sure parent directories are created
	if (uploadFile) {
		uploadFile.setBufferSize(chunk_size);
	} else {
		Log_Printf(LOGLEVEL_ERROR, "Failed to open file %s for writing!", filePath);
		uploadAborted = true;
	}

	for (;;) {
		if (uploadAborted) {
			break;
		}
		// check buffer is full with enough data or all data already sent
		uploadFileNotification = xTaskNotifyWait(0, 0, &uploadFileNotificationValue, 0);
		if ((buffer_full[index_buffer_read]) || (uploadFileNotification == pdPASS && uploadFileNotificationValue == 1u)) {

			while (buffer_full[index_buffer_read]) {
				chunkCount++;
				size_t item_size = size_in_buffer[index_buffer_read];
				size_t written = 0;
				if (item_size > 0) {
					const uint8_t maxRetries = 3;
					for (uint8_t attempt = 0; attempt < maxRetries && written != item_size; attempt++) {
						if (attempt > 0) {
							Log_Printf(LOGLEVEL_DEBUG, "Write retry %u for chunk %zu on %s", attempt, chunkCount, filePath);
							vTaskDelay(pdMS_TO_TICKS(20 * attempt)); // backoff: 20ms, 40ms
						}
						written = uploadFile.write(buffer[index_buffer_read], item_size);
					}
				}

				if (item_size > 0 && written != item_size) {
					Log_Printf(LOGLEVEL_ERROR, "Write error during upload of %s! (expected %u, wrote %u after retries)",
						filePath, item_size, (uint32_t) written);
					uploadAborted = true;
					break;
				} else {
					bytesOk += written;
				}
				// update handling of buffers
				size_in_buffer[index_buffer_read] = 0;
				buffer_full[index_buffer_read] = 0;
				index_buffer_read = (index_buffer_read + 1) % nr_of_buffers;
				if (chunkCount % 64 == 0) {
					uploadFile.flush();
					System_UpdateActivityTimer();
				}
				// update timestamp
				lastUpdateTimestamp = millis();
			}

			if (uploadFileNotification == pdPASS) {
				if (uploadFile) {
					uploadFile.close();
				}
				Log_Printf(LOGLEVEL_INFO, fileWritten, filePath, bytesOk, (millis() - transferStartTimestamp), (bytesOk) / (millis() - transferStartTimestamp));
				Log_Printf(LOGLEVEL_DEBUG, "Bytes [ok] %zu, Chunks: %zu\n", bytesOk, chunkCount);
				// done exit loop to terminate
				break;
			}
		} else {
			if ((lastUpdateTimestamp + (maxUploadDelay * 1000)) < millis() || ((uploadFileNotification == pdPASS) && (uploadFileNotificationValue == 2u))) {
				Log_Println(webTxCanceled, LOGLEVEL_ERROR);
				if (uploadFile) {
					uploadFile.close();
				}
				uploadAborted = true;
				xSemaphoreGive(explorerFileUploadFinished);
				break;
			}
			vTaskDelay(portTICK_PERIOD_MS * 2);
			continue;
		}
	}
	free(parameter);
	// resume the paused tasks
	System_PauseTasksDuringUpload(false);
	System_UpdateActivityTimer();
	// send signal to upload function to terminate
	xSemaphoreGive(explorerFileUploadFinished);
	fileStorageTaskHandle = NULL;
	vTaskDelete(NULL);
}

// Sends a list of the content of a directory as JSON file
// requires a GET parameter path for the directory
void explorerHandleListRequest(AsyncWebServerRequest *request) {
#ifdef NO_SDCARD
	request->send(200, "application/json; charset=utf-8", "[]"); // maybe better to send 404 here?
	return;
#endif

	File root;
	bool isRoot = false;
	if (request->hasParam("path")) {
		const AsyncWebParameter *param;
		param = request->getParam("path");
		const char *filePath = param->value().c_str();
		if (strcmp(filePath, "/") == 0) {
			isRoot = true;
		}
		root = gFSystem.open(filePath);
	} else {
		root = gFSystem.open("/");
		isRoot = true;
	}

	if (!root) {
		Log_Println(failedToOpenDirectory, LOGLEVEL_DEBUG);
		return;
	}

	if (!root.isDirectory()) {
		Log_Println(notADirectory, LOGLEVEL_DEBUG);
		return;
	}

	AsyncJsonResponse *response = new AsyncJsonResponse(true);
	JsonArray obj = response->getRoot();

	// For root directory, add volume label as first element if available
	if (isRoot) {
		String volumeLabel = SdCard_GetVolumeLabel();
		if (volumeLabel.length() > 0) {
			JsonObject labelEntry = obj.add<JsonObject>();
			labelEntry["name"] = volumeLabel;
			labelEntry["root"] = "sd";
		}
	}

	bool isDir = false;
	String MyfileName = gFSystem.nextFileName(root, &isDir);
	while (MyfileName != "") {
		// ignore hidden folders, e.g. MacOS spotlight files
		if (!MyfileName.startsWith("/.")) {
			JsonObject entry = obj.add<JsonObject>();
			entry["name"] = MyfileName.substring(MyfileName.lastIndexOf('/') + 1);
			if (isDir) {
				entry["dir"].set(true);
			}
		}
		MyfileName = gFSystem.nextFileName(root, &isDir);
	}
	root.close();

	if (response->overflowed()) {
		// JSON buffer too small for data
		Log_Println(jsonbufferOverflow, LOGLEVEL_ERROR);
		request->send(500);
		return;
	}
	response->setLength();
	request->send(response);
}

bool explorerDeleteDirectory(File dir) {

	File file = dir.openNextFile();
	while (file) {

		if (file.isDirectory()) {
			explorerDeleteDirectory(file);
		} else {
			gFSystem.remove(file);
		}

		file = dir.openNextFile();

		esp_task_wdt_reset();
	}

	return gFSystem.rmdir(dir);
}

// macOS metadata junk: Finder/Spotlight droppings that are useless on a music box
static bool Web_IsMacJunkName(const char *name) {
	return (strcmp(name, ".DS_Store") == 0) || (strncmp(name, "._", 2) == 0) || (strcmp(name, ".Spotlight-V100") == 0) || (strcmp(name, ".fseventsd") == 0) || (strcmp(name, ".Trashes") == 0) || (strcmp(name, ".TemporaryItems") == 0);
}

static void Web_CleanDirectory(File dir, uint32_t &deletedCount) {
	File file = dir.openNextFile();
	while (file) {
		const String path = file.path();
		const bool isDir = file.isDirectory();
		if (Web_IsMacJunkName(file.name())) {
			file.close();
			if (isDir) {
				File junkDir = gFSystem.open(path);
				if (junkDir && explorerDeleteDirectory(junkDir)) {
					deletedCount++;
				}
			} else if (gFSystem.remove(path)) {
				deletedCount++;
			}
		} else if (isDir) {
			Web_CleanDirectory(file, deletedCount);
			file.close();
		} else {
			file.close();
		}
		file = dir.openNextFile();
		esp_task_wdt_reset();
	}
}

// Handles the request to remove macOS metadata junk from the SD card
void handleCleanSdRequest(AsyncWebServerRequest *request) {
	uint32_t deletedCount = 0;
	// The full-tree sweep is heavy SD I/O; pause the RFID/LED/audio tasks for its duration so the
	// audio task doesn't fight for the SD bus (dropouts) and an RFID tap can't start playback in the
	// middle of the clean. (This handler still runs on the async task; the pause is the key fix.)
	System_PauseTasksDuringUpload(true);
	File root = gFSystem.open("/");
	if (root && root.isDirectory()) {
		Web_CleanDirectory(root, deletedCount);
	}
	System_PauseTasksDuringUpload(false);
	Log_Printf(LOGLEVEL_NOTICE, "SD cleanup: removed %lu entries", (unsigned long) deletedCount);
	AsyncJsonResponse *response = new AsyncJsonResponse(false);
	response->getRoot()["deleted"] = deletedCount;
	response->setLength();
	request->send(response);
}

// Handles download request of a file
// requires a GET parameter path to the file
void explorerHandleDownloadRequest(AsyncWebServerRequest *request) {
	File file;
	const AsyncWebParameter *param;
	// check has path param
	if (!request->hasParam("path")) {
		Log_Println("DOWNLOAD: No path variable set", LOGLEVEL_ERROR);
		request->send(404);
		return;
	}
	// check file exists on SD card
	param = request->getParam("path");
	const char *filePath = param->value().c_str();
	if (!gFSystem.exists(filePath)) {
		Log_Printf(LOGLEVEL_ERROR, "DOWNLOAD:  File not found on SD card: %s", filePath);
		request->send(404);
		return;
	}
	// check is file and not a directory
	file = gFSystem.open(filePath);
	if (file.isDirectory()) {
		Log_Printf(LOGLEVEL_ERROR, "DOWNLOAD:  Cannot download a directory %s", filePath);
		request->send(404);
		file.close();
		return;
	}

	// ready to serve the file for download.
	String dataType = "application/octet-stream";
	struct fileBlk {
		File dataFile;
	};
	fileBlk *fileObj = new fileBlk;
	fileObj->dataFile = file;
	request->_tempObject = (void *) fileObj;

	AsyncWebServerResponse *response = request->beginResponse(dataType, fileObj->dataFile.size(), [request](uint8_t *buffer, size_t maxlen, size_t index) -> size_t {
		fileBlk *fileObj = (fileBlk *) request->_tempObject;
		size_t thisSize = fileObj->dataFile.read(buffer, maxlen);
		if ((index + thisSize) >= fileObj->dataFile.size()) {
			fileObj->dataFile.close();
			request->_tempObject = NULL;
			delete fileObj;
		}
		return thisSize;
	});
	String filename = String(param->value().c_str());
	response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
	request->send(response);
}

// Handles delete request of a file or directory
// requires a GET parameter path to the file or directory
void explorerHandleDeleteRequest(AsyncWebServerRequest *request) {
	File file;
	if (request->hasParam("path")) {
		const AsyncWebParameter *param;
		param = request->getParam("path");
		System_UpdateActivityTimer();
		const char *filePath = param->value().c_str();
		if (gFSystem.exists(filePath)) {
			// stop playback, file to delete might be in use
			Cmd_Action(CMD_STOP);
			file = gFSystem.open(filePath);
			if (file.isDirectory()) {
				if (explorerDeleteDirectory(file)) {
					Log_Printf(LOGLEVEL_INFO, "DELETE:  %s deleted", filePath);
				} else {
					Log_Printf(LOGLEVEL_ERROR, "DELETE:  Cannot delete %s", filePath);
				}
			} else {
				if (gFSystem.remove(filePath)) {
					Log_Printf(LOGLEVEL_INFO, "DELETE:  %s deleted", filePath);
				} else {
					Log_Printf(LOGLEVEL_ERROR, "DELETE:  Cannot delete %s", filePath);
				}
			}
		} else {
			Log_Printf(LOGLEVEL_ERROR, "DELETE:  Path %s does not exist", filePath);
		}
	} else {
		Log_Println("DELETE:  No path variable set", LOGLEVEL_ERROR);
	}
	request->send(200);
	esp_task_wdt_reset();
}

// Handles create request of a directory
// requires a GET parameter path to the new directory
void explorerHandleCreateRequest(AsyncWebServerRequest *request) {
	if (request->hasParam("path")) {
		const AsyncWebParameter *param;
		System_UpdateActivityTimer();
		param = request->getParam("path");
		const char *filePath = param->value().c_str();
		if (gFSystem.mkdir(filePath)) {
			Log_Printf(LOGLEVEL_INFO, "CREATE:  %s created", filePath);
		} else {
			Log_Printf(LOGLEVEL_ERROR, "CREATE:  Cannot create %s", filePath);
		}
	} else {
		Log_Println("CREATE:  No path variable set", LOGLEVEL_ERROR);
	}
	request->send(200);
}

// Writes an .m3u playlist to the SD card. Body: {"path":"/Playlists/x.m3u","tracks":["/dir/a.mp3","http://stream", ...]}
// Each track becomes one line; the player's LOCAL_M3U mode plays SD files and webradio URLs alike.
void handleCreatePlaylistRequest(AsyncWebServerRequest *request, JsonVariant &json) {
	JsonObject obj = json.as<JsonObject>();
	String path = obj["path"] | "";
	JsonArray tracks = obj["tracks"].as<JsonArray>();
	if (path.length() == 0 || tracks.isNull()) {
		request->send(400, "text/plain", "missing path or tracks");
		return;
	}
	if (!path.startsWith("/")) {
		path = "/" + path;
	}
	if (!path.endsWith(".m3u")) {
		path += ".m3u";
	}
	System_UpdateActivityTimer();
	// create the parent directory (one level, e.g. /Playlists) if it doesn't exist yet
	const int slash = path.lastIndexOf('/');
	if (slash > 0) {
		const String dir = path.substring(0, slash);
		if (!gFSystem.exists(dir)) {
			gFSystem.mkdir(dir);
		}
	}
	File file = gFSystem.open(path, "w", true);
	if (!file) {
		Log_Printf(LOGLEVEL_ERROR, "PLAYLIST: cannot create %s", path.c_str());
		request->send(500, "text/plain", "cannot create file");
		return;
	}
	file.print("#EXTM3U\n");
	for (JsonVariant t : tracks) {
		String line = t.as<String>();
		line.trim();
		if (line.length() > 0) {
			file.print(line);
			file.print("\n");
		}
	}
	file.close();
	Log_Printf(LOGLEVEL_NOTICE, "PLAYLIST: wrote %s (%u entries)", path.c_str(), (unsigned) tracks.size());
	request->send(200, "application/json", "{\"ok\":true}");
}

// Handles rename request of a file or directory
// requires a GET parameter srcpath to the old file or directory name
// requires a GET parameter dstpath to the new file or directory name
void explorerHandleRenameRequest(AsyncWebServerRequest *request) {
	if (request->hasParam("srcpath") && request->hasParam("dstpath")) {
		const AsyncWebParameter *srcPath;
		const AsyncWebParameter *dstPath;
		System_UpdateActivityTimer();
		srcPath = request->getParam("srcpath");
		dstPath = request->getParam("dstpath");
		const char *srcFullFilePath = srcPath->value().c_str();
		const char *dstFullFilePath = dstPath->value().c_str();
		if (gFSystem.exists(srcFullFilePath)) {
			if (gFSystem.rename(srcFullFilePath, dstFullFilePath)) {
				Log_Printf(LOGLEVEL_INFO, "RENAME:  %s renamed to %s", srcFullFilePath, dstFullFilePath);
			} else {
				Log_Printf(LOGLEVEL_ERROR, "RENAME:  Cannot rename %s", srcFullFilePath);
			}
		} else {
			Log_Printf(LOGLEVEL_ERROR, "RENAME: Path %s does not exist", srcFullFilePath);
		}
	} else {
		Log_Println("RENAME: No path variable set", LOGLEVEL_ERROR);
	}

	request->send(200);
}

// Handles audio play requests
// requires a GET parameter path to the audio file or directory
// requires a GET parameter playmode
void explorerHandleAudioRequest(AsyncWebServerRequest *request) {
	const AsyncWebParameter *param;
	String playModeString;
	uint32_t playMode;
	if (request->hasParam("path") && request->hasParam("playmode")) {
		if (!System_IsWebControlAllowed()) {
			Log_Println(notAllowedInCurrentMode, LOGLEVEL_NOTICE);
			Web_SendWebsocketData(0, WebsocketCodeType::NotAllowedInCurrentMode);
			request->send(200);
			return;
		}
		param = request->getParam("path");
		const char *filePath = param->value().c_str();
		param = request->getParam("playmode");
		playModeString = param->value();

		playMode = atoi(playModeString.c_str());
		Rfid_ResetOldRfid();
		AudioPlayer_SetPlaylist(filePath, 0, playMode, 0);
	} else {
		Log_Println("AUDIO: No path variable set", LOGLEVEL_ERROR);
	}

	request->send(200);
}

// Takes stream from file-upload and writes payload into a temporary sd-file.
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
	static File tmpFile;
	static size_t fileIndex = 0;
	static char tmpFileName[13];
	// Runs before the password middleware (body streams in first), so an unauthenticated client
	// could otherwise inject RFID assignments into NVS. Refuse to buffer or import anything.
	if (!Web_RequestAuthorized(request)) {
		return;
	}
	esp_task_wdt_reset();
	if (!index) {
		snprintf(tmpFileName, 13, "/_%lu", millis());
		tmpFile = gFSystem.open(tmpFileName, FILE_WRITE);
	} else {
		tmpFile.seek(fileIndex);
	}

	if (!tmpFile) {
		Log_Println(errorWritingTmpfile, LOGLEVEL_ERROR);
		return;
	}

	size_t wrote = tmpFile.write(data, len);
	if (wrote != len) {
		// we did not write all bytes --> fail
		Log_Printf(LOGLEVEL_ERROR, "Error writing %s. Expected %u, wrote %u (error: %u)!", tmpFile.path(), len, wrote, tmpFile.getWriteError());
		return;
	}
	fileIndex += wrote;

	if (final) {
		tmpFile.close();
		Web_DumpSdToNvs(tmpFileName);
		fileIndex = 0;
	}
}

// Parses content of temporary backup-file and writes payload into NVS
void Web_DumpSdToNvs(const char *_filename) {
	char ebuf[290];
	uint16_t j = 0;
	char *token;
	bool count = false;
	uint16_t importCount = 0;
	uint16_t invalidCount = 0;
	nvs_t nvsEntry[1];
	File tmpFile = gFSystem.open(_filename);

	if (!tmpFile || (tmpFile.available() < 3)) {
		Log_Println(errorReadingTmpfile, LOGLEVEL_ERROR);
		return;
	}

	Led_SetPause(true);
	// try to read UTF-8 BOM marker
	bool isUtf8 = (tmpFile.read() == 0xEF) && (tmpFile.read() == 0xBB) && (tmpFile.read() == 0xBF);
	if (!isUtf8) {
		// no BOM found, reset to start of file
		tmpFile.seek(0);
	}

	while (tmpFile.available() > 0) {
		if (j >= sizeof(ebuf)) {
			Log_Println(errorReadingTmpfile, LOGLEVEL_ERROR);
			return;
		}
		char buf = tmpFile.read();
		if (buf != '\n') {
			ebuf[j++] = buf;
		} else {
			ebuf[j] = '\0';
			j = 0;
			token = strtok(ebuf, stringOuterDelimiter);
			while (token != NULL) {
				if (!count) {
					count = true;
					size_t keyLen = std::min(strlen(token), sizeof(nvsEntry[0].nvsKey) - 1);
					memcpy(nvsEntry[0].nvsKey, token, keyLen);
					nvsEntry[0].nvsKey[keyLen] = '\0';
				} else {
					count = false;
					if (isUtf8) {
						size_t entryLen = std::min(strlen(token), sizeof(nvsEntry[0].nvsEntry) - 1);
						memcpy(nvsEntry[0].nvsEntry, token, entryLen);
						nvsEntry[0].nvsEntry[entryLen] = '\0';
					} else {
						convertAsciiToUtf8(String(token), nvsEntry[0].nvsEntry, sizeof(nvsEntry[0].nvsEntry));
					}
				}
				token = strtok(NULL, stringOuterDelimiter);
			}
			if (isNumber(nvsEntry[0].nvsKey) && nvsEntry[0].nvsEntry[0] == '#') {
				Log_Printf(LOGLEVEL_NOTICE, writeEntryToNvs, ++importCount, nvsEntry[0].nvsKey, nvsEntry[0].nvsEntry);
				gPrefsRfid.putString(nvsEntry[0].nvsKey, nvsEntry[0].nvsEntry);
			} else {
				invalidCount++;
			}
		}
	}

	Led_SetPause(false);
	Log_Printf(LOGLEVEL_NOTICE, importCountNokNvs, invalidCount);
	tmpFile.close();
	gFSystem.remove(_filename);
}
