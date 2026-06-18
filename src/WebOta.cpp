#include <Arduino.h>
#include "settings.h"

#include "Log.h"
#include "Mqtt.h"
#include "System.h"
#include "Web.h"
#include "logmessages.h"
#include "revision.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// GitHub OTA + passive version check. Split out of Web.cpp: the firmware flasher and the
// "is this the latest release?" badge check are self-contained and only talk to the rest of
// the web server through the public Web_* API declared in Web.h.

// Rolling "complete" firmware + version manifest published by the GitHub Actions release workflow.
static const char *githubFirmwareUrl = "https://github.com/fhirschmann/leoino/releases/download/latest/firmware.bin";
static const char *githubVersionUrl = "https://github.com/fhirschmann/leoino/releases/download/latest/version.json";

// State of the GitHub OTA, polled by the web interface via GET /githubupdate.
// 0 = idle, 1 = running/downloading, 2 = already up to date, 3 = failed
static volatile uint8_t gGithubOtaStatus = 0;
static volatile uint8_t gGithubOtaProgress = 0; // download progress in percent
static char gGithubOtaMsg[96] = "";
// gGithubOtaMsg is written by the OTA task (core 1) and read by the web server (core 0).
// A short spinlock keeps the web server from ever reading a half-written string.
static portMUX_TYPE gGithubOtaMsgMux = portMUX_INITIALIZER_UNLOCKED;

static void otaSetMessage(const char *msg) {
	char tmp[sizeof(gGithubOtaMsg)];
	snprintf(tmp, sizeof(tmp), "%s", msg ? msg : "");
	taskENTER_CRITICAL(&gGithubOtaMsgMux);
	memcpy(gGithubOtaMsg, tmp, sizeof(gGithubOtaMsg));
	taskEXIT_CRITICAL(&gGithubOtaMsgMux);
}

uint8_t Web_GetGithubOtaStatus(void) {
	return gGithubOtaStatus;
}

uint8_t Web_GetGithubOtaProgress(void) {
	return gGithubOtaProgress;
}

void Web_GetGithubOtaMessage(char *dst, size_t dstLen) {
	if (!dst || dstLen == 0) {
		return;
	}
	taskENTER_CRITICAL(&gGithubOtaMsgMux);
	size_t n = strnlen(gGithubOtaMsg, sizeof(gGithubOtaMsg) - 1);
	if (n >= dstLen) {
		n = dstLen - 1;
	}
	memcpy(dst, gGithubOtaMsg, n);
	taskEXIT_CRITICAL(&gGithubOtaMsgMux);
	dst[n] = '\0';
}

// Returns true if the latest release (its "describe" field) matches the running firmware revision.
static bool githubOtaIsUpToDate() {
	WiFiClientSecure client;
	client.setInsecure();
	client.setHandshakeTimeout(15); // bound the TLS handshake (default is 120s); GitHub's release CDN can stall after wake-from-sleep
	HTTPClient http;
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	http.setConnectTimeout(8000);
	http.setTimeout(8000); // read timeout, so a half-open connection can't hang the check
	bool upToDate = false;
	if (http.begin(client, githubVersionUrl)) {
		if (http.GET() == 200) {
			JsonDocument doc;
			if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
				String latest = doc["describe"].as<String>();
				// gitRevShort is the quoted `git describe` output, e.g. "\"abc1234\""
				String current = String(gitRevShort);
				current.replace("\"", "");
				if (latest.length() > 0 && latest == current) {
					upToDate = true;
				}
			}
		}
		http.end();
	}
	return upToDate;
}

// Background task: pull the latest firmware from GitHub over HTTPS and flash it via OTA.
// Runs in its own task because the download/flash blocks for a while; the async webserver must not block.
static void githubOtaTask(void *parameter) {
	gGithubOtaProgress = 0;
	otaSetMessage("");

	// Skip the (pointless and error-prone) re-flash if we already run the latest build.
	if (githubOtaIsUpToDate()) {
		Log_Println("GitHub OTA: already up to date", LOGLEVEL_NOTICE);
		gGithubOtaStatus = 2;
		vTaskDelete(NULL);
		return;
	}

	Log_Println("GitHub OTA: downloading latest firmware...", LOGLEVEL_NOTICE);
	System_PauseTasksDuringUpload(true);

	WiFiClientSecure *secureClient = new WiFiClientSecure;
	if (secureClient != nullptr) {
		secureClient->setInsecure(); // GitHub uses valid certs, but we don't bundle a CA store
		secureClient->setHandshakeTimeout(20); // bound the TLS handshake so a stuck GitHub-CDN connection fails instead of hanging the OTA forever
		httpUpdate.rebootOnUpdate(true);
		httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // github.com -> release-assets.githubusercontent.com
		httpUpdate.onProgress([](int cur, int total) {
			gGithubOtaProgress = (total > 0) ? (uint8_t) ((cur * 100) / total) : 0;
		});
		t_httpUpdate_return ret = httpUpdate.update(*secureClient, githubFirmwareUrl);
		if (ret == HTTP_UPDATE_FAILED) {
			char msg[96];
			snprintf(msg, sizeof(msg), "%d: %s", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
			otaSetMessage(msg);
			Log_Printf(LOGLEVEL_ERROR, "GitHub OTA failed (%s)", msg);
			gGithubOtaStatus = 3;
		} else if (ret == HTTP_UPDATE_NO_UPDATES) {
			gGithubOtaStatus = 2;
		}
		// on HTTP_UPDATE_OK the device reboots inside httpUpdate.update()
		delete secureClient;
	} else {
		gGithubOtaStatus = 3;
	}

	System_PauseTasksDuringUpload(false);
#ifdef MQTT_ENABLE
	// On success the device reboots inside httpUpdate.update() and re-announces itself; only the
	// terminal "no update needed"/"failed" states need to be reported back to MQTT here.
	publishMqtt(topicFirmwareUpdate, Web_GetGithubOtaStatusText(), false);
#endif
	vTaskDelete(NULL);
}

// Returns the current GitHub-OTA state as a short, MQTT/UI-friendly string.
const char *Web_GetGithubOtaStatusText(void) {
	switch (gGithubOtaStatus) {
		case 1:
			return "updating";
		case 2:
			return "up_to_date";
		case 3:
			return "failed";
		default:
			return "idle";
	}
}

// Start the GitHub OTA in the background (no-op if it is already running or the board lacks OTA support).
// Shared by the web endpoint, the CMD_FIRMWARE_UPDATE command and the MQTT firmware_update topic.
void Web_TriggerGithubOta(void) {
#ifdef BOARD_HAS_16MB_FLASH_AND_OTA_SUPPORT
	if (gGithubOtaStatus != 1) {
		gGithubOtaStatus = 1;
		gGithubOtaProgress = 0;
		otaSetMessage("");
		xTaskCreatePinnedToCore(githubOtaTask, "githubOta", 16384, NULL, 1, NULL, 1);
	}
#else
	Log_Println(otaNotSupported, LOGLEVEL_ERROR);
#endif
}

// Passive "is this build the latest release?" check, used only for the UI version badge
// (independent of the OTA flasher). -1 = unknown/not yet checked, 0 = update available, 1 = up to date.
static volatile int8_t gFirmwareUpToDate = -1;
static char gLatestBuild[24] = "";
static volatile uint32_t gLastVersionCheckMs = 0; // 0 = never; used to rate-limit re-checks
static volatile bool gVersionCheckRunning = false; // single-flight guard, see Web_CheckForUpdate()

int8_t Web_GetFirmwareUpToDate(void) {
	return gFirmwareUpToDate;
}

void Web_GetLatestBuild(char *dst, size_t dstLen) {
	if (!dst || dstLen == 0) {
		return;
	}
	strncpy(dst, gLatestBuild, dstLen - 1);
	dst[dstLen - 1] = '\0';
}

// Fetches the rolling release's version.json and compares it to the running build (background task).
static void versionCheckTask(void *parameter) {
	WiFiClientSecure client;
	client.setInsecure();
	client.setHandshakeTimeout(15); // bound the TLS handshake (default is 120s); GitHub's release CDN can stall after wake-from-sleep
	HTTPClient http;
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	http.setConnectTimeout(8000);
	http.setTimeout(8000); // read timeout, so a half-open connection can't pin this task (and its ~40 KB TLS context)
	if (http.begin(client, githubVersionUrl)) {
		if (http.GET() == 200) {
			JsonDocument doc;
			if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
				String latest = doc["describe"].as<String>();
				String latestBuild = doc["build"].as<String>();
				String current = String(gitRevShort);
				current.replace("\"", "");
				if (latest.length() > 0) {
					gFirmwareUpToDate = (latest == current) ? 1 : 0;
				}
				if (latestBuild.length() > 0) {
					strncpy(gLatestBuild, latestBuild.c_str(), sizeof(gLatestBuild) - 1);
					gLatestBuild[sizeof(gLatestBuild) - 1] = '\0';
				}
			}
		}
		http.end();
	}
	gVersionCheckRunning = false;
	vTaskDelete(NULL);
}

// Kick off a background version check (no-op on boards without OTA support).
// Rate-limited to once per minute so it can be called on every /version poll
// without re-checking the stored result going stale after new releases.
void Web_CheckForUpdate(void) {
#ifdef BOARD_HAS_16MB_FLASH_AND_OTA_SUPPORT
	// One check at a time: a stalled TLS handshake to GitHub's release CDN after wake-from-sleep would
	// otherwise let checks stack up (one per /version poll), each pinning ~40 KB of scarce internal heap
	// until the async webserver can no longer allocate response buffers and the web UI "loads forever".
	if (gVersionCheckRunning) {
		return;
	}
	uint32_t now = millis();
	if (gLastVersionCheckMs != 0 && (now - gLastVersionCheckMs) < 60000u) {
		return;
	}
	gLastVersionCheckMs = now;
	gVersionCheckRunning = true;
	if (xTaskCreatePinnedToCore(versionCheckTask, "verCheck", 8192, NULL, 1, NULL, 1) != pdPASS) {
		gVersionCheckRunning = false;
	}
#endif
}
