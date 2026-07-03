#include <Arduino.h>
#include "settings.h"

#include "Webdav.h"

#include "Common.h"
#include "Log.h"
#include "SdCard.h"
#include "System.h"
#include "Web.h"
#include "Wlan.h"

#include <WiFi.h>
#include <mbedtls/base64.h>

// A compact, self-contained WebDAV/1,2 server over gFSystem (the sanitized SD filesystem). It
// supports the methods Finder/Explorer need to mount the card read/write: OPTIONS, PROPFIND,
// GET/HEAD (incl. byte ranges), PUT, DELETE, MKCOL, MOVE, COPY and stub LOCK/UNLOCK/PROPPATCH.
// Everything runs inside one FreeRTOS task on core 0; requests are handled synchronously there so
// large copies block only this task, never the audio pipeline on core 1.

#ifdef WEBDAV_ENABLE

// macOS Finder opens one TCP connection per file in a folder (no keep-alive: "Connection: close")
// to fetch cover-art/thumbnail data, all at once when entering a folder in icon view. Handling
// them one at a time made a real folder's browse time scale linearly with file count (measured
// ~0.6s/file -> 5.5s wall for 9 files, worse for a typical 14+-track audiobook folder) - easily
// past what Finder waits before it looks permanently stuck. NetworkServer::accept() is safe to
// call from multiple tasks on the same listening socket (the only shared, mutable member it
// touches - _accepted_sockfd - is only ever written by the available()/hasClient() path, which
// this server never calls), so a small pool of worker tasks pulls connections off the same
// listener concurrently. Each request is still handled synchronously end-to-end on its own
// worker, so a single slow transfer only blocks the OTHER workers, never all of them at once.
static constexpr int WEBDAV_WORKER_COUNT = 2;

static WiFiServer *webdavServer = nullptr;
static TaskHandle_t webdavTaskHandles[WEBDAV_WORKER_COUNT] = {nullptr};
static volatile bool webdavShouldRun = false;
static volatile bool webdavRunning = false; // true once the first worker has created+started the listener
static volatile int webdavActiveWorkers = 0; // workers currently inside their accept loop
// Enable/Disable/Exit are called from several tasks (web, MQTT, command, system-shutdown), and now
// also guard the multi-worker startup/teardown handoff (first worker in creates the listener, last
// worker out destroys it). Without serialization, a start racing the previous run's self-teardown
// (delete webdavServer) could `new` a second server against the concurrent delete -> use-after-free
// / double-free reboot. This mutex makes those decisions atomic; steady-state request handling
// itself stays lock-free.
static portMUX_TYPE webdavStateMux = portMUX_INITIALIZER_UNLOCKED;

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
		// A literal '+' in a URL *path* is a plain plus, not a space (the '+'->space rule is query-string
		// only, RFC 3986); only %XX sequences are decoded, everything else is appended verbatim.
		out += c;
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

// Discard a chunked request body (Transfer-Encoding: chunked, no Content-Length). macOS Finder
// sends chunked bodies on PROPFIND/LOCK/PROPPATCH; without draining them, client.stop() closes
// the socket with the body still unread, which RSTs the connection and can truncate our reply -
// Finder then never sees a valid response and retries in a tight loop. We don't need the decoded
// content, so just read until the terminating 0-length chunk or the peer stops sending.
static void webdavDrainChunked(WiFiClient &client) {
	uint8_t tmp[256];
	uint32_t idle = millis();
	while (client.connected() && (millis() - idle) < 2000) {
		int got = client.read(tmp, sizeof(tmp));
		if (got > 0) {
			idle = millis();
			// the last chunk is "0\r\n\r\n"; once we see a standalone 0-size chunk trailer we're done
			if (got >= 5 && tmp[0] == '0' && (tmp[1] == '\r' || tmp[1] == '\n')) {
				break;
			}
			continue;
		}
		if (!client.available()) {
			vTaskDelay(pdMS_TO_TICKS(5));
		}
	}
}

// ---------------------------------------------------------------------------- PROPFIND

// Escape the five XML-significant characters we can actually emit inside element text (a filename can
// contain any of &, <, >). Ampersand must be replaced first so we don't double-escape the entities we
// just introduced. Without this a file like "Simon & Garfunkel" makes the whole 207 body malformed.
static String webdavXmlEscape(const String &s) {
	String out = s;
	out.replace("&", "&amp;");
	out.replace("<", "&lt;");
	out.replace(">", "&gt;");
	return out;
}

// Append one <D:response> element describing <logicalPath> (a file or directory) to <body>.
static void webdavAppendResponse(String &body, const String &logicalPath, bool isDir, uint32_t size, time_t mtime) {
	String href = Url_EncodePath(logicalPath);
	if (href.isEmpty()) {
		href = "/";
	}
	if (isDir && !href.endsWith("/")) {
		href += "/";
	}
	body += "<D:response><D:href>";
	body += href;
	body += "</D:href><D:propstat><D:prop>";
	body += "<D:displayname>" + webdavXmlEscape(webdavBaseName(logicalPath)) + "</D:displayname>";
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
		if (dash == 0) {
			// Suffix form "bytes=-N": the last N bytes. Misparsing this as the first N bytes would
			// serve a *different* range than requested under a 206, which corrupts media playback.
			uint32_t suffix = (uint32_t) r.substring(1).toInt();
			if (suffix > 0) {
				start = (suffix >= total) ? 0 : (total - suffix);
				end = total - 1;
				partial = true;
			}
		} else if (dash > 0) {
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
			while (remaining > 0 && client.connected() && webdavShouldRun) { // abort a big GET promptly on shutdown
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
	while (ok && remaining > 0 && client.connected() && webdavShouldRun) { // abort a big PUT promptly on shutdown
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

static bool webdavDeleteRecursive(const String &path, uint8_t depth = 0) {
	// Bound the recursion: the WebDAV task has an 8 kB stack and each frame holds a File + a String,
	// so a pathologically deep tree could otherwise overflow it (mirrors syncMirrorDir in Sync.cpp).
	if (depth >= 20) {
		return false;
	}
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
			webdavDeleteRecursive(child, depth + 1);
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

static bool webdavCopyRecursive(const String &src, const String &dst, uint8_t depth = 0) {
	// Bound the recursion: the WebDAV task has an 8 kB stack and each frame holds two Files + Strings,
	// so a pathologically deep tree could otherwise overflow it (mirrors syncMirrorDir in Sync.cpp).
	if (depth >= 20) {
		return false;
	}
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
			ok = webdavCopyRecursive(child, dst + "/" + webdavBaseName(child), depth + 1) && ok;
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
	// Reject self- and inside-source destinations: a MOVE onto its own path would delete the source
	// (we remove an overwrite target first), and a COPY of /a into /a/b would recurse into its own
	// output until the 8 kB task stack overflows. FAT is case-insensitive, so compare that way.
	String pathSlash = path + "/";
	String destSlash = dest + "/";
	bool destInsideSrc = destSlash.length() >= pathSlash.length() && destSlash.substring(0, pathSlash.length()).equalsIgnoreCase(pathSlash);
	bool srcInsideDest = pathSlash.length() >= destSlash.length() && pathSlash.substring(0, destSlash.length()).equalsIgnoreCase(destSlash);
	if (dest.equalsIgnoreCase(path) || destInsideSrc || srcInsideDest) {
		webdavSendStatus(client, 403, "Forbidden");
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

// The whole request line + header block of a real WebDAV request arrives within one LAN round-trip
// (single-digit ms). We read the header block under this overall deadline so a connection that
// stalls mid-request (macOS opens a pool of connections and doesn't always finish a request on
// each) is abandoned in well under a second instead of blocking this single-threaded server for
// the full socket timeout. That per-connection stall was what made Finder browsing sit on
// "Loading..." for many seconds: every half-open pooled connection froze the server in turn.
static constexpr uint32_t WEBDAV_HEADER_DEADLINE_MS = 500;

// Read one line (up to and including a '\n', which is discarded) into out, bounded by an absolute
// millis() deadline. Returns false when the deadline passes before a full line arrives, or the
// connection closes, or the line grows past 2 KB (OOM guard) - the caller then drops the socket.
static bool webdavReadLine(WiFiClient &client, String &out, uint32_t deadlineMs) {
	out = "";
	while (true) {
		int c = client.read();
		if (c < 0) {
			if ((int32_t) (millis() - deadlineMs) >= 0) {
				return false; // request stalled - don't let it hog the single server task
			}
			if (!client.connected() && client.available() == 0) {
				return false;
			}
			vTaskDelay(pdMS_TO_TICKS(2));
			continue;
		}
		if (c == '\n') {
			return true;
		}
		if (out.length() >= 2048) {
			return false; // absurdly long line (no newline) - drop before it OOMs the heap
		}
		out += (char) c;
	}
}

static void webdavHandleClient(WiFiClient &client) {
	client.setNoDelay(true);
	client.setTimeout(1500);
	// macOS' webdavfs keeps a pool of keep-alive connections parked. Since we answer with
	// "Connection: close", accept() hands us those parked sockets with no request on them. Drop
	// them fast (a real request's first byte is here within one LAN round-trip) so the pool doesn't
	// serialize into a multi-second stall on this single-threaded server.
	uint32_t firstByte = millis();
	while (client.connected() && client.available() == 0 && (millis() - firstByte) < 120) {
		vTaskDelay(pdMS_TO_TICKS(2));
	}
	if (client.available() == 0) {
		return; // parked/empty connection
	}
	// Once bytes are flowing, the whole header block of a real request follows within a few ms.
	// Bound the rest so a connection that stalls mid-request is abandoned in well under a second.
	const uint32_t headerDeadline = millis() + WEBDAV_HEADER_DEADLINE_MS;

	String reqLine;
	if (!webdavReadLine(client, reqLine, headerDeadline)) {
		return; // stalled after the first byte -- drop it
	}
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
	String destination, authz, overwrite = "T", range, transferEncoding;
	size_t headerBytes = 0;
	while (true) {
		String line;
		if (!webdavReadLine(client, line, headerDeadline)) {
			return; // stalled mid-headers -- drop fast rather than block the server
		}
		if (line == "\r" || line.length() == 0) {
			break;
		}
		// Bound total header size: a flood of headers would otherwise exhaust the internal heap.
		// 8 KB is far more than any real WebDAV request needs.
		if ((headerBytes += line.length()) > 8192) {
			webdavSendStatus(client, 431, "Request Header Fields Too Large");
			return;
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
		} else if (key == "transfer-encoding") {
			transferEncoding = val;
		}
	}

	// Authentication (HTTP Basic). Any username is accepted; only the password is checked.
	// When no password is configured the drive is open.
	if (!webdavCheckAuth(authz)) {
		if (transferEncoding.indexOf("hunked") >= 0) {
			webdavDrainChunked(client);
		} else {
			webdavDrain(client, contentLength);
		}
		client.print("HTTP/1.1 401 Unauthorized\r\n");
		client.print("WWW-Authenticate: Basic realm=\"ESPuino WebDAV\"\r\n");
		client.print("Connection: close\r\n");
		client.print("Content-Length: 0\r\n\r\n");
		return;
	}

	String path = webdavUriToPath(rawUri);
	bool overwriteFlag = !overwrite.equalsIgnoreCase("F");

	// Whether the request carries a chunked body (no Content-Length). We don't decode chunked
	// content, but every non-PUT method below still has to *consume* it before closing the socket,
	// or the RST-on-close truncates our reply and Finder retries in a loop.
	bool bodyIsChunked = false;
	{
		String te = transferEncoding;
		te.toLowerCase();
		bodyIsChunked = te.indexOf("chunked") >= 0;
	}
	// Consume the request body (fixed length or chunked) so client.stop() never RSTs on unread data.
	auto consumeBody = [&]() {
		if (bodyIsChunked) {
			webdavDrainChunked(client);
		} else {
			webdavDrain(client, contentLength);
		}
	};

	if (method == "OPTIONS") {
		consumeBody();
		client.print("HTTP/1.1 200 OK\r\n");
		client.print("Connection: close\r\n");
		client.print("DAV: 1, 2\r\n");
		client.print("MS-Author-Via: DAV\r\n");
		client.print("Allow: OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK\r\n");
		client.print("Content-Length: 0\r\n\r\n");
	} else if (method == "PROPFIND") {
		consumeBody();
		webdavHandlePropfind(client, path, depth);
	} else if (method == "GET" || method == "HEAD") {
		consumeBody();
		webdavHandleGet(client, path, method == "HEAD", range);
	} else if (method == "PUT") {
		// A chunked body has no Content-Length, so we'd otherwise treat it as a 0-byte PUT and truncate
		// the target to empty. We don't decode chunked transfer-encoding, so per RFC 7230 demand a length
		// (draining the body first so the 411 reaches Finder cleanly instead of being RST-truncated).
		if (bodyIsChunked) {
			webdavDrainChunked(client);
			webdavSendStatus(client, 411, "Length Required");
		} else if (contentLength < 0) {
			// a negative length would skip the copy loop and leave the freshly-truncated target empty
			webdavSendStatus(client, 400, "Bad Request");
		} else {
			webdavHandlePut(client, path, contentLength);
		}
	} else if (method == "DELETE") {
		consumeBody();
		webdavHandleDelete(client, path);
	} else if (method == "MKCOL") {
		consumeBody();
		webdavHandleMkcol(client, path);
	} else if (method == "MOVE" || method == "COPY") {
		consumeBody();
		webdavHandleMoveCopy(client, path, destination, method == "MOVE", overwriteFlag);
	} else if (method == "LOCK") {
		consumeBody();
		webdavHandleLock(client, path);
	} else if (method == "UNLOCK") {
		consumeBody();
		webdavSendStatus(client, 204, "No Content");
	} else if (method == "PROPPATCH") {
		consumeBody();
		// We don't persist arbitrary props (e.g. Win32 timestamps); acknowledge so writes complete.
		String href = Url_EncodePath((path == "/") ? "" : path);
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
		consumeBody();
		webdavSendStatus(client, 405, "Method Not Allowed");
	}
}

// ---------------------------------------------------------------------------- task + lifecycle

static void webdavTask(void *param) {
	const int workerIndex = (int) (intptr_t) param;

	// First worker in creates the shared listener; later workers just wait for it. Backlog raised
	// from the default of 4 to match CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE (bumped alongside this in
	// sdkconfig.defaults) so a realistic folder's simultaneous connections are all queued
	// immediately instead of a fraction of them being SYN-retried.
	bool isFirst = false;
	portENTER_CRITICAL(&webdavStateMux);
	webdavActiveWorkers++;
	isFirst = (webdavActiveWorkers == 1);
	portEXIT_CRITICAL(&webdavStateMux);
	if (isFirst) {
		webdavServer = new WiFiServer(webdavPort, 12);
		webdavServer->begin();
		webdavRunning = true;
		Log_Printf(LOGLEVEL_NOTICE, "WebDAV server started on port %u (%d workers)", webdavPort, WEBDAV_WORKER_COUNT);
	} else {
		while (!webdavRunning && webdavShouldRun) {
			vTaskDelay(pdMS_TO_TICKS(5));
		}
	}

	while (webdavShouldRun) {
		if (!Wlan_IsConnected()) {
			vTaskDelay(pdMS_TO_TICKS(250));
			continue;
		}
		// Use accept() (not the deprecated available()): it hands back each *new* connection exactly
		// once and a falsy client when none is pending. available() can keep returning the same/stale
		// client, which spins this loop at 100% CPU and wedges the server once Finder opens its burst
		// of short-lived connections -- the drive then "disappears" mid-mount. Safe to call from
		// several worker tasks concurrently (see the WEBDAV_WORKER_COUNT comment above).
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

	bool isLast = false;
	portENTER_CRITICAL(&webdavStateMux);
	webdavActiveWorkers--;
	isLast = (webdavActiveWorkers == 0);
	portEXIT_CRITICAL(&webdavStateMux);
	if (isLast) {
		if (webdavServer != nullptr) { // defensive: null only if this worker raced past a not-yet-scheduled "first" worker
			webdavServer->stop();
			delete webdavServer;
			webdavServer = nullptr;
		}
		webdavRunning = false;
		Log_Println("WebDAV server stopped", LOGLEVEL_NOTICE);
	}
	webdavTaskHandles[workerIndex] = nullptr;
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

	// Keep every open web UI in sync no matter WHO toggled the server (websocket button, command
	// 188 via button/IR remote, MQTT, autostart): broadcast on every state edge. The websocket
	// handler's own broadcast right after Enable/Disable covers the common case; this catches the
	// rest without the other call sites having to know about the web layer.
	static bool lastReportedState = false;
	const bool state = Webdav_IsServerRunning();
	if (state != lastReportedState) {
		lastReportedState = state;
		Web_SendWebsocketData(0, WebsocketCodeType::WebdavStatus);
	}
}

void Webdav_EnableServer(void) {
	if (!Wlan_IsConnected()) {
		Log_Println("WebDAV: cannot start, no WiFi", LOGLEVEL_ERROR);
		System_IndicateError();
		return;
	}
	// Claim the start atomically: only proceed if the server is fully stopped (no worker task, not
	// running, and not asked to run). Anything else means a start is already live or the previous
	// instance hasn't finished tearing down - either way, don't spawn a second batch of workers.
	bool claimed = false;
	portENTER_CRITICAL(&webdavStateMux);
	bool anyTaskAlive = false;
	for (int i = 0; i < WEBDAV_WORKER_COUNT; i++) {
		anyTaskAlive = anyTaskAlive || (webdavTaskHandles[i] != nullptr);
	}
	if (!anyTaskAlive && !webdavRunning && !webdavShouldRun) {
		webdavShouldRun = true;
		claimed = true;
	}
	portEXIT_CRITICAL(&webdavStateMux);
	if (!claimed) {
		return; // already running or mid-transition
	}
	for (int i = 0; i < WEBDAV_WORKER_COUNT; i++) {
		if (xTaskCreatePinnedToCore(webdavTask, "webdav", 8192, (void *) (intptr_t) i, 1, &webdavTaskHandles[i], 0) != pdPASS) {
			Log_Println("WebDAV: failed to create worker task", LOGLEVEL_ERROR);
			// Stop whichever workers already started and wait for them to unwind before giving up,
			// so we don't leave a partially-started server (and its listener) behind.
			webdavShouldRun = false;
			uint32_t start = millis();
			while (webdavActiveWorkers > 0 && (millis() - start < 9000)) {
				vTaskDelay(pdMS_TO_TICKS(20));
			}
			for (int j = 0; j < WEBDAV_WORKER_COUNT; j++) {
				webdavTaskHandles[j] = nullptr;
			}
			System_IndicateError();
			return;
		}
	}
	System_IndicateOk();
}

void Webdav_DisableServer(void) {
	if (!webdavShouldRun && !webdavRunning) {
		return;
	}
	webdavShouldRun = false; // each worker closes the listener (the last one out) and self-deletes on its next iteration
	System_IndicateOk();
}

void Webdav_Exit(void) {
	// Signal the workers to stop; the GET/PUT transfer loops watch webdavShouldRun and bail out
	// fast, so an in-flight transfer no longer runs its full 8 s timeout before teardown. Wait long
	// enough (9 s) that every worker always finishes (the last one deletes webdavServer) before we
	// return - a shorter cap let a worker outlive teardown and dereference a WiFi stack that was
	// already being shut down.
	webdavShouldRun = false;
	uint32_t start = millis();
	while (webdavRunning && (millis() - start < 9000)) {
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

bool Webdav_IsServerRunning(void) {
	// Report the TARGET state, not the workers' teardown progress: after a stop request the
	// workers keep running for one more loop pass (longer mid-transfer), and the status
	// broadcast the web/command handlers send right after Enable/Disable raced that window -
	// the UI's on/off button then showed "on" although the server was already shutting down.
	// For every consumer (web UI button, MQTT state topic, the command-188 toggle) the intent
	// is the correct answer during that brief transition.
	return webdavShouldRun;
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
