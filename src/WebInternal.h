#pragma once

#include <Arduino.h>

#include "ArduinoJson.h"
#include "ESPAsyncWebServer.h"
#include "FS.h"

// Internal interface between Web.cpp (route table + lifecycle) and WebExplorer.cpp
// (SD file-browser + chunked upload subsystem). Not part of the public Web.h API.

// File-browser handlers (registered as routes in Web.cpp::webserverStart).
void explorerHandleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
void explorerHandleFileStorageTask(void *parameter);
void explorerHandleListRequest(AsyncWebServerRequest *request);
void explorerHandleDownloadRequest(AsyncWebServerRequest *request);
void explorerHandleDeleteRequest(AsyncWebServerRequest *request);
void explorerHandleCreateRequest(AsyncWebServerRequest *request);
void explorerHandleRenameRequest(AsyncWebServerRequest *request);
void explorerHandleAudioRequest(AsyncWebServerRequest *request);
void handleCleanSdRequest(AsyncWebServerRequest *request);
void handleCreatePlaylistRequest(AsyncWebServerRequest *request, JsonVariant &json);
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);

// Upload double-buffer helpers + backup restore, also reached from Web.cpp.
void destroyDoubleBuffer();
bool allocateDoubleBuffer();
void handleUploadError(AsyncWebServerRequest *request, int code);
bool explorerDeleteDirectory(File dir);
void Web_DumpSdToNvs(const char *_filename);

// Aborts a running file-upload storage task and waits for it to exit (used by Web_Exit).
void WebExplorer_AbortUpload(void);
