#include <Arduino.h>
#include "settings.h"

#include "HomeKit.h"

#ifdef HOMEKIT_ENABLE

	#include "AudioPlayer.h"
	#include "Battery.h"
	#include "Cmd.h"
	#include "Led.h"
	#include "Log.h"
	#include "System.h"
	#include "values.h"

	#include <HomeSpan.h>
	#include "qrcode.h" // ESP-IDF managed component (espressif__qrcode), already in the build

// HomeKit accessory (control + Siri) via HomeSpan.
//
// Threading model: HomeSpan's poll task runs on core 0 (see autoPoll below),
// the audio decoder + everything else on core 1. To avoid cross-core races on
// the audio object / ADC, the HomeKit update() handlers (core 0) only record an
// *intent*; HomeKit_Cyclic() runs from the main loop (core 1, like Button/Ir)
// and is the only place that calls into ESPuino or reads its state. State is
// mirrored back to the Home app from there too, so manual changes (buttons,
// RFID) show up on the iPhone.

	#include "esp_random.h"

// Setup ID baked into the pairing QR code. Must match the value handed to
// homeSpan.setQRID() so the QR HomeSpan advertises and the one we render agree.
	#define HOMEKIT_QR_ID "ESPU"
// HAP accessory category, must equal the one passed to homeSpan.begin() below so
// the QR payload matches what HomeSpan advertises over mDNS. The device is a
// bridge (controls + television accessories), so the QR advertises Bridges.
	#define HOMEKIT_CATEGORY ((uint8_t) Category::Bridges)

// --- Characteristic handles (owned by HomeSpan, updated from core 1) ---------
static SpanCharacteristic *gPlayPower = nullptr; // Switch: On == playing
static SpanCharacteristic *gVolOn = nullptr; // LightBulb: On == volume > 0
static SpanCharacteristic *gVolBright = nullptr; // LightBulb: Brightness == volume %
static SpanCharacteristic *gLockPower = nullptr; // Switch: On == controls locked
static SpanCharacteristic *gRepeatPower = nullptr; // Switch: On == playlist loop active
static SpanCharacteristic *gNightPower = nullptr; // Switch: On == LED night mode active
	#ifdef BATTERY_MEASURE_ENABLE
static SpanCharacteristic *gBattLevel = nullptr; // Battery level in %
static SpanCharacteristic *gBattLow = nullptr; // StatusLowBattery
	#endif

// --- Intents handed from core 0 (update) to core 1 (cyclic) ------------------
// 32-bit aligned scalars -> reads/writes are atomic on the ESP32, so plain
// volatile is enough for this single-producer/single-consumer hand-off.
static volatile int32_t gDesiredPlay = -1; // -1 none, 0 pause, 1 play
static volatile int32_t gDesiredLock = -1; // -1 none, 0 unlock, 1 lock
static volatile int32_t gDesiredRepeat = -1; // -1 none, 0 off, 1 on (playlist loop)
static volatile int32_t gDesiredNight = -1; // -1 none, 0 off, 1 on (LED night mode)
static volatile int32_t gPendingVolume = -1; // absolute target volume, -1 none

// Live pairing state, maintained via HomeSpan's pair callback (avoids pulling in
// HomeSpan's private HAP headers). HomeSpan fires this whenever a controller is
// added or removed; the web section reads it for its status badge.
static volatile bool gPaired = false;
static void HomeKit_PairCallback(boolean isPaired) {
	gPaired = isPaired;
}

// Render target for the QR display callback (web handler runs single-threaded).
static String *gQrSvgTarget = nullptr;

// Television service: power mirrors play state; remote keys + volume selector are
// momentary, so they go through a small command queue (core 0 -> core 1).
static SpanCharacteristic *gTvActive = nullptr;
static QueueHandle_t gCmdQueue = nullptr;
static inline void HomeKit_EnqueueCmd(uint16_t mod) {
	if (gCmdQueue != nullptr) {
		xQueueSend(gCmdQueue, &mod, 0);
	}
}

// Set by the web "regenerate code" button; handled in HomeKit_Cyclic (core 1) so
// the expensive SRP recompute never blocks the async web/TCP task.
static volatile bool gRegenRequested = false;

// User-configurable HomeKit names (persisted in NVS, applied at boot). Held in
// file-static Strings so the c_str() handed to HomeSpan stays valid for the
// program's lifetime. The TV name is what shows on the Control-Center remote.
static String gHkDeviceName;
static String gHkTvName;
static String gHkControlsName;
	#define HOMEKIT_DEFAULT_NAME "ESPuino"

static inline bool HomeKit_IsPlaying(void) {
	return gPlayProperties.pausePlay == false;
}

// --- Per-device pairing code -------------------------------------------------
// A hard-coded code would collide across multiple ESPuinos and be public to
// everyone running this firmware. Instead each device generates its own random
// 8-digit code on first boot and persists the plaintext in ESPuino's NVS (the
// matching SRP verifier lives in HomeSpan's own NVS). gSetupCodeGenerated tells
// HomeKit_Init() whether it must (re)write that verifier this boot.
static char gSetupCode[9] = {0};
static bool gSetupCodeGenerated = false;

static bool HomeKit_CodeAllowed(const char *c) {
	// HAP forbids these trivial codes (mirrors HomeSpan's Network_HS::allowedCode).
	static const char *const forbidden[] = {
		"00000000", "11111111", "22222222", "33333333", "44444444", "55555555",
		"66666666", "77777777", "88888888", "99999999", "12345678", "87654321"};
	for (const char *f : forbidden) {
		if (strcmp(c, f) == 0) {
			return false;
		}
	}
	return true;
}

// Returns the device's 8-digit setup code, generating + persisting one on first
// use. Sets gSetupCodeGenerated when a fresh code was just created.
static const char *HomeKit_RawSetupCode(void) {
	if (gSetupCode[0] != 0) {
		return gSetupCode;
	}
	const String stored = gPrefsSettings.getString("hkSetupCode", "");
	bool valid = stored.length() == 8 && HomeKit_CodeAllowed(stored.c_str());
	for (size_t i = 0; valid && i < 8; i++) {
		if (stored[i] < '0' || stored[i] > '9') {
			valid = false;
		}
	}
	if (valid) {
		strncpy(gSetupCode, stored.c_str(), 8);
		gSetupCode[8] = 0;
		return gSetupCode;
	}
	do {
		snprintf(gSetupCode, sizeof(gSetupCode), "%08u", (unsigned) (esp_random() % 100000000UL));
	} while (!HomeKit_CodeAllowed(gSetupCode));
	gPrefsSettings.putString("hkSetupCode", gSetupCode);
	gSetupCodeGenerated = true;
	Log_Printf(LOGLEVEL_NOTICE, "HomeKit: generated unique pairing code %.3s-%.2s-%.3s", gSetupCode, gSetupCode + 3, gSetupCode + 5);
	return gSetupCode;
}

// Force a brand-new code and rewrite HomeSpan's SRP verifier. Runs on core 1
// (main loop) because setPairingCode() does an expensive SRP computation. Existing
// pairings keep working (they use long-term keys, not the setup code); only the
// QR / setup code for *new* pairings changes.
static void HomeKit_DoRegenerate(void) {
	do {
		snprintf(gSetupCode, sizeof(gSetupCode), "%08u", (unsigned) (esp_random() % 100000000UL));
	} while (!HomeKit_CodeAllowed(gSetupCode));
	gPrefsSettings.putString("hkSetupCode", gSetupCode);
	homeSpan.setPairingCode(gSetupCode);
	Log_Printf(LOGLEVEL_NOTICE, "HomeKit: pairing code regenerated -> %.3s-%.2s-%.3s", gSetupCode, gSetupCode + 3, gSetupCode + 5);
}

// --- Services ----------------------------------------------------------------
struct HKPlayPause : Service::Switch {
	HKPlayPause()
		: Service::Switch() {
		new Characteristic::ConfiguredName("Playback");
		gPlayPower = new Characteristic::On(0);
	}
	boolean update() override {
		gDesiredPlay = gPlayPower->getNewVal() ? 1 : 0;
		return true;
	}
};

struct HKVolume : Service::LightBulb {
	HKVolume()
		: Service::LightBulb() {
		new Characteristic::ConfiguredName("Volume");
		gVolOn = new Characteristic::On(0);
		gVolBright = new Characteristic::Brightness(0);
		gVolBright->setRange(0, 100, 1);
	}
	boolean update() override {
		const bool on = gVolOn->getNewVal();
		const int pct = on ? (int) gVolBright->getNewVal() : 0;
		const int maxVol = AudioPlayer_GetMaxVolume() < 1 ? 1 : AudioPlayer_GetMaxVolume();
		gPendingVolume = (int32_t) ((pct * maxVol + 50) / 100); // round
		return true;
	}
};

struct HKLock : Service::Switch {
	HKLock()
		: Service::Switch() {
		new Characteristic::ConfiguredName("Button lock");
		gLockPower = new Characteristic::On(0);
	}
	boolean update() override {
		gDesiredLock = gLockPower->getNewVal() ? 1 : 0;
		return true;
	}
};

struct HKRepeat : Service::Switch {
	HKRepeat()
		: Service::Switch() {
		new Characteristic::ConfiguredName("Loop");
		gRepeatPower = new Characteristic::On(0);
	}
	boolean update() override {
		gDesiredRepeat = gRepeatPower->getNewVal() ? 1 : 0;
		return true;
	}
};

struct HKNight : Service::Switch {
	HKNight()
		: Service::Switch() {
		new Characteristic::ConfiguredName("Night mode");
		gNightPower = new Characteristic::On(0);
	}
	boolean update() override {
		gDesiredNight = gNightPower->getNewVal() ? 1 : 0;
		return true;
	}
};

// Television service: shows as a "TV" tile + an Apple-TV-style remote in Control
// Center. Power = play/pause; remote keys map to transport + volume.
struct HKTelevision : Service::Television {
	SpanCharacteristic *remoteKey;
	HKTelevision()
		: Service::Television() {
		new Characteristic::ConfiguredName(gHkTvName.c_str());
		gTvActive = new Characteristic::Active(0);
		new Characteristic::ActiveIdentifier(1);
		remoteKey = new Characteristic::RemoteKey();
	}
	boolean update() override {
		if (gTvActive->updated()) {
			gDesiredPlay = gTvActive->getNewVal() ? 1 : 0;
		}
		if (remoteKey->updated()) {
			switch (remoteKey->getNewVal()) {
				case 6:
					HomeKit_EnqueueCmd(CMD_PREVTRACK);
					break; // LEFT
				case 7:
					HomeKit_EnqueueCmd(CMD_NEXTTRACK);
					break; // RIGHT
				case 4:
					HomeKit_EnqueueCmd(CMD_VOLUMEUP);
					break; // UP
				case 5:
					HomeKit_EnqueueCmd(CMD_VOLUMEDOWN);
					break; // DOWN
				case 8: // CENTER
				case 11:
					HomeKit_EnqueueCmd(CMD_PLAYPAUSE);
					break; // PLAY_PAUSE
				default:
					break;
			}
		}
		return true;
	}
};

// One (required) input source so the Television service is well-formed.
struct HKTvInput : Service::InputSource {
	HKTvInput()
		: Service::InputSource() {
		new Characteristic::Identifier(1);
		new Characteristic::ConfiguredName("Player");
		new Characteristic::IsConfigured(1); // CONFIGURED
		new Characteristic::CurrentVisibilityState(0); // VISIBLE
	}
};

// Linked speaker: maps the remote's volume up/down to ESPuino volume.
struct HKTvSpeaker : Service::TelevisionSpeaker {
	SpanCharacteristic *volSelector;
	HKTvSpeaker()
		: Service::TelevisionSpeaker() {
		new Characteristic::ConfiguredName("Volume");
		new Characteristic::VolumeControlType(1); // RELATIVE -> uses VolumeSelector
		volSelector = new Characteristic::VolumeSelector();
		new Characteristic::Mute(0);
	}
	boolean update() override {
		if (volSelector->updated()) {
			HomeKit_EnqueueCmd(volSelector->getNewVal() == 0 ? CMD_VOLUMEUP : CMD_VOLUMEDOWN);
		}
		return true;
	}
};

	#ifdef BATTERY_MEASURE_ENABLE
struct HKBattery : Service::BatteryService {
	HKBattery()
		: Service::BatteryService() {
		gBattLevel = new Characteristic::BatteryLevel(100);
		gBattLow = new Characteristic::StatusLowBattery(0);
	}
};
	#endif

void HomeKit_Init(void) {
	// ESPuino owns the WiFi connection. HomeSpan is event-driven and rides the
	// existing link via the shared Arduino WiFi events, so we deliberately do NOT
	// hand it credentials or let it run its own captive portal. The
	// "WIFI CREDENTIALS DATA NOT FOUND" notice it prints is expected and harmless.

	// ESPuino's web interface already listens on port 80, so move the HAP server
	// off it to avoid a bind clash.
	homeSpan.setPortNum(1201);

	// Quiet HomeSpan's serial CLI so it doesn't fight ESPuino's logging/serial.
	homeSpan.setLogLevel(0);
	homeSpan.setSerialInputDisable(true);
	// Per-device pairing code. Only (re)write HomeSpan's SRP verifier when the
	// code was freshly generated -- createVerifyCode() is expensive, so we skip
	// it on every later boot (the verifier already persists in HomeSpan's NVS).
	const char *setupCode = HomeKit_RawSetupCode();
	if (gSetupCodeGenerated) {
		homeSpan.setPairingCode(setupCode);
	}
	homeSpan.setQRID(HOMEKIT_QR_ID);
	homeSpan.setPairCallback(HomeKit_PairCallback);

	gCmdQueue = xQueueCreate(8, sizeof(uint16_t));

	// Load the user-configurable names (defaults if unset).
	gHkDeviceName = gPrefsSettings.getString("hkDeviceName", HOMEKIT_DEFAULT_NAME);
	gHkTvName = gPrefsSettings.getString("hkTvName", HOMEKIT_DEFAULT_NAME);
	gHkControlsName = gHkDeviceName + " Controls";

	homeSpan.begin(Category::Bridges, gHkDeviceName.c_str());

	// Bridge with two functional accessories: the switches stay grouped under one
	// "Controls" device, while the Television service gets its own accessory so it
	// renders as a proper TV tile + Control-Center remote.
	new SpanAccessory(); // bridge root
	new Service::AccessoryInformation();
	new Characteristic::Identify();
	new Characteristic::Name(gHkDeviceName.c_str());
	new Characteristic::Manufacturer("ESPuino");
	new Characteristic::Model("ESPuino complete");

	new SpanAccessory(); // grouped controls
	new Service::AccessoryInformation();
	new Characteristic::Identify();
	new Characteristic::Name(gHkControlsName.c_str());
	new Characteristic::Manufacturer("ESPuino");
	new HKPlayPause();
	new HKVolume();
	new HKLock();
	new HKRepeat();
	new HKNight();
	#ifdef BATTERY_MEASURE_ENABLE
	new HKBattery();
	#endif

	new SpanAccessory(); // television
	new Service::AccessoryInformation();
	new Characteristic::Identify();
	new Characteristic::Name(gHkTvName.c_str());
	new Characteristic::Manufacturer("ESPuino");
	HKTelevision *tv = new HKTelevision();
	tv->addLink(new HKTvInput());
	tv->addLink(new HKTvSpeaker());

	// Own task on core 0 (WiFi side), low priority -- the audio task on core 1
	// can always preempt it, so the I2S DMA buffer never underruns.
	homeSpan.autoPoll(8192, 1, 0);

	Log_Println("HomeKit: HomeSpan started (autoPoll on core 0)", LOGLEVEL_NOTICE);
}

void HomeKit_Cyclic(void) {
	// --- deferred one-shot bring-up -------------------------------------------
	// homeSpan.begin() blocks the calling task for ~2 s (and several seconds more on
	// the first boot, computing the SRP verifier). We therefore keep it out of setup()
	// and fire it here on the first cyclic call, once BootComplete has lit the idle
	// animation and the main loop is already running.
	static bool initialized = false;
	if (!initialized) {
		initialized = true;
		HomeKit_Init();
		return;
	}

	// --- regenerate pairing code (web button) ---------------------------------
	if (gRegenRequested) {
		gRegenRequested = false;
		HomeKit_DoRegenerate();
	}

	// --- drain momentary commands from the TV remote / volume selector --------
	if (gCmdQueue != nullptr) {
		uint16_t mod;
		while (xQueueReceive(gCmdQueue, &mod, 0) == pdTRUE) {
			Cmd_Action(mod);
		}
	}

	// --- apply intents recorded by the HomeKit poll task (core 0) -------------
	if (gDesiredPlay >= 0) {
		if ((gDesiredPlay == 1) != HomeKit_IsPlaying()) {
			Cmd_Action(CMD_PLAYPAUSE);
		}
		gDesiredPlay = -1;
	}
	if (gDesiredLock >= 0) {
		if ((gDesiredLock == 1) != System_AreControlsLocked()) {
			Cmd_Action(CMD_LOCK_BUTTONS_MOD);
		}
		gDesiredLock = -1;
	}
	if (gDesiredRepeat >= 0) {
		// CMD_REPEAT_PLAYLIST toggles; only fire when the state actually differs.
		// It no-ops while idle, so the switch will snap back via the mirror below.
		if ((gDesiredRepeat == 1) != (bool) gPlayProperties.repeatPlaylist) {
			Cmd_Action(CMD_REPEAT_PLAYLIST);
		}
		gDesiredRepeat = -1;
	}
	if (gDesiredNight >= 0) {
		Led_SetNightmode(gDesiredNight == 1); // direct setter, no toggle guessing
		gDesiredNight = -1;
	}
	if (gPendingVolume >= 0) {
		AudioPlayer_SetVolume(gPendingVolume);
		gPendingVolume = -1;
	}

	// --- mirror current ESPuino state back to the Home app at ~1 Hz -----------
	static uint32_t lastReflect = 0;
	if (millis() - lastReflect < 1000) {
		return;
	}
	lastReflect = millis();

	const bool playing = HomeKit_IsPlaying();
	if (gPlayPower && gPlayPower->getVal() != playing) {
		gPlayPower->setVal(playing);
	}
	if (gTvActive && gTvActive->getVal() != playing) {
		gTvActive->setVal(playing);
	}

	const int maxVol = AudioPlayer_GetMaxVolume() < 1 ? 1 : AudioPlayer_GetMaxVolume();
	const int curVol = AudioPlayer_GetCurrentVolume();
	const int pct = (curVol * 100 + maxVol / 2) / maxVol;
	if (gVolBright && gVolBright->getVal() != pct) {
		gVolBright->setVal(pct);
	}
	if (gVolOn && gVolOn->getVal() != (curVol > 0)) {
		gVolOn->setVal(curVol > 0);
	}

	if (gLockPower) {
		const bool locked = System_AreControlsLocked();
		if (gLockPower->getVal() != locked) {
			gLockPower->setVal(locked);
		}
	}

	if (gRepeatPower) {
		const bool repeat = gPlayProperties.repeatPlaylist;
		if (gRepeatPower->getVal() != repeat) {
			gRepeatPower->setVal(repeat);
		}
	}

	if (gNightPower) {
		const bool night = Led_GetNightmode();
		if (gNightPower->getVal() != night) {
			gNightPower->setVal(night);
		}
	}

	#ifdef BATTERY_MEASURE_ENABLE
	int lvl = (int) (Battery_EstimateLevel() * 100.0F + 0.5F);
	lvl = lvl < 0 ? 0 : (lvl > 100 ? 100 : lvl);
	if (gBattLevel && gBattLevel->getVal() != lvl) {
		gBattLevel->setVal(lvl);
	}
	const int low = Battery_IsLow() ? 1 : 0;
	if (gBattLow && gBattLow->getVal() != low) {
		gBattLow->setVal(low);
	}
	#endif
}

// --- Web interface helpers ---------------------------------------------------
bool HomeKit_IsEnabled(void) {
	return true;
}

bool HomeKit_IsPaired(void) {
	return gPaired;
}

const char *HomeKit_GetSetupCode(void) {
	// HomeKit shows the 8-digit code grouped as ddd-dd-ddd.
	static char formatted[11] = {0};
	const char *c = HomeKit_RawSetupCode();
	snprintf(formatted, sizeof(formatted), "%.3s-%.2s-%.3s", c, c + 3, c + 5);
	return formatted;
}

String HomeKit_GetSetupPayload(void) {
	HapQR qr;
	return String(qr.get((uint32_t) atol(HomeKit_RawSetupCode()), HOMEKIT_QR_ID, HOMEKIT_CATEGORY, HapQR::IP));
}

// QR display callback invoked by esp_qrcode_generate(): emit the matrix as SVG,
// collapsing runs of dark modules per row to keep the response small.
static void HomeKit_QrToSvg(esp_qrcode_handle_t qr) {
	if (gQrSvgTarget == nullptr) {
		return;
	}
	String &svg = *gQrSvgTarget;
	const int n = esp_qrcode_get_size(qr);
	const int quiet = 3; // mandatory quiet zone so scanners lock on
	const int dim = n + 2 * quiet;

	svg.reserve(900 + n * n * 6);
	svg += F("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 ");
	svg += dim;
	svg += ' ';
	svg += dim;
	svg += F("' shape-rendering='crispEdges'>");
	svg += F("<rect width='100%' height='100%' fill='#ffffff'/>");
	for (int y = 0; y < n; y++) {
		int x = 0;
		while (x < n) {
			if (!esp_qrcode_get_module(qr, x, y)) {
				x++;
				continue;
			}
			const int runStart = x;
			while (x < n && esp_qrcode_get_module(qr, x, y)) {
				x++;
			}
			svg += F("<rect x='");
			svg += (runStart + quiet);
			svg += F("' y='");
			svg += (y + quiet);
			svg += F("' width='");
			svg += (x - runStart);
			svg += F("' height='1' fill='#000000'/>");
		}
	}
	svg += F("</svg>");
}

String HomeKit_GetQrSvg(void) {
	const String payload = HomeKit_GetSetupPayload();

	String svg;
	gQrSvgTarget = &svg;
	esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
	cfg.display_func = HomeKit_QrToSvg;
	cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
	esp_qrcode_generate(&cfg, payload.c_str());
	gQrSvgTarget = nullptr;
	return svg;
}

void HomeKit_ResetPairing(void) {
	// 'U' clears all controller data and tears down connections without a reboot.
	homeSpan.processSerialCommand("U");
	Log_Println("HomeKit: pairing reset via web interface", LOGLEVEL_NOTICE);
}

void HomeKit_RequestRegenerate(void) {
	// Deferred to HomeKit_Cyclic (core 1): the SRP recompute is too heavy for the
	// async web/TCP task.
	gRegenRequested = true;
}

String HomeKit_GetDeviceName(void) {
	return gPrefsSettings.getString("hkDeviceName", HOMEKIT_DEFAULT_NAME);
}

String HomeKit_GetTvName(void) {
	return gPrefsSettings.getString("hkTvName", HOMEKIT_DEFAULT_NAME);
}

void HomeKit_SetNames(const String &deviceName, const String &tvName) {
	// Fall back to the default for empty input; takes effect on next reboot.
	gPrefsSettings.putString("hkDeviceName", deviceName.length() ? deviceName : String(HOMEKIT_DEFAULT_NAME));
	gPrefsSettings.putString("hkTvName", tvName.length() ? tvName : String(HOMEKIT_DEFAULT_NAME));
	Log_Println("HomeKit: names updated (restart to apply)", LOGLEVEL_NOTICE);
}

#else // HOMEKIT_ENABLE

void HomeKit_Init(void) {
}
void HomeKit_Cyclic(void) {
}
bool HomeKit_IsEnabled(void) {
	return false;
}
bool HomeKit_IsPaired(void) {
	return false;
}
const char *HomeKit_GetSetupCode(void) {
	return "";
}
String HomeKit_GetSetupPayload(void) {
	return String();
}
String HomeKit_GetQrSvg(void) {
	return String();
}
void HomeKit_ResetPairing(void) {
}
void HomeKit_RequestRegenerate(void) {
}
String HomeKit_GetDeviceName(void) {
	return String();
}
String HomeKit_GetTvName(void) {
	return String();
}
void HomeKit_SetNames(const String &, const String &) {
}

#endif
