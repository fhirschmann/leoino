#pragma once

#include <Arduino.h>

#include "ArduinoJson.h"
#include "AsyncJson.h"
#include "ESPAsyncWebServer.h"
#include "FS.h"
#include "JsonPsram.h"
#include "MemX.h"

#include <algorithm>
#include <string.h>
#include <utility>

// Internal interface between Web.cpp (route table + lifecycle) and the extracted web
// subsystems (WebExplorer.cpp = SD file-browser + chunked upload, WebRfid.cpp = RFID
// tag-assignment endpoints). Not part of the public Web.h API.

// Auth gate for streaming body/upload handlers (defined in Web.cpp). The password middleware
// does not cover the upload/body callback, which runs while the body streams in, so handlers that
// buffer bodies or write flash/NVS/SD must call this themselves before doing anything.
bool Web_RequestAuthorized(AsyncWebServerRequest *request);

// Drop-in replacement for AsyncJsonResponse that stores its JsonDocument in PSRAM (falling back to
// internal heap if PSRAM is absent) instead of exhausting internal DRAM on large responses (/rfid,
// /explorer, /settings, /info, /debug).
class PsramJsonResponse : public AsyncAbstractResponse {
private:
	SpiRamAllocator _allocator;
	JsonDocument _jsonBuffer;
	JsonVariant _root;
	bool _isValid;

public:
	explicit PsramJsonResponse(bool isArray = false)
		: _jsonBuffer(&_allocator)
		, _isValid(false) {
		_code = 200;
		_contentType = asyncsrv::T_application_json;
		if (isArray) {
			_root = _jsonBuffer.add<JsonArray>();
		} else {
			_root = _jsonBuffer.add<JsonObject>();
		}
	}

	JsonVariant &getRoot() {
		return _root;
	}

	bool _sourceValid() const override {
		return _isValid;
	}

	size_t setLength() {
		_contentLength = measureJson(_root);
		if (_contentLength) {
			_isValid = true;
		}
		return _contentLength;
	}

	size_t getSize() const {
		return _jsonBuffer.size();
	}

	bool overflowed() const {
		return _jsonBuffer.overflowed();
	}

	size_t _fillBuffer(uint8_t *data, size_t len) override {
		ChunkPrint dest(data, _sentLength, len);
		serializeJson(_root, dest);
		return dest.written();
	}
};

// Hardened replacement for ESPAsyncWebServer's AsyncCallbackJsonWebHandler:
// 1. Rejects unauthenticated body uploads BEFORE allocating memory (unless requireAuth == false, e.g. /login).
// 2. Restricts allowed methods to POST/PUT/PATCH by default (never dispatches HTTP_GET with an empty JsonVariant).
// 3. Bounds-checks every chunk against actual buffer capacity and _maxContentLength (fixing the upstream
//    X-Expected-Entity-Length + Transfer-Encoding: chunked internal-DRAM heap buffer overflow, while also
//    supporting standard chunked JSON bodies without X-Expected-Entity-Length).
// 4. Allocates both the raw request buffer and the parsed JsonDocument in PSRAM first.
class SafeJsonWebHandler : public AsyncWebHandler {
private:
	struct BodyBuffer {
		size_t len;
		size_t cap;
		bool overflowed;
		char data[1];
	};

	AsyncURIMatcher _uri;
	WebRequestMethodComposite _method;
	ArJsonRequestHandlerFunction _onRequest;
	size_t _maxContentLength;
	bool _requireAuth;

public:
	SafeJsonWebHandler(AsyncURIMatcher uri, ArJsonRequestHandlerFunction onRequest, size_t maxContentLength = 16384, bool requireAuth = true)
		: _uri(std::move(uri))
		, _method(AsyncWebRequestMethod::HTTP_POST | AsyncWebRequestMethod::HTTP_PUT | AsyncWebRequestMethod::HTTP_PATCH)
		, _onRequest(std::move(onRequest))
		, _maxContentLength(maxContentLength)
		, _requireAuth(requireAuth) {
	}

	void setMethod(WebRequestMethodComposite method) {
		_method = std::move(method);
	}

	void setMaxContentLength(size_t maxContentLength) {
		_maxContentLength = maxContentLength;
	}

	bool canHandle(AsyncWebServerRequest *request) const final {
		if (!_onRequest || !request->isHTTP() || !_method.matches(request->method())) {
			return false;
		}
		if (!_uri.matches(request)) {
			return false;
		}
		return request->contentType().equalsIgnoreCase(asyncsrv::T_application_json);
	}

	void handleUpload(AsyncWebServerRequest *, const String &, size_t, uint8_t *, size_t, bool) final {
	}

	void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) final {
		if (!_onRequest) {
			return;
		}
		if (_requireAuth && !Web_RequestAuthorized(request)) {
			return;
		}
		BodyBuffer *buf = static_cast<BodyBuffer *>(request->_tempObject);
		if (total > _maxContentLength) {
			if (!buf) {
				buf = static_cast<BodyBuffer *>(x_malloc(sizeof(BodyBuffer)));
				if (buf) {
					buf->len = 0;
					buf->cap = 0;
					buf->overflowed = true;
					buf->data[0] = '\0';
					request->_tempObject = buf;
				}
			} else {
				buf->overflowed = true;
			}
			return;
		}
		if (!buf) {
			if (len == 0 && total == 0) {
				return;
			}
			if (len > _maxContentLength) {
				buf = static_cast<BodyBuffer *>(x_malloc(sizeof(BodyBuffer)));
				if (buf) {
					buf->len = 0;
					buf->cap = 0;
					buf->overflowed = true;
					buf->data[0] = '\0';
					request->_tempObject = buf;
				}
				return;
			}
			size_t initCap = (total > 0 && total <= _maxContentLength) ? std::max(total, len) : std::min<size_t>(_maxContentLength, std::max<size_t>(len, 256));
			buf = static_cast<BodyBuffer *>(x_malloc(sizeof(BodyBuffer) + initCap));
			if (!buf) {
				request->abort();
				return;
			}
			buf->len = 0;
			buf->cap = initCap;
			buf->overflowed = false;
			buf->data[0] = '\0';
			request->_tempObject = buf;
		}
		if (buf->overflowed || len == 0) {
			return;
		}
		if (index != buf->len || len > (_maxContentLength - buf->len)) {
			buf->overflowed = true;
			return;
		}
		if (buf->len + len > buf->cap) {
			size_t newCap = std::min<size_t>(_maxContentLength, std::max<size_t>(buf->cap * 2, buf->len + len));
			BodyBuffer *grown = static_cast<BodyBuffer *>(x_realloc(buf, sizeof(BodyBuffer) + newCap));
			if (!grown) {
				request->abort();
				return;
			}
			buf = grown;
			buf->cap = newCap;
			request->_tempObject = buf;
		}
		memcpy(buf->data + buf->len, data, len);
		buf->len += len;
		buf->data[buf->len] = '\0';
	}

	void handleRequest(AsyncWebServerRequest *request) final {
		if (!_onRequest) {
			request->send(500);
			return;
		}
		if (_requireAuth && !Web_RequestAuthorized(request)) {
			request->send(401);
			return;
		}
		BodyBuffer *buf = static_cast<BodyBuffer *>(request->_tempObject);
		if (request->contentLength() > _maxContentLength || (buf && buf->overflowed)) {
			request->send(413);
			return;
		}
		if (!buf || buf->len == 0) {
			request->send(400);
			return;
		}
		SpiRamAllocator allocator;
		JsonDocument doc(&allocator);
		DeserializationError err = deserializeJson(doc, buf->data, buf->len);
		free(request->_tempObject);
		request->_tempObject = nullptr;
		if (err) {
			request->send(400);
			return;
		}
		JsonVariant json = doc.as<JsonVariant>();
		_onRequest(request, json);
	}

	bool isRequestHandlerTrivial() const final {
		return !_onRequest;
	}
};

// RFID tag-assignment handlers (registered as routes in Web.cpp::webserverStart, implemented
// in WebRfid.cpp).
void handleGetRFIDRequest(AsyncWebServerRequest *request);

// Serialize a single RFID tag (by NVS key) to its JSON object string; nameOnly omits the details.
// Owned by WebRfid.cpp, reused by Backup.cpp to stream the RFID section of a backup.
String tagIdToJsonStr(const char *key, const bool nameOnly);

// Serialize the device settings into obj. An empty section emits everything; otherwise just that
// section (e.g. "sync"). Defined in WebSettings.cpp, reused by Backup.cpp to build the settings
// section and by Web.cpp's websocket dispatcher.
void settingsToJSON(JsonObject obj, const String &section);

// Settings / equalizer-rule / operation-mode endpoints (registered as routes in
// Web.cpp::webserverStart, implemented in WebSettings.cpp).
void handleGetSettings(AsyncWebServerRequest *request);
void handleGetEqRules(AsyncWebServerRequest *request);
void handleSetEqRule(AsyncWebServerRequest *request);
void handleDeleteEqRule(AsyncWebServerRequest *request);
void handleGetOperationMode(AsyncWebServerRequest *request);
void handlePostOperationMode(AsyncWebServerRequest *request, JsonVariant &json);
void handlePostRFIDRequest(AsyncWebServerRequest *request, JsonVariant &json);
void handleDeleteRFIDRequest(AsyncWebServerRequest *request);
void handleResetRfidPos(AsyncWebServerRequest *request);

// NVS helpers owned by Web.cpp but reused by the extracted modules.
bool listNVSKeys(const char *_namespace, void *data, bool (*callback)(const char *key, void *data));
bool DumpNvsToArrayCallback(const char *key, void *data); // collects NVS keys into a std::vector<String>
bool Web_DumpNvsToSd(const char *_namespace, const char *_destFile); // mirror an NVS namespace to a backup file on SD

// File-browser handlers (registered as routes in Web.cpp::webserverStart).
void explorerHandleUploadComplete(AsyncWebServerRequest *request);
void explorerHandleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
void explorerHandleFileStorageTask(void *parameter);
void explorerHandleListRequest(AsyncWebServerRequest *request);
void explorerHandleDownloadRequest(AsyncWebServerRequest *request);
void explorerHandleDeleteRequest(AsyncWebServerRequest *request);
void explorerHandleCreateRequest(AsyncWebServerRequest *request);
void explorerHandleRenameRequest(AsyncWebServerRequest *request);
void explorerHandleAudioRequest(AsyncWebServerRequest *request);
void handleCleanSdRequest(AsyncWebServerRequest *request);
void handleFormatSdRequest(AsyncWebServerRequest *request);
void handleSdMaintStatusRequest(AsyncWebServerRequest *request);
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
