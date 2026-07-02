#include <Arduino.h>
#include "settings.h"

#include "Net.h"

#include "System.h"

#include <WiFiClientSecure.h>

std::unique_ptr<WiFiClient> Net_MakeClient(const String &url) {
	if (url.startsWith("https://")) {
		auto *secure = new WiFiClientSecure;
		secure->setInsecure(); // no bundled CA store; the sync server is LAN/self-hosted
		secure->setHandshakeTimeout(20); // bound the TLS handshake so a stuck connection fails instead of hanging
		return std::unique_ptr<WiFiClient>(secure);
	}
	return std::unique_ptr<WiFiClient>(new WiFiClient);
}

void Net_SetupHttp(HTTPClient &http, const String &user, const String &pass, uint32_t readTimeoutMs) {
	http.setConnectTimeout(8000);
	http.setTimeout(readTimeoutMs);
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	if (user.length() > 0) {
		http.setAuthorization(user.c_str(), pass.c_str()); // HTTP Basic Auth
	}
}

void Net_GetSyncCreds(String &user, String &pass) {
	user = gPrefsSettings.getString("syncUser", "");
	pass = gPrefsSettings.getString("syncPwd", "");
}

// One binary semaphore, lazily created "available" on first use. A binary semaphore (not a mutex) is
// required because the slot is claimed by the trigger task and released by the spawned worker task —
// different tasks, which a mutex forbids. The C++11 magic-static guard makes the one-time creation
// thread-safe, so concurrent first-callers on both cores can't double-create it.
static SemaphoreHandle_t bgJobSlot(void) {
	static SemaphoreHandle_t slot = []() {
		SemaphoreHandle_t s = xSemaphoreCreateBinary();
		if (s != NULL) {
			xSemaphoreGive(s); // start in the "available" state
		}
		return s;
	}();
	return slot;
}

bool Net_TryClaimBgJob(void) {
	SemaphoreHandle_t s = bgJobSlot();
	if (s == NULL) {
		return true; // couldn't allocate the gate; don't block the job on it
	}
	return xSemaphoreTake(s, 0) == pdTRUE;
}

void Net_ReleaseBgJob(void) {
	SemaphoreHandle_t s = bgJobSlot();
	if (s != NULL) {
		xSemaphoreGive(s);
	}
}
