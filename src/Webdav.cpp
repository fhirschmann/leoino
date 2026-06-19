#include <Arduino.h>
#include "settings.h"

#include "Webdav.h"

#include "Log.h"
#include "SdCard.h"
#include "System.h"
#include "Wlan.h"

#include <WiFi.h>
#include <mbedtls/base64.h>

// A compact, self-contained WebDAV/1,2 server over gFSystem (the sanitized SD filesystem). It
// supports the methods Finder/Explorer need to mount the card read/write: OPTIONS, PROPFIND,
// GET/HEAD (incl. byte ranges), PUT, DELETE, MKCOL, MOVE, COPY and stub LOCK/UNLOCK/PROPPATCH.
// Everything runs inside one FreeRTOS task on core 0; requests are handled synchronously there so
// large copies block only this task, never the audio pipeline on core 1.

#ifdef WEBDAV_ENABLE

static WiFiServer *webdavServer = nullptr;
static TaskHandle_t webdavTaskHandle = nullptr;
static volatile bool webdavShouldRun = false;
static volatile bool webdavRunning = false;

String Webdav_User = "esp32"; // default; kept for compatibility but ignored for auth (any username is accepted)
String Webdav_Password = "esp32"; // the shared device password (set on the Security tab)
static bool webdavAuthRequired = false; // true when a password is set; computed in webdavComputeAuth()
static bool webdavAutostart = false; // start automatically on boot (persisted setting)

static constexpr size_t WEBDAV_BUFFER_SIZE = 2048;

// ---------------------------------------------------------------------------- helpers

// Auth is required only when a password is set. The username is ignored (any username is accepted),
// so the check decodes the HTTP Basic header per request and compares only the password.
static void webdavComputeAuth(void) {
	webdavAuthRequired = !Webdav_Password.isEmpty();
}

// Returns true if the request may proceed: no password set (open drive), or the password part of the
// "Authorization: Basic <base64(user:password)>" header matches the configured password. Any username
// is accepted - only the password matters.
static bool webdavCheckAuth(const String &authz) {
	if (!webdavAuthRequired) {
		return true;
	}
	if (!authz.startsWith("Basic ")) {
		return false;
	}
	String b64 = authz.substring(6);
	b64.trim();
	unsigned char dec[160];
	size_t dlen = 0;
	if (mbedtls_base64_decode(dec, sizeof(dec), &dlen, (const unsigned char *) b64.c_str(), b64.length()) != 0) {
		return false;
	}
	String cred = String((const char *) dec, dlen);
	int colon = cred.indexOf(':');
	if (colon < 0) {
		return false;
	}
	return cred.substring(colon + 1) == Webdav_Password;
}

static int webdavFromHex(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

// Percent-decode a URL path component into the logical filesystem path (gFSystem re-sanitizes).
static String webdavDecode(const String &in) {
	String out;
	out.reserve(in.length());
	for (size_t i = 0; i < in.length(); i++) {
		char c = in[i];
		if (c == '%' && i + 2 < in.length()) {
			int hi = webdavFromHex(in[i + 1]);
			int lo = webdavFromHex(in[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out += (char) ((hi << 4) | lo);
				i += 2;
				continue;
			}
		}
		if (c == '+') {
			out += ' ';
		} else {
			out += c;
		}
	}
	return out;
}

// Percent-encode a logical path for use in an XML <href>. Keeps '/' and the unreserved set.
static String webdavEncodeHref(const String &in) {
	static const char *hex = "0123456789ABCDEF";
	String out;
	out.reserve(in.length() + 8);
	for (size_t i = 0; i < in.length(); i++) {
		unsigned char c = (unsigned char) in[i];
		if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
			out += (char) c;
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 0x0F];
		}
	}
	return out;
}

// Turn a request URI (or Destination header) into a normalized logical path: strip scheme/host,
// drop the query, percent-decode, collapse to a leading-slash path with no trailing slash (except root).
static String webdavUriToPath(const String &uriIn) {
	String uri = uriIn;
	int scheme = uri.indexOf("://");
	if (scheme >= 0) {
		int slash = uri.indexOf('/', scheme + 3);
		uri = (slash >= 0) ? uri.substring(slash) : "/";
	}
	int q = uri.indexOf('?');
	if (q >= 0) {
		uri = uri.substring(0, q);
	}
	String path = webdavDecode(uri);
	if (path.isEmpty()) {
		path = "/";
	}
	while (path.length() > 1 && path.endsWith("/")) {
		path.remove(path.length() - 1);
	}
	return path;
}

static String webdavBaseName(const String &path) {
	int slash = path.lastIndexOf('/');
	return (slash >= 0) ? path.substring(slash + 1) : path;
}

static String webdavHttpDate(time_t t) {
	struct tm g;
	gmtime_r(&t, &g);
	char buf[40];
	strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &g);
	return String(buf);
}

static const char *webdavContentType(const String &path) {
	String p = path;
	p.toLowerCase();
	if (p.endsWith(".mp3")) {
		return "audio/mpeg";
	}
	if (p.endsWith(".m4a") || p.endsWith(".aac")) {
		return "audio/mp4";
	}
	if (p.endsWith(".ogg") || p.endsWith(".opus")) {
		return "audio/ogg";
	}
	if (p.endsWith(".flac")) {
		return "audio/flac";
	}
	if (p.endsWith(".wav")) {
		return "audio/wav";
	}
	if (p.endsWith(".m3u") || p.endsWith(".m3u8")) {
		return "audio/x-mpegurl";
	}
	if (p.endsWith(".jpg") || p.endsWith(".jpeg")) {
		return "image/jpeg";
	}
	if (p.endsWith(".png")) {
		return "image/png";
	}
	if (p.endsWith(".txt")) {
		return "text/plain";
	}
	return "application/octet-stream";
}

static void webdavSendStatus(WiFiClient &client, int code, const char *reason) {
	client.printf("HTTP/1.1 %d %s\r\n", code, reason);
	client.print("Connection: close\r\n");
	client.print("DAV: 1, 2\r\n");
	client.print("Content-Length: 0\r\n\r\n");
}

// Discard up to <n> bytes of a request body we don't consume, so the client receives our full
// response before the socket is closed (a reset with unread data can truncate the reply).
static void webdavDrain(WiFiClient &client, long n) {
	uint8_t tmp[256];
	while (n > 0 && client.connected()) {
		int want = (n > (long) sizeof(tmp)) ? (int) sizeof(tmp) : (int) n;
		int got = client.read(tmp, want);
		if (got <= 0) {
			if (!client.available()) {
				break;
			}
			continue;
		}
		n -= got;
	}
}

// ---------------------------------------------------------------------------- PROPFIND

// Append one <D:response> element describing <logicalPath> (a file or directory) to <body>.
static void webdavAppendResponse(String &body, const String &logicalPath, bool isDir, uint32_t size, time_t mtime) {
	String href = webdavEncodeHref(logicalPath);
	if (href.isEmpty()) {
		href = "/";
	}
	if (isDir && !href.endsWith("/")) {
		href += "/";
	}
	body += "<D:response><D:href>";
	body += href;
	body += "</D:href><D:propstat><D:prop>";
	body += "<D:displayname>" + webdavBaseName(logicalPath) + "</D:displayname>";
	body += "<D:getlastmodified>" + webdavHttpDate(mtime) + "</D:getlastmodified>";
	if (isDir) {
		body += "<D:resourcetype><D:collection/></D:resourcetype>";
	} else {
		body += "<D:resourcetype/>";
		body += "<D:getcontentlength>" + String(size) + "</D:getcontentlength>";
		body += "<D:getcontenttype>" + String(webdavContentType(logicalPath)) + "</D:getcontenttype>";
	}
	body += "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>";
}

static void webdavHandlePropfind(WiFiClient &client, const String &path, const String &depth) {
	fs::File node = gFSystem.open(path);
	if (!node) {
		webdavSendStatus(client, 404, "Not Found");
		return;
	}
	bool isDir = node.isDirectory();
	uint32_t size = isDir ? 0 : node.size();
	time_t mtime = node.getLastWrite();
	node.close();

	static const char *prolog = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<D:multistatus xmlns:D=\"DAV:\">";
	static const char *epilog = "</D:multistatus>";

	String self;
	webdavAppendResponse(self, (path == "/") ? "" : path, isDir, size, mtime);
	const bool listChildren = isDir && depth != "0";

	// macOS' WebDAV client (and others) need a Content-Length -- it rejects a chunked PROPFIND, so the
	// drive shows nothing in Finder. We therefore send Content-Length, but to stay O(n) and keep memory
	// flat for big music folders we do two passes: pass 1 sums the body length, pass 2 streams it one
	// entry at a time. Both passes enumerate with openNextFile() (sequential -- the old open()-per-child
	// re-scanned the directory for every entry => O(n^2), which is what made large folders "load forever").
	size_t total = strlen(prolog) + self.length() + strlen(epilog);
	if (listChildren) {
		fs::File dir = gFSystem.open(path);
		fs::File child;
		while ((child = dir.openNextFile())) {
			String cp = gFSystem.path(child);
			bool cDir = child.isDirectory();
			uint32_t cs = cDir ? 0 : child.size();
			time_t cm = child.getLastWrite();
			child.close();
			String entry;
			webdavAppendResponse(entry, cp, cDir, cs, cm);
			total += entry.length();
		}
		dir.close();
	}

	client.print("HTTP/1.1 207 Multi-Status\r\n");
	client.print("Connection: close\r\n");
	client.print("DAV: 1, 2\r\n");
	client.print("Content-Type: application/xml; charset=utf-8\r\n");
	client.printf("Content-Length: %u\r\n\r\n", (unsigned) total);

	client.print(prolog);
	client.print(self);
	if (listChildren) {
		fs::File dir = gFSystem.open(path);
		fs::File child;
		while ((child = dir.openNextFile())) {
			String cp = gFSystem.path(child);
			bool cDir = child.isDirectory();
			uint32_t cs = cDir ? 0 : child.size();
			time_t cm = child.getLastWrite();
			child.close();
			String entry;
			webdavAppendResponse(entry, cp, cDir, cs, cm);
			client.print(entry);
			if (!client.connected()) {
				break;
			}
		}
		dir.close();
	}
	client.print(epilog);
}

// ---------------------------------------------------------------------------- GET / HEAD

static void webdavHandleGet(WiFiClient &client, const String &path, bool headOnly, const String &range) {
	fs::File f = gFSystem.open(path);
	if (!f) {
		webdavSendStatus(client, 404, "Not Found");
		return;
	}
	if (f.isDirectory()) {
		f.close();
		webdavSendStatus(client, 403, "Forbidden");
		return;
	}
	uint32_t total = f.size();
	uint32_t start = 0;
	uint32_t end = (total > 0) ? total - 1 : 0;
	bool partial = false;
	// Minimal single-range support ("bytes=start-" / "bytes=start-end"); some clients need it for media.
	if (range.startsWith("bytes=") && total > 0) {
		String r = range.substring(6);
		int dash = r.indexOf('-');
		if (dash >= 0) {
			start = (uint32_t) r.substring(0, dash).toInt();
			String es = r.substring(dash + 1);
			if (es.length() > 0) {
				end = (uint32_t) es.toInt();
			}
			if (start <= end && end < total) {
				partial = true;
			} else {
				start = 0;
				end = total - 1;
			}
		}
	}
	uint32_t length = (total == 0) ? 0 : (end - start + 1);

	if (partial) {
		client.print("HTTP/1.1 206 Partial Content\r\n");
		client.printf("Content-Range: bytes %u-%u/%u\r\n", (unsigned) start, (unsigned) end, (unsigned) total);
	} else {
		client.print("HTTP/1.1 200 OK\r\n");
	}
	client.print("Connection: close\r\n");
	client.print("Accept-Ranges: bytes\r\n");
	client.printf("Content-Type: %s\r\n", webdavContentType(path));
	client.printf("Content-Length: %u\r\n\r\n", (unsigned) length);

	if (!headOnly && length > 0) {
		f.seek(start);
		uint8_t *buf = (uint8_t *) malloc(WEBDAV_BUFFER_SIZE);
		if (buf) {
			uint32_t remaining = length;
			while (remaining > 0 && client.connected()) {
				size_t want = (remaining > WEBDAV_BUFFER_SIZE) ? WEBDAV_BUFFER_SIZE : remaining;
				int got = f.read(buf, want);
				if (got <= 0) {
					break;
				}
				client.write(buf, got);
				remaining -= got;
				System_UpdateActivityTimer(); // an active download keeps the device awake (idle polling does not)
			}
			free(buf);
		}
	}
	f.close();
}

// ---------------------------------------------------------------------------- PUT

static void webdavHandlePut(WiFiClient &client, const String &path, long contentLength) {
	bool existed = gFSystem.exists(path);
	fs::File f = gFSystem.open(path, "w", true);
	if (!f) {
		webdavDrain(client, contentLength);
		webdavSendStatus(client, 409, "Conflict"); // parent collection missing or open failed
		return;
	}
	uint8_t *buf = (uint8_t *) malloc(WEBDAV_BUFFER_SIZE);
	bool ok = (buf != nullptr);
	long remaining = contentLength;
	uint32_t idleStart = millis();
	while (ok && remaining > 0 && client.connected()) {
		int avail = client.available();
		if (avail <= 0) {
			if (millis() - idleStart > 8000) {
				ok = false; // stalled upload
				break;
			}
			vTaskDelay(pdMS_TO_TICKS(2));
			continue;
		}
		idleStart = millis();
		size_t want = (size_t) avail;
		if (want > WEBDAV_BUFFER_SIZE) {
			want = WEBDAV_BUFFER_SIZE;
		}
		if ((long) want > remaining) {
			want = (size_t) remaining;
		}
		int got = client.read(buf, want);
		if (got <= 0) {
			continue;
		}
		if (f.write(buf, got) != (size_t) got) {
			ok = false;
			break;
		}
		remaining -= got;
		System_UpdateActivityTimer(); // an active upload keeps the device awake (idle polling does not)
	}
	if (buf) {
		free(buf);
	}
	f.close();
	if (!ok || remaining > 0) {
		gFSystem.remove(path); // drop the partial file
		webdavSendStatus(client, 500, "Internal Server Error");
		return;
	}
	webdavSendStatus(client, existed ? 204 : 201, existed ? "No Content" : "Created");
}

// ---------------------------------------------------------------------------- DELETE / MKCOL

static bool webdavDeleteRecursive(const String &path) {
	fs::File f = gFSystem.open(path);
	if (!f) {
		return false;
	}
	if (!f.isDirectory()) {
		f.close();
		return gFSystem.remove(path);
	}
	f.close();
	fs::File dir = gFSystem.open(path);
	bool childIsDir = false;
	String child;
	while ((child = gFSystem.nextFileName(dir, &childIsDir)).length() > 0) {
		if (childIsDir) {
			webdavDeleteRecursive(child);
		} else {
			gFSystem.remove(child);
		}
	}
	dir.close();
	return gFSystem.rmdir(path);
}

static void webdavHandleDelete(WiFiClient &client, const String &path) {
	if (!gFSystem.exists(path)) {
		webdavSendStatus(client, 404, "Not Found");
		return;
	}
	if (webdavDeleteRecursive(path)) {
		webdavSendStatus(client, 204, "No Content");
	} else {
		webdavSendStatus(client, 500, "Internal Server Error");
	}
}

static void webdavHandleMkcol(WiFiClient &client, const String &path) {
	if (gFSystem.exists(path)) {
		webdavSendStatus(client, 405, "Method Not Allowed");
		return;
	}
	if (gFSystem.mkdir(path)) {
		webdavSendStatus(client, 201, "Created");
	} else {
		webdavSendStatus(client, 409, "Conflict");
	}
}

// ---------------------------------------------------------------------------- MOVE / COPY

static bool webdavCopyRecursive(const String &src, const String &dst) {
	fs::File sf = gFSystem.open(src);
	if (!sf) {
		return false;
	}
	if (sf.isDirectory()) {
		sf.close();
		if (!gFSystem.exists(dst) && !gFSystem.mkdir(dst)) {
			return false;
		}
		fs::File dir = gFSystem.open(src);
		bool childIsDir = false;
		String child;
		bool ok = true;
		while ((child = gFSystem.nextFileName(dir, &childIsDir)).length() > 0) {
			ok = webdavCopyRecursive(child, dst + "/" + webdavBaseName(child)) && ok;
		}
		dir.close();
		return ok;
	}
	// regular file: stream src -> dst
	fs::File df = gFSystem.open(dst, "w", true);
	if (!df) {
		sf.close();
		return false;
	}
	uint8_t *buf = (uint8_t *) malloc(WEBDAV_BUFFER_SIZE);
	bool ok = (buf != nullptr);
	while (ok) {
		int got = sf.read(buf, WEBDAV_BUFFER_SIZE);
		if (got <= 0) {
			break;
		}
		if (df.write(buf, got) != (size_t) got) {
			ok = false;
		}
	}
	if (buf) {
		free(buf);
	}
	sf.close();
	df.close();
	return ok;
}

static void webdavHandleMoveCopy(WiFiClient &client, const String &path, const String &destHeader, bool isMove, bool overwrite) {
	if (destHeader.isEmpty()) {
		webdavSendStatus(client, 400, "Bad Request");
		return;
	}
	String dest = webdavUriToPath(destHeader);
	if (!gFSystem.exists(path)) {
		webdavSendStatus(client, 404, "Not Found");
		return;
	}
	bool destExisted = gFSystem.exists(dest);
	if (destExisted) {
		if (!overwrite) {
			webdavSendStatus(client, 412, "Precondition Failed");
			return;
		}
		webdavDeleteRecursive(dest);
	}
	bool ok;
	if (isMove) {
		ok = gFSystem.rename(path, dest);
	} else {
		ok = webdavCopyRecursive(path, dest);
	}
	if (ok) {
		webdavSendStatus(client, destExisted ? 204 : 201, destExisted ? "No Content" : "Created");
	} else {
		webdavSendStatus(client, 500, "Internal Server Error");
	}
}

// ---------------------------------------------------------------------------- LOCK (stub)

// macOS Finder / Windows / Office insist on locking before writing. We don't track real locks
// (single user, no concurrency) but must hand back a well-formed lock token so writes proceed.
static void webdavHandleLock(WiFiClient &client, const String &path) {
	String token = "opaquelocktoken:espuino-" + String((uint32_t) millis(), HEX);
	String body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
	body += "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>";
	body += "<D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope>";
	body += "<D:depth>infinity</D:depth><D:timeout>Second-3600</D:timeout>";
	body += "<D:locktoken><D:href>" + token + "</D:href></D:locktoken>";
	body += "</D:activelock></D:lockdiscovery></D:prop>";
	client.print("HTTP/1.1 200 OK\r\n");
	client.print("Connection: close\r\n");
	client.print("Lock-Token: <" + token + ">\r\n");
	client.print("Content-Type: application/xml; charset=utf-8\r\n");
	client.printf("Content-Length: %u\r\n\r\n", (unsigned) body.length());
	client.print(body);
}

// ---------------------------------------------------------------------------- request dispatch

static void webdavHandleClient(WiFiClient &client) {
	client.setNoDelay(true);
	client.setTimeout(1500);
	// macOS' webdavfs keeps idle keep-alive connections parked in a pool. Since we answer with
	// "Connection: close", accept() hands us those parked sockets with no request on them; blocking
	// in readStringUntil for the full timeout would stall this single-threaded server and make Finder
	// hang ("stuck loading"). A real request's bytes arrive within the first round-trip, so wait only
	// briefly for the first byte and drop the connection fast if nothing comes.
	uint32_t idleStart = millis();
	while (client.connected() && client.available() == 0 && (millis() - idleStart) < 150) {
		vTaskDelay(pdMS_TO_TICKS(3));
	}
	if (client.available() == 0) {
		return; // parked/empty connection -- caller closes it, freeing us to accept the next at once
	}
	String reqLine = client.readStringUntil('\n');
	reqLine.trim();
	if (reqLine.isEmpty()) {
		return;
	}
	int sp1 = reqLine.indexOf(' ');
	int sp2 = reqLine.indexOf(' ', sp1 + 1);
	if (sp1 < 0 || sp2 < 0) {
		webdavSendStatus(client, 400, "Bad Request");
		return;
	}
	String method = reqLine.substring(0, sp1);
	String rawUri = reqLine.substring(sp1 + 1, sp2);

	long contentLength = 0;
	String depth = "infinity";
	String destination, authz, overwrite = "T", range;
	while (client.connected()) {
		String line = client.readStringUntil('\n');
		if (line == "\r" || line.length() == 0 || line == "\n") {
			break;
		}
		line.trim();
		if (line.isEmpty()) {
			break;
		}
		int colon = line.indexOf(':');
		if (colon < 0) {
			continue;
		}
		String key = line.substring(0, colon);
		key.toLowerCase();
		key.trim();
		String val = line.substring(colon + 1);
		val.trim();
		if (key == "content-length") {
			contentLength = val.toInt();
		} else if (key == "depth") {
			depth = val;
		} else if (key == "destination") {
			destination = val;
		} else if (key == "authorization") {
			authz = val;
		} else if (key == "overwrite") {
			overwrite = val;
		} else if (key == "range") {
			range = val;
		}
	}

	// Authentication (HTTP Basic). Any username is accepted; only the password is checked.
	// When no password is configured the drive is open.
	if (!webdavCheckAuth(authz)) {
		webdavDrain(client, contentLength);
		client.print("HTTP/1.1 401 Unauthorized\r\n");
		client.print("WWW-Authenticate: Basic realm=\"ESPuino WebDAV\"\r\n");
		client.print("Connection: close\r\n");
		client.print("Content-Length: 0\r\n\r\n");
		return;
	}

	String path = webdavUriToPath(rawUri);
	bool overwriteFlag = !overwrite.equalsIgnoreCase("F");

	if (method == "OPTIONS") {
		webdavDrain(client, contentLength);
		client.print("HTTP/1.1 200 OK\r\n");
		client.print("Connection: close\r\n");
		client.print("DAV: 1, 2\r\n");
		client.print("MS-Author-Via: DAV\r\n");
		client.print("Allow: OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK\r\n");
		client.print("Content-Length: 0\r\n\r\n");
	} else if (method == "PROPFIND") {
		webdavDrain(client, contentLength);
		webdavHandlePropfind(client, path, depth);
	} else if (method == "GET" || method == "HEAD") {
		webdavDrain(client, contentLength);
		webdavHandleGet(client, path, method == "HEAD", range);
	} else if (method == "PUT") {
		webdavHandlePut(client, path, contentLength);
	} else if (method == "DELETE") {
		webdavDrain(client, contentLength);
		webdavHandleDelete(client, path);
	} else if (method == "MKCOL") {
		webdavDrain(client, contentLength);
		webdavHandleMkcol(client, path);
	} else if (method == "MOVE" || method == "COPY") {
		webdavDrain(client, contentLength);
		webdavHandleMoveCopy(client, path, destination, method == "MOVE", overwriteFlag);
	} else if (method == "LOCK") {
		webdavDrain(client, contentLength);
		webdavHandleLock(client, path);
	} else if (method == "UNLOCK") {
		webdavDrain(client, contentLength);
		webdavSendStatus(client, 204, "No Content");
	} else if (method == "PROPPATCH") {
		webdavDrain(client, contentLength);
		// We don't persist arbitrary props (e.g. Win32 timestamps); acknowledge so writes complete.
		String href = webdavEncodeHref((path == "/") ? "" : path);
		if (href.isEmpty()) {
			href = "/";
		}
		String body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>";
		body += href + "</D:href><D:propstat><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>";
		client.print("HTTP/1.1 207 Multi-Status\r\n");
		client.print("Connection: close\r\n");
		client.print("Content-Type: application/xml; charset=utf-8\r\n");
		client.printf("Content-Length: %u\r\n\r\n", (unsigned) body.length());
		client.print(body);
	} else {
		webdavDrain(client, contentLength);
		webdavSendStatus(client, 405, "Method Not Allowed");
	}
}

// ---------------------------------------------------------------------------- task + lifecycle

static void webdavTask(void *param) {
	webdavServer = new WiFiServer(webdavPort);
	webdavServer->begin();
	webdavRunning = true;
	Log_Printf(LOGLEVEL_NOTICE, "WebDAV server started on port %u", webdavPort);
	while (webdavShouldRun) {
		if (!Wlan_IsConnected()) {
			vTaskDelay(pdMS_TO_TICKS(250));
			continue;
		}
		// Use accept() (not the deprecated available()): it hands back each *new* connection exactly
		// once and a falsy client when none is pending. available() can keep returning the same/stale
		// client, which spins this loop at 100% CPU and wedges the server once Finder opens its burst
		// of short-lived connections -- the drive then "disappears" mid-mount.
		WiFiClient client = webdavServer->accept();
		if (client) {
			// NB: we deliberately do NOT refresh the inactivity timer here. A mounted drive makes
			// Finder/Explorer poll (PROPFIND) constantly; refreshing on every request would keep the
			// device awake forever. Instead only an active file transfer (GET/PUT body) keeps it awake,
			// so an idle-but-mounted drive still lets the device fall asleep normally.
			webdavHandleClient(client);
			client.stop();
			vTaskDelay(pdMS_TO_TICKS(2)); // always yield, even under a burst of connections
		} else {
			vTaskDelay(pdMS_TO_TICKS(20));
		}
	}
	webdavServer->stop();
	delete webdavServer;
	webdavServer = nullptr;
	webdavRunning = false;
	webdavTaskHandle = nullptr;
	Log_Println("WebDAV server stopped", LOGLEVEL_NOTICE);
	vTaskDelete(nullptr);
}

void Webdav_Init(void) {
	String nvsUser = gPrefsSettings.getString("webdavUser", "-1");
	if (nvsUser == "-1") {
		gPrefsSettings.putString("webdavUser", Webdav_User);
	} else {
		Webdav_User = nvsUser;
	}
	String nvsPwd = gPrefsSettings.getString("webdavPwd", "-1");
	if (nvsPwd == "-1") {
		gPrefsSettings.putString("webdavPwd", Webdav_Password);
	} else {
		Webdav_Password = nvsPwd;
	}
	webdavAutostart = gPrefsSettings.getBool("webdavEnable", false);
	webdavComputeAuth();
}

void Webdav_ReloadCredentials(void) {
	Webdav_User = gPrefsSettings.getString("webdavUser", Webdav_User);
	Webdav_Password = gPrefsSettings.getString("webdavPwd", Webdav_Password);
	webdavComputeAuth(); // running task re-checks auth per request, so no restart needed
}

void Webdav_Cyclic(void) {
	// One-shot auto-start: when the persisted setting asks for it, fire the server up the first time
	// WiFi is available after boot, then never touch it again (so a manual stop isn't overridden).
	// webdavAutostart is cached in Webdav_Init, so this stays a cheap branch every loop() (no NVS read).
	static bool autostartHandled = false;
	if (webdavAutostart && !autostartHandled && Wlan_IsConnected()) {
		autostartHandled = true;
		Webdav_EnableServer();
	}
}

void Webdav_EnableServer(void) {
	if (webdavTaskHandle != nullptr || webdavRunning) {
		return; // already running
	}
	if (!Wlan_IsConnected()) {
		Log_Println("WebDAV: cannot start, no WiFi", LOGLEVEL_ERROR);
		System_IndicateError();
		return;
	}
	webdavShouldRun = true;
	if (xTaskCreatePinnedToCore(webdavTask, "webdav", 8192, nullptr, 1, &webdavTaskHandle, 0) != pdPASS) {
		webdavShouldRun = false;
		webdavTaskHandle = nullptr;
		Log_Println("WebDAV: failed to create task", LOGLEVEL_ERROR);
		System_IndicateError();
		return;
	}
	System_IndicateOk();
}

void Webdav_DisableServer(void) {
	if (!webdavShouldRun && !webdavRunning) {
		return;
	}
	webdavShouldRun = false; // the task closes the listener and self-deletes on its next iteration
	System_IndicateOk();
}

void Webdav_Exit(void) {
	webdavShouldRun = false;
	uint32_t start = millis();
	while (webdavRunning && (millis() - start < 1500)) {
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

bool Webdav_IsServerRunning(void) {
	return webdavRunning || webdavShouldRun;
}

#else // WEBDAV_ENABLE

void Webdav_Init(void) {
}
void Webdav_Cyclic(void) {
}
void Webdav_Exit(void) {
}
void Webdav_ReloadCredentials(void) {
}
void Webdav_EnableServer(void) {
}
void Webdav_DisableServer(void) {
}
bool Webdav_IsServerRunning(void) {
	return false;
}

#endif // WEBDAV_ENABLE
