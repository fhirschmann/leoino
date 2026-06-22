#include <Arduino.h>
#include "settings.h"

#include "Airplay.h"

#include "AudioPlayer.h"
#include "Log.h"
#include "Port.h"
#include "System.h"
#include "Web.h"
#include "Wlan.h"
#include "logmessages.h"

#ifdef MQTT_ENABLE
	#include "Mqtt.h"
#endif

#ifdef AIRPLAY_ENABLE
	#include <atomic>
	#include <cmath>
	#include <cstring>

	#include "ESP_I2S.h"
	#include "esp_err.h"

extern "C" {
	#include "esp_raop_receiver.h"
}

// Own I2S peripheral handle, set up exactly like the Bluetooth A2DP sink (Bluetooth.cpp). Because
// Airplay_Init() runs *after* AudioPlayer_Init() (which also calls setPinout), this begin() wins the
// GPIO routing and the idle ESP32-audioI2S `audio` object never drives the DAC in AirPlay mode.
static I2SClass airplayI2s;
static raop_handle_t *airplayRaop = nullptr;

static bool airplayI2sReady = false;
static bool airplayRaopStarted = false;
static uint32_t airplayWifiUpSince = 0;
static uint32_t airplayLastStartAttempt = 0;
static std::atomic<bool> airplayStreaming {false};

// Deferred-work flags. The RAOP callbacks can run in small-stack RTP/RTSP tasks, so they only flip
// these atomics; the heavy websocket/MQTT/volume/amp work is flushed from Airplay_Cyclic() (main loop).
static std::atomic<bool> airplayMetaDirty {false};
static std::atomic<int> airplayPendingVolume {-1}; // -1 = nothing pending
static bool airplayPublishedStreaming = false; // last streaming state pushed to UI (main-loop owned)

static char airplayDeviceName[64] = {0};
static char airplayNowPlaying[160] = {0};

// AirPlay carries the volume as a dB value in roughly [-30, 0] plus -144 for mute. Map it onto
// ESPuino's 0..AUDIOPLAYER_VOLUME_MAX scale purely so the web UI / LED ring / MQTT reflect what the
// phone shows; the actual attenuation is done in software by the RAOP component (RAOP_VOLUME_SOFTWARE).
static uint8_t airplayDbToVolume(float db) {
	if (db <= -144.0f) {
		return 0;
	}
	float v = (db + 30.0f) / 30.0f;
	if (v < 0.0f) {
		v = 0.0f;
	}
	if (v > 1.0f) {
		v = 1.0f;
	}
	return (uint8_t) lroundf(v * (float) AUDIOPLAYER_VOLUME_MAX);
}

// Called from the RAOP audio-output task (core 1) with decoded 16-bit/44.1kHz/stereo PCM. The blocking
// I2S write provides the playback back-pressure that paces the stream, just like the BT sink.
static void airplayAudioOutputCb(const uint8_t *data, size_t len, void *ctx) {
	(void) ctx;
	static uint32_t airplayCbCount = 0;
	size_t written = 0;
	if (airplayI2sReady && data && len) {
		written = airplayI2s.write((uint8_t *) data, len);
	}
	// DIAGNOSTIC: every ~250 frames (~2 s of audio) confirm PCM is reaching I2S.
	if ((airplayCbCount++ % 250) == 0) {
		Log_Printf(LOGLEVEL_NOTICE, "AirPlay PCM cb #%u: len=%u written=%u i2sReady=%d", airplayCbCount, (unsigned) len, (unsigned) written, airplayI2sReady);
	}
	System_UpdateActivityTimer(); // never fall asleep mid-stream
}

// Flush state accumulated by the RAOP callbacks. Runs from Airplay_Cyclic() in the main loop, where
// the stack is comfortable for the websocket/MQTT/volume work (the callbacks themselves may run in
// the RAOP RTP task, which only has a 4 KB stack).
static void airplayFlushDeferred(void) {
	bool streaming = airplayStreaming.load();
	if (streaming != airplayPublishedStreaming) {
		airplayPublishedStreaming = streaming;
		if (streaming) {
			AudioPlayer_SetupVolumeAndAmps(); // power the amp so the stream is audible
	#ifdef GPIO_PA_EN
			Log_Printf(LOGLEVEL_NOTICE, "AirPlay: streaming start, GPIO_PA_EN=%d", Port_Read(GPIO_PA_EN));
	#endif
		}
		Log_Println(streaming ? airplayClientConnected : airplayClientDisconnected, LOGLEVEL_NOTICE);
		Web_SendWebsocketData(0, WebsocketCodeType::AirplayStatus);
	#ifdef MQTT_ENABLE
		publishMqtt(topicAirplay, streaming ? "ON" : "OFF", false);
	#endif
	}

	if (airplayMetaDirty.exchange(false)) {
		if (airplayNowPlaying[0]) {
			Audio_setTitle("%s", airplayNowPlaying); // mirror to OLED / web track-info
		}
		Web_SendWebsocketData(0, WebsocketCodeType::AirplayStatus);
	}

	int pendingVol = airplayPendingVolume.exchange(-1);
	if (pendingVol >= 0 && (uint8_t) pendingVol != AudioPlayer_GetCurrentVolume()) {
		AudioPlayer_SetVolume(pendingVol); // reflect the phone's slider into UI/LED/MQTT
	}
}

// Called from the RAOP RTSP / RTP / command tasks on session events. Keep this light: only update
// atomics — the heavy work is deferred to airplayFlushDeferred() in the main loop.
static void airplayEventCb(raop_event_t event, void *event_data, void *ctx) {
	(void) ctx;
	switch (event) {
		case RAOP_EVENT_CONNECTED:
		case RAOP_EVENT_PLAYING:
			airplayStreaming.store(true);
			System_UpdateActivityTimer();
			break;

		case RAOP_EVENT_STOPPED:
		case RAOP_EVENT_DISCONNECTED:
		case RAOP_EVENT_PAUSED:
			airplayStreaming.store(false);
			airplayNowPlaying[0] = '\0';
			airplayMetaDirty.store(true);
			break;

		case RAOP_EVENT_METADATA: {
			// The metadata pointers are only valid for the duration of this call, so copy now.
			raop_metadata_t *meta = (raop_metadata_t *) event_data;
			const char *artist = (meta && meta->artist) ? meta->artist : "";
			const char *title = (meta && meta->title) ? meta->title : "";
			if (title[0] && artist[0]) {
				snprintf(airplayNowPlaying, sizeof(airplayNowPlaying), "%s - %s", artist, title);
			} else if (title[0]) {
				snprintf(airplayNowPlaying, sizeof(airplayNowPlaying), "%s", title);
			} else {
				airplayNowPlaying[0] = '\0';
			}
			airplayMetaDirty.store(true);
			break;
		}

		case RAOP_EVENT_VOLUME: {
			float db = event_data ? *(float *) event_data : 0.0f;
			airplayPendingVolume.store((int) airplayDbToVolume(db));
			break;
		}

		default:
			break;
	}
}

static void airplayResolveName(void) {
	String name = gPrefsSettings.getString("airplayName", "");
	if (name.isEmpty()) {
		name = Wlan_GetHostname();
	}
	if (name.isEmpty()) {
		name = nameAirplayDevice;
	}
	strncpy(airplayDeviceName, name.c_str(), sizeof(airplayDeviceName) - 1);
	airplayDeviceName[sizeof(airplayDeviceName) - 1] = '\0';
}
#endif // AIRPLAY_ENABLE

void Airplay_Init(void) {
#ifdef AIRPLAY_ENABLE
	if (System_GetOperationMode() != OPMODE_AIRPLAY) {
		return;
	}

	airplayResolveName();

	// Claim the I2S peripheral (same call sequence as the BT sink).
	airplayI2s.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT);
	if (!airplayI2s.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
		Log_Println(airplayI2sFailed, LOGLEVEL_ERROR);
		return;
	}
	airplayI2sReady = true;

	// Power up the amplifier so the first stream is audible immediately.
	AudioPlayer_SetupVolumeAndAmps();

	Log_Printf(LOGLEVEL_NOTICE, airplayModeStarted, airplayDeviceName);
#endif
}

void Airplay_Cyclic(void) {
#ifdef AIRPLAY_ENABLE
	if (System_GetOperationMode() != OPMODE_AIRPLAY || !airplayI2sReady) {
		return;
	}

	if (airplayRaopStarted) {
		airplayFlushDeferred(); // reflect streaming/metadata/volume changes into UI + MQTT
		return;
	}

	// The RAOP receiver needs a valid IP and the (ESPuino-owned) mDNS responder up before it can bind
	// its RTSP socket and advertise _raop._tcp. Wait until WiFi has been connected for a moment.
	if (!Wlan_IsConnected()) {
		airplayWifiUpSince = 0;
		return;
	}
	uint32_t now = millis();
	if (airplayWifiUpSince == 0) {
		airplayWifiUpSince = now;
	}
	if (now - airplayWifiUpSince < 3000) {
		return; // let DHCP/mDNS settle
	}
	if (airplayLastStartAttempt != 0 && (now - airplayLastStartAttempt) < 5000) {
		return; // back off between retries
	}
	airplayLastStartAttempt = now;

	raop_config_t cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.device_name = airplayDeviceName;
	cfg.volume_mode = RAOP_VOLUME_SOFTWARE; // phone slider scales the PCM
	cfg.mdns_mode = RAOP_MDNS_EXTERNAL; // ESPuino already called mdns_init() (Wlan.cpp / ESPmDNS)
	cfg.audio_output_cb = airplayAudioOutputCb;
	cfg.event_cb = airplayEventCb;
	cfg.user_ctx = nullptr;

	esp_err_t err = raop_init(&cfg, &airplayRaop);
	if (err != ESP_OK) {
		Log_Printf(LOGLEVEL_ERROR, airplayStartFailed, esp_err_to_name(err));
		return; // retry on the next cycle (back-off above)
	}

	airplayRaopStarted = true;
	Log_Printf(LOGLEVEL_NOTICE, airplayReceiverReady, airplayDeviceName);
#endif
}

void Airplay_Exit(void) {
#ifdef AIRPLAY_ENABLE
	// The reboot/power-down path tears everything down; like Bluetooth_Exit we just abandon the
	// receiver here rather than racing the RTSP/RTP tasks during shutdown.
	airplayStreaming = false;
#endif
}

bool Airplay_IsActive(void) {
#ifdef AIRPLAY_ENABLE
	return System_GetOperationMode() == OPMODE_AIRPLAY;
#else
	return false;
#endif
}

bool Airplay_IsRunning(void) {
#ifdef AIRPLAY_ENABLE
	return airplayStreaming.load();
#else
	return false;
#endif
}

void Airplay_Enable(void) {
#ifdef AIRPLAY_ENABLE
	System_SetOperationMode(OPMODE_AIRPLAY); // reboots if the mode actually changes
#endif
}

void Airplay_Disable(void) {
#ifdef AIRPLAY_ENABLE
	if (System_GetOperationMode() == OPMODE_AIRPLAY) {
		System_SetOperationMode(OPMODE_NORMAL); // reboots
	}
#endif
}

const char *Airplay_GetDeviceName(void) {
#ifdef AIRPLAY_ENABLE
	return airplayDeviceName;
#else
	return "";
#endif
}

const char *Airplay_GetNowPlaying(void) {
#ifdef AIRPLAY_ENABLE
	return airplayNowPlaying;
#else
	return "";
#endif
}
