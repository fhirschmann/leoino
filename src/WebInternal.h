#pragma once

#include <Arduino.h>

#include "ArduinoJson.h"
#include "ESPAsyncWebServer.h"
#include "FS.h"

// Internal interface between Web.cpp (route table + lifecycle) and the extracted web
// subsystems (WebExplorer.cpp = SD file-browser + chunked upload, WebRfid.cpp = RFID
// tag-assignment endpoints). Not part of the public Web.h API.

// RFID tag-assignment handlers (registered as routes in Web.cpp::webserverStart, implemented
// in WebRfid.cpp).
void handleGetRFIDRequest(AsyncWebServerRequest *request);
void handlePostRFIDRequest(AsyncWebServerRequest *request, JsonVariant &json);
void handleDeleteRFIDRequest(AsyncWebServerRequest *request);
void handleResetRfidPos(AsyncWebServerRequest *request);

// NVS helpers owned by Web.cpp but reused by the extracted modules.
bool listNVSKeys(const char *_namespace, void *data, bool (*callback)(const char *key, void *data));
bool DumpNvsToArrayCallback(const char *key, void *data); // collects NVS keys into a std::vector<String>
bool Web_DumpNvsToSd(const char *_namespace, const char *_destFile); // mirror an NVS namespace to a backup file on SD

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
