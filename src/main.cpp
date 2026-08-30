// !!! MAKE SURE TO EDIT settings.h !!!
#include <Arduino.h>
#include "settings.h" // Contains all user-relevant settings (general)

#include "main.h"

#include "AudioPlayer.h"
#include "Backup.h"
#include "Battery.h"
#include "Bluetooth.h"
#include "Button.h"
#include "Cmd.h"
#include "Common.h"
#include "CrashDump.h"
#include "Display.h"
#include "Ftp.h"
#include "HallEffectSensor.h"
#include "HomeKit.h"
#include "I2cSupervisor.h"
#include "IrReceiver.h"
#include "Led.h"
#include "Log.h"
#include "MemX.h"
#include "Mqtt.h"
#include "Port.h"
#include "Power.h"
#include "Queues.h"
#include "Rfid.h"
#include "RfidConfig.h"
#include "RfidSync.h"
#include "RotaryEncoder.h"
#include "Rtc.h"
#include "SdCard.h"
#include "System.h"
#include "Web.h"
#include "Webdav.h"
#include "Wlan.h"
#include "gitrevision.h"

#include <Wire.h>
#include <esp_log.h>

bool gRetryRfidOnWifiConnect = false;
char gRetryRfidTagId[cardIdStringSize] = "";

static constexpr const char *logo = R"literal(
 _____   ____    ____            _
| ____| / ___|  |  _ \   _   _  (_)  _ __     ___
|  _|   \__  \  | |_) | | | | | | | | '_ \   / _ \
| |___   ___) | |  __/  | |_| | | | | | | | | (_) |
|_____| |____/  |_|      \__,_| |_| |_| |_|  \___/
         Rfid-controlled musicplayer


)literal";

// avoid PSRAM check while wake-up from deepsleep
bool testSPIRAM(void) {
	return true;
}

bool recoverLastRfid = true;
bool recoverBootCount = true;
bool resetBootCount = false;
uint32_t bootCount = 0;

////////////

// I2C
#ifdef I2C_2_ENABLE
TwoWire i2cBusTwo = TwoWire(1);
#endif

// If a problem occurs, remembering last rfid can lead into a boot loop that's hard to escape of.
// That reason for a mechanism is necessary to prevent this.
// At start of a boot, bootCount is incremented by one and after 30s decremented because
// uptime of 30s is considered as "successful boot".
void recoverBootCountFromNvs(void) {
	if (recoverBootCount) {
		recoverBootCount = false;
		resetBootCount = true;
		bootCount = gPrefsSettings.getUInt("bootCount", 999);

		if (bootCount == 999) { // first init
			bootCount = 1;
			gPrefsSettings.putUInt("bootCount", bootCount);
		} else if (bootCount >= 3) { // considered being a bootloop => don't recover last rfid!
			bootCount = 1;
			gPrefsSettings.putUInt("bootCount", bootCount);
			gPrefsSettings.putString("lastRfid", "-1"); // reset last rfid
			Log_Println(bootLoopDetected, LOGLEVEL_ERROR);
			recoverLastRfid = false;
		} else { // normal operation
			gPrefsSettings.putUInt("bootCount", ++bootCount);
		}
	}

	if (resetBootCount && millis() >= 30000) { // reset bootcount
		resetBootCount = false;
		bootCount = 0;
		gPrefsSettings.putUInt("bootCount", bootCount);
		Log_Println(noBootLoopDetected, LOGLEVEL_INFO);
	}
}

// Get last RFID-tag applied from NVS
void recoverLastRfidPlayedFromNvs(bool force) {
	if (recoverLastRfid || force) {
		if (System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) { // Don't recover if BT-mode is desired
			recoverLastRfid = false;
			return;
		}
		recoverLastRfid = false;
		String lastRfidPlayed = gPrefsSettings.getString("lastRfid", "-1");
		if (!lastRfidPlayed.compareTo("-1")) {
			Log_Println(unableToRestoreLastRfidFromNVS, LOGLEVEL_INFO);
		} else {
			Rfid_ResetOldRfid();
			xQueueSend(gRfidCardQueue, lastRfidPlayed.c_str(), 0);
			Log_Printf(LOGLEVEL_INFO, restoredLastRfidFromNVS, lastRfidPlayed.c_str());
		}
	}
}

void setup() {
	// Keep the unpowered WS2812 chain from loading/back-powering the complete board while its
	// PCA9555-controlled peripheral rail is still coming up. FastLED takes over this pin later.
	Led_PrepareForBoot();
	Log_Init();
	CrashDump_Init();
	// Silence the IDF mDNS component's own logging (one tag per source file). Its announce
	// and TXT paths log an ERROR when an allocation fails under memory pressure -- and if
	// that is the calling task's first ever print, newlib lazily allocates the task's stdio
	// lock, which is another malloc at the exact moment the heap is empty and abort()s the
	// whole box (observed twice in the boot trough: in the mdns task's announce and in
	// HomeSpan's poll task setting HAP TXT records). Without the log line mDNS just drops
	// that one packet / retries the record later; the box stays up.
	static const char *mdnsLogTags[] = {"mdns", "mdns_browser", "mdns_netif", "mdns_networking", "mdns_pcb", "mdns_querier", "mdns_receive", "mdns_responder", "mdns_send", "mdns_service", "mdns_utils"};
	for (const char *tag : mdnsLogTags) {
		esp_log_level_set(tag, ESP_LOG_NONE);
	}
	Queues_Init();

	// Make sure all wakeups can be enabled *before* initializing RFID, which can enter sleep immediately
	Button_Init(); // To preseed internal button-storage with values

	System_Init_Rfid_Prefs();
	const bool pn5180LpcdEnabled = gPrefsRfid.getBool("pn5180Lpcd", false);
	if (pn5180LpcdEnabled) {
		// Only handle a possible deep-sleep wakeup here; starting the RFID scanning task
		// this early raced with the peripheral init below and could hang the boot (see
		// Rfid_StartTask() call further down).
		Rfid_WakeupHandling();
	}
	System_Init();

// Init 2nd i2c-bus if RC522 is used with i2c or if port-expander is enabled
#ifdef I2C_2_ENABLE
	I2cSupervisor_Begin();
	delay(50);
	Log_Println(rfidScannerReady, LOGLEVEL_DEBUG);
#endif

	// Needs i2c first if port-expander is used
	// The complete board cannot enable its peripheral rail without the PCA9555. Never continue into
	// an apparent SD-card failure after a missed cold-start probe. Claim the expander before talking
	// to RTC/OLED devices on this same bus, reducing traffic during the cold-start window. Hardware
	// must still avoid I2C pull-ups to the initially-off switched rail, which can hold the bus down
	// when that rail also supplies a connected Neopixel chain.
	while (!Port_Init()) {
		I2cSupervisor_Recover("port-expander startup probe");
		delay(250);
	}

	// Init RTC after PE114 is in a defined safe-OFF state, but still early enough to seed the system
	// clock before WiFi/NTP.
	Rtc_Init();

#ifdef HALLEFFECT_SENSOR_ENABLE
	gHallEffectSensor.init();
#endif

	// If port-expander is used, port_init has to be called first, as power can be (possibly) done by port-expander
	Power_Init();

	Battery_Init();

	// Init audio before power on to avoid speaker noise
	AudioPlayer_Init();
	Backup_Init(); // daily auto-backup of the full config to the sync server
	RfidSync_Init(); // open the RFID-sync NVS namespaces + mutex/push-task once, single-threaded (avoids the lazy-init race)

	// All checks that could send us to sleep are done, power up fully
	while (!Power_PeripheralOn()) {
		Log_Println("Unable to enable peripheral power via port-expander; retrying", LOGLEVEL_ERROR);
		I2cSupervisor_Recover("peripheral power write");
		delay(250);
		while (!Port_Init()) {
			I2cSupervisor_Recover("port-expander power retry");
			delay(250);
		}
	}

	// Needs power first
	SdCard_Init();

	// Do not start the comparatively high-current Neopixel boot animation until the SD card is
	// mounted. On a full USB cold start the LED inrush otherwise overlaps the peripheral rail and
	// SD power-up, which can leave the PCA9555/SD path in an unrecoverable state.
	Led_Init();

	// OLED on the ext-I2C bus (i2cBusTwo). Initialised after SdCard_Init so the SD comes up first
	// (ext-I2C shares pins with the SD-MMC D1/D3 lines).
#ifdef OLED_ENABLE
	Display_Init();
#endif

	// welcome message
	Serial.print(logo);

	// Software-version
	Log_Println(softwareRevision, LOGLEVEL_NOTICE);
	Log_Println(gitRevision, LOGLEVEL_NOTICE);
	Log_Printf(LOGLEVEL_NOTICE, "Arduino version: %d.%d.%d", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
	Log_Printf(LOGLEVEL_NOTICE, "ESP-IDF version: %s", ESP.getSdkVersion());

	// print wake-up reason
	System_ShowWakeUpReason();
	// print SD card info
	SdCard_PrintInfo();

	Ftp_Init();
	Webdav_Init();
	if (!pn5180LpcdEnabled) {
		Rfid_Init();
	} else {
		// Peripherals are up now, safe to start the scanning task deferred above.
		Rfid_StartTask();
	}
	RotaryEncoder_Init();
	Bluetooth_Init();
	Wlan_Init();
	Mqtt_Init();

	if (OPMODE_NORMAL == System_GetOperationMode()) {
		Wlan_Cyclic();
	}

	// HomeKit is brought up lazily on the first HomeKit_Cyclic() call (see HomeKit.cpp),
	// *after* BootComplete below. homeSpan.begin() blocks for ~2 s every boot (and several
	// more on the very first boot, while it computes the SRP pairing verifier); doing that
	// here held the boot-LED animation and stalled the main loop for the whole duration.

	IrReceiver_Init();
	Button_StartSampler(); // capture quick physical taps even while a synchronous SD track-open blocks loop()
	System_UpdateActivityTimer(); // initial set after boot
	Led_Indicate(LedIndicatorType::BootComplete);

	if (System_IsColdStart()) {
		AudioPlayer_PlayReadyMsg();
	}

	Log_Printf(LOGLEVEL_DEBUG, "%s: %u", freeHeapAfterSetup, ESP.getFreeHeap());
	if (psramFound()) {
		Log_Printf(LOGLEVEL_DEBUG, "PSRAM: %u bytes", ESP.getPsramSize());
	} else {
		Log_Println("PSRAM: --", LOGLEVEL_DEBUG);
	}
	Log_Printf(LOGLEVEL_DEBUG, "Flash-size: %u bytes", ESP.getFlashChipSize());

	// setup timezone & show internal RTC date/time if available
	setenv("TZ", timeZone, 1);
	tzset();
	struct tm timeinfo;
	if (getLocalTime(&timeinfo, 5)) {
		static char timeStringBuff[255];
		snprintf(timeStringBuff, sizeof(timeStringBuff), dateTimeRTC, timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
		Log_Println(timeStringBuff, LOGLEVEL_DEBUG);
	}

	if (Wlan_IsConnected()) {
		Log_Printf(LOGLEVEL_DEBUG, "RSSI: %d dBm", Wlan_GetRssi());
	}

#ifdef CONTROLS_LOCKED_BY_DEFAULT
	System_SetLockControls(true);
#endif
}

void loop() {
	Wlan_Cyclic();
	Web_Cyclic();
	if (OPMODE_BLUETOOTH_SINK == System_GetOperationMode()) {
		// bluetooth speaker mode
		Bluetooth_Cyclic();
	} else if (OPMODE_BLUETOOTH_SOURCE == System_GetOperationMode()) {
		// bluetooth headset mode
		Bluetooth_Cyclic();
	}
	Button_Cyclic(); // apply buffered button edges before rotary gestures and audio consume their command
	RotaryEncoder_Cyclic();
	Ftp_Cyclic();
	Webdav_Cyclic();
	AudioPlayer_Cyclic();
	Battery_Cyclic();
	Rtc_Cyclic();
	RfidSync_Cyclic();
	Backup_Cyclic();
	System_Cyclic();
#ifdef OLED_ENABLE
	Display_Cyclic();
#endif
	Rfid_PreferenceLookupHandler();

	// Read once and cache: this runs on every loop() iteration (~150x/s) but the setting only matters
	// during the post-boot recovery window. An NVS lookup every loop forever was pure waste.
	static int8_t playLastRfidAfterReboot = -1;
	if (playLastRfidAfterReboot < 0) {
		playLastRfidAfterReboot = gPrefsSettings.getBool("playLastOnBoot", false) ? 1 : 0;
	}

	if (playLastRfidAfterReboot) {
		recoverBootCountFromNvs();
		recoverLastRfidPlayedFromNvs();
	}

	IrReceiver_Cyclic();
	HomeKit_Cyclic();

#ifdef HALLEFFECT_SENSOR_ENABLE
	gHallEffectSensor.cyclic();
#endif

	vTaskDelay(portTICK_PERIOD_MS * 6u);
}
