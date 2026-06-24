#include <Arduino.h>
#include "settings.h"

#include "ArduinoJson.h"
#include "AsyncJson.h"
#include "AudioPlayer.h"
#include "Battery.h"
#include "Bluetooth.h"
#include "Cmd.h"
#include "Common.h"
#include "ESPAsyncWebServer.h"
#include "EnumUtils.h"
#include "Ftp.h"
#include "HallEffectSensor.h"
#include "HomeKit.h"
#include "IrReceiver.h"
#include "Led.h"
#include "Log.h"
#include "MemX.h"
#include "Mqtt.h"
#include "Playstats.h"
#include "Rfid.h"
#include "RfidSync.h"
#include "RotaryEncoder.h"
#include "Rtc.h"
#include "SdCard.h"
#include "Sync.h"
#include "System.h"
#include "Web.h"
#include "WebInternal.h"
#include "Webdav.h"
#include "Wlan.h"
#include "revision.h"

// Settings serialization + the settings/equalizer-rule/operation-mode endpoints, extracted from
// Web.cpp. Web.cpp registers these handlers (declared in WebInternal.h) in webserverStart and owns
// the websocket dispatcher JSONToSettings (which handlePostSettings, still in Web.cpp, calls).

// process settings to JSON object
void settingsToJSON(JsonObject obj, const String section) {
	if ((section == "") || (section == "current")) {
		// current values
		JsonObject curObj = obj["current"].to<JsonObject>();
		curObj["volume"].set(AudioPlayer_GetCurrentVolume());
		curObj["rfidTagId"] = String(gCurrentRfidTagId);
	}
	if ((section == "") || (section == "general")) {
		// general settings
		JsonObject generalObj = obj["general"].to<JsonObject>();
		generalObj["initVolume"].set(gPrefsSettings.getUInt("initVolume", 3));
		generalObj["maxVolumeSp"].set(gPrefsSettings.getUInt("maxVolumeSp", 21));
		generalObj["maxVolumeHp"].set(gPrefsSettings.getUInt("maxVolumeHp", 21));
		generalObj["sleepInactivity"].set(gPrefsSettings.getUInt("mInactiviyT", 10));
		generalObj["playMono"].set(gPrefsSettings.getBool("playMono", false));
		generalObj["savePosShutdown"].set(gPrefsSettings.getBool("savePosShutdown", false)); // SAVE_PLAYPOS_BEFORE_SHUTDOWN
		generalObj["savePosRfidChge"].set(gPrefsSettings.getBool("savePosRfidChge", false)); // SAVE_PLAYPOS_WHEN_RFID_CHANGE
		generalObj["savePosPeriodic"].set(gPrefsSettings.getBool("savePosPeriodic", true)); // periodic audiobook play-position checkpoint
		generalObj["restartFreshHrs"].set(gPrefsSettings.getUInt("freshAfterHrs", 24)); // restart audiobooks from start after this idle gap (0 = off)
		generalObj["minResumeSec"].set(gPrefsSettings.getUInt("minResumeSec", 20)); // don't resume an audiobook pulled within the first N seconds (0 = off)
		generalObj["seekStep"].set(gPrefsSettings.getUInt("seekStep", seekStepDefault)); // step (s) for smart forward/backward in-file seeking
		generalObj["playLastRfidOnReboot"].set(gPrefsSettings.getBool("playLastOnBoot", false)); // PLAY_LAST_RFID_AFTER_REBOOT
		generalObj["pauseIfRfidRemoved"].set(gPrefsSettings.getBool("pauseRfidRem", false)); // PAUSE_WHEN_RFID_REMOVED
		generalObj["stopIfRfidRemoved"].set(gPrefsSettings.getBool("stopRfidRem", false)); // stop instead of pause when RFID is removed
		generalObj["dontAcceptRfidTwice"].set(gPrefsSettings.getBool("dAccRfidTwice", false)); // DONT_ACCEPT_SAME_RFID_TWICE
		generalObj["rfidReaderType"].set(gPrefsRfid.getUChar("rfidReaderType", 0)); // RFID_READER_TYPE_RUNTIME
		generalObj["pn5180Lpcd"].set(gPrefsRfid.getBool("pn5180Lpcd", false)); // PN5180 LPCD
		generalObj["slix2Password"].set(gPrefsRfid.getString("slix2Pwd", "")); // SLIX2 Password
		generalObj["mfrc522Gain"].set(gPrefsRfid.getUChar("mfrc522Gain", 7)); // MFRC522_GAIN
		generalObj["pauseOnMinVol"].set(gPrefsSettings.getBool("pauseOnMinVol", false)); // PAUSE_ON_MIN_VOLUME
		generalObj["recoverVolBoot"].set(gPrefsSettings.getBool("recoverVolBoot", false)); // USE_LAST_VOLUME_AFTER_REBOOT
		generalObj["volumeCurve"].set(gPrefsSettings.getUChar("volumeCurve", 0)); // VOLUMECURVE
		generalObj["readyPath"].set(gPrefsSettings.getString("readyPath", "/ready.mp3")); // READY_PATH
		generalObj["playStartupSnd"].set(gPrefsSettings.getBool("playStartupSnd", true)); // play the ready sound on cold start
		generalObj["noSleepWhenPowered"].set(gPrefsSettings.getBool("noSleepPwr", false)); // stay awake on external power
		generalObj["poweredVoltage"].set(gPrefsSettings.getFloat("pwrSleepVolt", 3.5f)); // voltage threshold for "powered"
		// single custom brand text for both navbar header and footer (empty = default).
		// fall back to the legacy uiHeader key so existing installs keep their brand.
		generalObj["brandText"].set(gPrefsSettings.getString("uiBrand", gPrefsSettings.getString("uiHeader", "")));
	}
	if ((section == "") || (section == "equalizer")) {
		// equalizer settings
		JsonObject equalizerObj = obj["equalizer"].to<JsonObject>();
		equalizerObj["gainLowPass"].set(gPrefsSettings.getChar("gainLowPass", 0));
		equalizerObj["gainBandPass"].set(gPrefsSettings.getChar("gainBandPass", 0));
		equalizerObj["gainHighPass"].set(gPrefsSettings.getChar("gainHighPass", 0));
		equalizerObj["profile"].set(gPrefsSettings.getString("eqProfile", "custom"));
	}
	if ((section == "") || (section == "wifi")) {
		// WiFi settings
		JsonObject wifiObj = obj["wifi"].to<JsonObject>();
		wifiObj["hostname"] = Wlan_GetHostname();
		wifiObj["scanOnStart"].set(gPrefsSettings.getBool("ScanWiFiOnStart", false));
	}
	if (section == "ssids") {
		// saved SSID's
		JsonObject ssidsObj = obj["ssids"].to<JsonObject>();
		JsonArray ssidArr = ssidsObj["savedSSIDs"].to<JsonArray>();
		Wlan_GetSavedNetworks([ssidArr](const WiFiSettings &network) {
			ssidArr.add(network.ssid);
		});

		// active SSID
		if (Wlan_IsConnected()) {
			ssidsObj["active"] = Wlan_GetCurrentSSID();
		}
	}
#ifdef NEOPIXEL_ENABLE
	if ((section == "") || (section == "led")) {
		// LED settings
		JsonObject ledObj = obj["led"].to<JsonObject>();
		ledObj["initBrightness"].set(gPrefsSettings.getUChar("iLedBrightness", 0));
		ledObj["nightBrightness"].set(gPrefsSettings.getUChar("nLedBrightness", 0));
		ledObj["atmoBrightness"].set(gPrefsSettings.getUChar("aLedBrightness", 0));
		ledObj["numIndicator"].set(gPrefsSettings.getUChar("numIndicator", NUM_INDICATOR_LEDS));
		uint8_t numControlLeds = gPrefsSettings.getUChar("numControl", NUM_CONTROL_LEDS);
		ledObj["numControl"].set(numControlLeds);
		ledObj["controlLedsEnabled"].set(gPrefsSettings.getBool("ctrlLedsOn", true));
		if (numControlLeds > 0) {
			// get control led colors from NVS
			std::vector<uint32_t> controlLedColors = CONTROL_LEDS_COLORS;
			size_t keySize = gPrefsSettings.getBytesLength("controlColors");
			if (keySize == (numControlLeds * sizeof(uint32_t))) {
				controlLedColors.resize(numControlLeds);
				gPrefsSettings.getBytes("controlColors", controlLedColors.data(), keySize);
			}
			if (controlLedColors.size() > 0) {
				JsonArray colorArr = ledObj["controlColors"].to<JsonArray>();
				for (uint8_t controlLed = 0; controlLed < controlLedColors.size(); controlLed++) {
					colorArr.add(controlLedColors[controlLed]);
				}
			}
			// get control led functions from NVS (defaults to Static for every slot)
			std::vector<uint8_t> controlLedFunctions = CONTROL_LEDS_FUNCTIONS;
			controlLedFunctions.resize(numControlLeds, 0);
			size_t funcSize = gPrefsSettings.getBytesLength("controlFuncs");
			if (funcSize == (numControlLeds * sizeof(uint8_t))) {
				gPrefsSettings.getBytes("controlFuncs", controlLedFunctions.data(), funcSize);
			}
			JsonArray funcArr = ledObj["controlFuncs"].to<JsonArray>();
			for (uint8_t controlLed = 0; controlLed < controlLedFunctions.size(); controlLed++) {
				funcArr.add(controlLedFunctions[controlLed]);
			}
		}
		ledObj["numIdleDots"].set(gPrefsSettings.getUChar("numIdleDots", NUM_LEDS_IDLE_DOTS));
		ledObj["idleColor"].set(gPrefsSettings.getUInt("idleColor", IDLE_COLOR));
		ledObj["idleAnimation"].set(gPrefsSettings.getUChar("idleAnim", 0));
		ledObj["offsetPause"].set(gPrefsSettings.getBool("offsetPause", OFFSET_PAUSE_LEDS));
		ledObj["progColorStart"].set(gPrefsSettings.getUInt("progColorStart", PROGRESS_COLOR_START));
		ledObj["progColorEnd"].set(gPrefsSettings.getUInt("progColorEnd", PROGRESS_COLOR_END));
		ledObj["hueAtmo"].set(gPrefsSettings.getShort("hueAtmo", ATMO_HUE));
		ledObj["satAtmo"].set(gPrefsSettings.getShort("satAtmo", ATMO_SATURATION));
		ledObj["dimStates"].set(gPrefsSettings.getUChar("dimStates", DIMMABLE_STATES));
		ledObj["reverseRot"].set(gPrefsSettings.getBool("ledReverseRot", false));
		ledObj["offsetStart"].set(gPrefsSettings.getUChar("ledOffset", 0));
	}
#endif
#ifdef OLED_ENABLE
	if ((section == "") || (section == "oled")) {
		// OLED display settings
		JsonObject oledObj = obj["oled"].to<JsonObject>();
		oledObj["enable"].set(gPrefsSettings.getBool("oledEnable", true));
		oledObj["startAnim"].set(gPrefsSettings.getUChar("oledStartAnim", 3)); // 0=none 1=boot 2=login 3=full
		oledObj["showBattery"].set(gPrefsSettings.getBool("oledShowBatt", true));
		oledObj["showTime"].set(gPrefsSettings.getBool("oledShowTime", true));
		oledObj["showWifi"].set(gPrefsSettings.getBool("oledShowWifi", true));
		oledObj["showVolume"].set(gPrefsSettings.getBool("oledShowVol", true));
		oledObj["flip"].set(gPrefsSettings.getBool("oledFlip", false));
		oledObj["idleLine1"].set(gPrefsSettings.getString("oledIdleL1", "LEO INDUSTRIES"));
		oledObj["idleLine2"].set(gPrefsSettings.getString("oledIdleL2", "AUDIO TERMINAL AT-1"));
	}
#endif
	if ((section == "") || (section == "buttons")) {
		// button settings
		JsonObject buttonsObj = obj["buttons"].to<JsonObject>();
		buttonsObj["short0"].set(gPrefsSettings.getUChar("btnShort0", BUTTON_0_SHORT));
		buttonsObj["short1"].set(gPrefsSettings.getUChar("btnShort1", BUTTON_1_SHORT));
		buttonsObj["short2"].set(gPrefsSettings.getUChar("btnShort2", BUTTON_2_SHORT));
		buttonsObj["short3"].set(gPrefsSettings.getUChar("btnShort3", BUTTON_3_SHORT));
		buttonsObj["short4"].set(gPrefsSettings.getUChar("btnShort4", BUTTON_4_SHORT));
		buttonsObj["short5"].set(gPrefsSettings.getUChar("btnShort5", BUTTON_5_SHORT));
		buttonsObj["short6"].set(gPrefsSettings.getUChar("btnShort6", BUTTON_6_SHORT));
		buttonsObj["long0"].set(gPrefsSettings.getUChar("btnLong0", BUTTON_0_LONG));
		buttonsObj["long1"].set(gPrefsSettings.getUChar("btnLong1", BUTTON_1_LONG));
		buttonsObj["long2"].set(gPrefsSettings.getUChar("btnLong2", BUTTON_2_LONG));
		buttonsObj["long3"].set(gPrefsSettings.getUChar("btnLong3", BUTTON_3_LONG));
		buttonsObj["long4"].set(gPrefsSettings.getUChar("btnLong4", BUTTON_4_LONG));
		buttonsObj["long5"].set(gPrefsSettings.getUChar("btnLong5", BUTTON_5_LONG));
		buttonsObj["long6"].set(gPrefsSettings.getUChar("btnLong6", BUTTON_6_LONG));
		buttonsObj["multi01"].set(gPrefsSettings.getUChar("btnMulti01", BUTTON_MULTI_01));
		buttonsObj["multi02"].set(gPrefsSettings.getUChar("btnMulti02", BUTTON_MULTI_02));
		buttonsObj["multi03"].set(gPrefsSettings.getUChar("btnMulti03", BUTTON_MULTI_03));
		buttonsObj["multi04"].set(gPrefsSettings.getUChar("btnMulti04", BUTTON_MULTI_04));
		buttonsObj["multi05"].set(gPrefsSettings.getUChar("btnMulti05", BUTTON_MULTI_05));
		buttonsObj["multi06"].set(gPrefsSettings.getUChar("btnMulti06", BUTTON_MULTI_06));
		buttonsObj["multi12"].set(gPrefsSettings.getUChar("btnMulti12", BUTTON_MULTI_12));
		buttonsObj["multi13"].set(gPrefsSettings.getUChar("btnMulti13", BUTTON_MULTI_13));
		buttonsObj["multi14"].set(gPrefsSettings.getUChar("btnMulti14", BUTTON_MULTI_14));
		buttonsObj["multi15"].set(gPrefsSettings.getUChar("btnMulti15", BUTTON_MULTI_15));
		buttonsObj["multi16"].set(gPrefsSettings.getUChar("btnMulti16", BUTTON_MULTI_16));
		buttonsObj["multi23"].set(gPrefsSettings.getUChar("btnMulti23", BUTTON_MULTI_23));
		buttonsObj["multi24"].set(gPrefsSettings.getUChar("btnMulti24", BUTTON_MULTI_24));
		buttonsObj["multi25"].set(gPrefsSettings.getUChar("btnMulti25", BUTTON_MULTI_25));
		buttonsObj["multi26"].set(gPrefsSettings.getUChar("btnMulti26", BUTTON_MULTI_26));
		buttonsObj["multi34"].set(gPrefsSettings.getUChar("btnMulti34", BUTTON_MULTI_34));
		buttonsObj["multi35"].set(gPrefsSettings.getUChar("btnMulti35", BUTTON_MULTI_35));
		buttonsObj["multi36"].set(gPrefsSettings.getUChar("btnMulti36", BUTTON_MULTI_36));
		buttonsObj["multi45"].set(gPrefsSettings.getUChar("btnMulti45", BUTTON_MULTI_45));
		buttonsObj["multi46"].set(gPrefsSettings.getUChar("btnMulti46", BUTTON_MULTI_46));
		buttonsObj["multi56"].set(gPrefsSettings.getUChar("btnMulti56", BUTTON_MULTI_56));
	}
	if ((section == "") || (section == "rotary")) {
		// Rotary encoder
		JsonObject rotaryObj = obj["rotary"].to<JsonObject>();
		rotaryObj["reverse"].set(gPrefsSettings.getBool("rotaryReverse", false));
	}
	// playlist
	if ((section == "") || (section == "playlist")) {
		JsonObject playlistObj = obj["playlist"].to<JsonObject>();
		playlistObj["sortMode"] = EnumUtils::underlying_value(AudioPlayer_GetPlaylistSortMode());
		playlistObj["recDepth"] = SdCard_GetMaxRecursionDepth();
	}
#ifdef BATTERY_MEASURE_ENABLE
	if ((section == "") || (section == "battery")) {
		// battery settings
		JsonObject batteryObj = obj["battery"].to<JsonObject>();
	#ifdef MEASURE_BATTERY_VOLTAGE
		batteryObj["warnLowVoltage"].set(gPrefsSettings.getFloat("wLowVoltage", s_warningLowVoltage));
		batteryObj["indicatorLow"].set(gPrefsSettings.getFloat("vIndicatorLow", s_voltageIndicatorLow));
		batteryObj["indicatorHi"].set(gPrefsSettings.getFloat("vIndicatorHigh", s_voltageIndicatorHigh));
		#ifdef SHUTDOWN_ON_BAT_CRITICAL
		batteryObj["criticalVoltage"].set(gPrefsSettings.getFloat("wCritVoltage", s_warningCriticalVoltage));
		#endif
	#endif

		batteryObj["voltageCheckInterval"].set(gPrefsSettings.getUInt("vCheckIntv", s_batteryCheckInterval));
	}
#endif
	if (section == "defaults") {
		// default factory settings NOTE: maintain the settings section structure as above to make it easier for clients to use
		JsonObject defaultsObj = obj["defaults"].to<JsonObject>();
		JsonObject genSettings = defaultsObj["general"].to<JsonObject>();
		genSettings["initVolume"].set(AUDIOPLAYER_VOLUME_INIT);
		genSettings["maxVolumeSp"].set(AUDIOPLAYER_VOLUME_MAX);
		genSettings["maxVolumeHp"].set(18u); // gPrefsSettings.getUInt("maxVolumeHp", 0));
		genSettings["sleepInactivity"].set(10u); // System_MaxInactivityTime
		genSettings["playMono"].set(false); // PLAY_MONO_SPEAKER
		genSettings["savePosShutdown"].set(false); // SAVE_PLAYPOS_BEFORE_SHUTDOWN
		genSettings["savePosRfidChge"].set(false); // SAVE_PLAYPOS_WHEN_RFID_CHANGE
		genSettings["savePosPeriodic"].set(true); // periodic audiobook play-position checkpoint
		genSettings["restartFreshHrs"].set(24u); // restart audiobooks from start after 24 h idle (0 = off)
		genSettings["minResumeSec"].set(20u); // don't resume an audiobook pulled within the first 20 s (0 = off)
		genSettings["seekStep"].set((uint32_t) seekStepDefault); // step (s) for smart forward/backward in-file seeking
		genSettings["playStartupSnd"].set(true); // play the ready sound on cold start
		genSettings["playLastRfidOnReboot"].set(false); // PLAY_LAST_RFID_AFTER_REBOOT
		genSettings["pauseIfRfidRemoved"].set(false); // PAUSE_WHEN_RFID_REMOVED
		genSettings["stopIfRfidRemoved"].set(false); // stop instead of pause when RFID is removed
		genSettings["dontAcceptRfidTwice"].set(false); // DONT_ACCEPT_SAME_RFID_TWICE
		genSettings["pauseOnMinVol"].set(false); // PAUSE_ON_MIN_VOLUME
		genSettings["recoverVolBoot"].set(false); // USE_LAST_VOLUME_AFTER_REBOOT
		genSettings["volumeCurve"].set(0u); // VOLUME_CURVE
		genSettings["rfidReaderType"].set(0u); // RFID_READER_TYPE_RUNTIME (auto-detect)
		genSettings["pn5180Lpcd"].set(false); // PN5180 LPCD disabled
		genSettings["mfrc522Gain"].set(7u); // MFRC522_GAIN default (max gain)
		JsonObject eqSettings = defaultsObj["equalizer"].to<JsonObject>();
		eqSettings["gainHighPass"].set(0);
		eqSettings["gainBandPass"].set(0);
		eqSettings["gainLowPass"].set(0);
		eqSettings["profile"].set("flat");
#ifdef NEOPIXEL_ENABLE
		JsonObject ledSettings = defaultsObj["led"].to<JsonObject>();
		ledSettings["initBrightness"].set(16u); // LED_INITIAL_BRIGHTNESS
		ledSettings["nightBrightness"].set(2u); // LED_INITIAL_NIGHT_BRIGHTNESS
		ledSettings["atmoBrightness"].set(30u); // LED_INITIAL_NIGHT_BRIGHTNESS
		ledSettings["numIndicator"].set(NUM_INDICATOR_LEDS); // NUM_INDICATOR_LEDS
		ledSettings["numControl"].set(NUM_CONTROL_LEDS); // NUM_CONTROL_LEDS
		ledSettings["controlLedsEnabled"].set(true); // control LEDs enabled by default
		ledSettings["numIdleDots"].set(NUM_LEDS_IDLE_DOTS); // NUM_LEDS_IDLE_DOTS
		ledSettings["idleColor"].set(IDLE_COLOR); // IDLE_COLOR
		ledSettings["idleAnimation"].set(0u); // standard idle animation
		ledSettings["offsetPause"].set(OFFSET_PAUSE_LEDS); // OFFSET_PAUSE_LEDS
		ledSettings["progColorStart"].set(PROGRESS_COLOR_START); // PROGRESS_COLOR_START
		ledSettings["progColorEnd"].set(PROGRESS_COLOR_END); // PROGRESS_COLOR_END
		ledSettings["hueAtmo"].set(ATMO_HUE);
		ledSettings["satAtmo"].set(ATMO_SATURATION);
		ledSettings["dimStates"].set(DIMMABLE_STATES); // DIMMABLE_STATES
	#ifdef NEOPIXEL_REVERSE_ROTATION
		ledSettings["reverseRot"].set(true);
	#else
		ledSettings["reverseRot"].set(false);
	#endif
	#ifdef LED_OFFSET
		ledSettings["offsetStart"].set(LED_OFFSET);
	#else
		ledSettings["offsetStart"].set(0);
	#endif
		JsonArray colorArr = ledSettings["controlColors"].to<JsonArray>();
		std::vector<uint32_t> controlLedColors = CONTROL_LEDS_COLORS;
		for (uint8_t controlLed = 0; controlLed < controlLedColors.size(); controlLed++) {
			colorArr.add(controlLedColors[controlLed]);
		}
#endif
#ifdef OLED_ENABLE
		JsonObject oledSettings = defaultsObj["oled"].to<JsonObject>();
		oledSettings["enable"].set(true);
		oledSettings["startAnim"].set(3u); // full boot + login animation
		oledSettings["showBattery"].set(true);
		oledSettings["showTime"].set(true);
		oledSettings["showWifi"].set(true);
		oledSettings["showVolume"].set(true);
		oledSettings["flip"].set(false);
		oledSettings["idleLine1"].set("LEO INDUSTRIES");
		oledSettings["idleLine2"].set("AUDIO TERMINAL AT-1");
#endif
		JsonObject buttonsSettings = defaultsObj["buttons"].to<JsonObject>();
		buttonsSettings["short0"].set(BUTTON_0_SHORT);
		buttonsSettings["short1"].set(BUTTON_1_SHORT);
		buttonsSettings["short2"].set(BUTTON_2_SHORT);
		buttonsSettings["short3"].set(BUTTON_3_SHORT);
		buttonsSettings["short4"].set(BUTTON_4_SHORT);
		buttonsSettings["short5"].set(BUTTON_5_SHORT);
		buttonsSettings["short6"].set(BUTTON_6_SHORT);
		buttonsSettings["long0"].set(BUTTON_0_LONG);
		buttonsSettings["long1"].set(BUTTON_1_LONG);
		buttonsSettings["long2"].set(BUTTON_2_LONG);
		buttonsSettings["long3"].set(BUTTON_3_LONG);
		buttonsSettings["long4"].set(BUTTON_4_LONG);
		buttonsSettings["long5"].set(BUTTON_5_LONG);
		buttonsSettings["long6"].set(BUTTON_6_LONG);
		buttonsSettings["multi01"].set(BUTTON_MULTI_01);
		buttonsSettings["multi02"].set(BUTTON_MULTI_02);
		buttonsSettings["multi03"].set(BUTTON_MULTI_03);
		buttonsSettings["multi04"].set(BUTTON_MULTI_04);
		buttonsSettings["multi05"].set(BUTTON_MULTI_05);
		buttonsSettings["multi06"].set(BUTTON_MULTI_06);
		buttonsSettings["multi12"].set(BUTTON_MULTI_12);
		buttonsSettings["multi13"].set(BUTTON_MULTI_13);
		buttonsSettings["multi14"].set(BUTTON_MULTI_14);
		buttonsSettings["multi15"].set(BUTTON_MULTI_15);
		buttonsSettings["multi16"].set(BUTTON_MULTI_16);
		buttonsSettings["multi23"].set(BUTTON_MULTI_23);
		buttonsSettings["multi24"].set(BUTTON_MULTI_24);
		buttonsSettings["multi25"].set(BUTTON_MULTI_25);
		buttonsSettings["multi26"].set(BUTTON_MULTI_26);
		buttonsSettings["multi34"].set(BUTTON_MULTI_34);
		buttonsSettings["multi35"].set(BUTTON_MULTI_35);
		buttonsSettings["multi36"].set(BUTTON_MULTI_36);
		buttonsSettings["multi45"].set(BUTTON_MULTI_45);
		buttonsSettings["multi46"].set(BUTTON_MULTI_46);
		buttonsSettings["multi56"].set(BUTTON_MULTI_56);
#ifdef USEROTARY_ENABLE
		JsonObject rotarySettings = defaultsObj["rotary"].to<JsonObject>();
		rotarySettings["reverse"].set(false); // REVERSE_ROTARY
#endif
		JsonObject playlistSettings = defaultsObj["playlist"].to<JsonObject>();
		playlistSettings["sortMode"].set(EnumUtils::underlying_value(AUDIOPLAYER_PLAYLIST_SORT_MODE_DEFAULT));
		playlistSettings["recDepth"].set(2u);
#ifdef BATTERY_MEASURE_ENABLE
		JsonObject batSettings = defaultsObj["battery"].to<JsonObject>();
	#ifdef MEASURE_BATTERY_VOLTAGE
		batSettings["warnLowVoltage"].set(s_warningLowVoltage);
		batSettings["indicatorLow"].set(s_voltageIndicatorLow);
		batSettings["indicatorHi"].set(s_voltageIndicatorHigh);
		#ifdef SHUTDOWN_ON_BAT_CRITICAL
		batSettings["criticalVoltage"].set(s_warningCriticalVoltage);
		#endif
	#endif
		batSettings["voltageCheckInterval"].set(s_batteryCheckInterval);
#endif
	}
// FTP
#ifdef FTP_ENABLE
	if ((section == "") || (section == "ftp")) {
		JsonObject ftpObj = obj["ftp"].to<JsonObject>();
		ftpObj["username"] = gPrefsSettings.getString("ftpuser", "-1");
		ftpObj["password"] = gPrefsSettings.getString("ftppassword", "-1");
		ftpObj["maxUserLength"].set(ftpUserLength - 1);
		ftpObj["maxPwdLength"].set(ftpUserLength - 1);
		ftpObj["enable"] = gPrefsSettings.getBool("ftpEnable", false);
	}
#endif
#ifdef WEBDAV_ENABLE
	if ((section == "") || (section == "webdav")) {
		JsonObject wdObj = obj["webdav"].to<JsonObject>();
		wdObj["username"] = gPrefsSettings.getString("webdavUser", "esp32");
		wdObj["password"] = gPrefsSettings.getString("webdavPwd", "esp32");
		wdObj["enable"] = gPrefsSettings.getBool("webdavEnable", false);
		wdObj["running"] = Webdav_IsServerRunning();
		wdObj["port"].set(webdavPort);
		wdObj["maxUserLength"].set(webdavUserLength - 1);
		wdObj["maxPwdLength"].set(webdavPasswordLength - 1);
	}
#endif
	// HTTP file sync
	if ((section == "") || (section == "sync")) {
		JsonObject syncObj = obj["sync"].to<JsonObject>();
		syncObj["url"] = gPrefsSettings.getString("syncUrl", "");
		syncObj["username"] = gPrefsSettings.getString("syncUser", "");
		syncObj["password"] = gPrefsSettings.getString("syncPwd", "");
		syncObj["abortOnButton"] = gPrefsSettings.getBool("syncAbortBtn", true);
		syncObj["deleteRemoved"] = gPrefsSettings.getBool("syncDelete", false);
		syncObj["rfidUrl"] = gPrefsSettings.getString("rfidSyncUrl", "");
		syncObj["rfidPeers"] = gPrefsSettings.getString("rfidPeers", "");
		syncObj["rfidPeerKey"] = gPrefsSettings.getString("rfidPeerKey", "");
		syncObj["rfidLearn"] = gPrefsSettings.getBool("rfidSyncLearn", true);
		syncObj["backupUrl"] = gPrefsSettings.getString("backupUrl", "");
		syncObj["backupAuto"] = gPrefsSettings.getBool("backupAuto", false);
	}
// MQTT
#ifdef MQTT_ENABLE
	if ((section == "") || (section == "mqtt")) {
		JsonObject mqttObj = obj["mqtt"].to<JsonObject>();
		mqttObj["enable"].set(Mqtt_IsEnabled());
		String macPlain = Wlan_GetMacAddress(); // returns AA:BB:CC:DD:EE:FF or empty
		macPlain.replace(":", "");
		macPlain.toUpperCase();
		mqttObj["macAddressPlain"] = macPlain;
		mqttObj["clientID"] = gPrefsSettings.getString("mqttClientId", "-1");
		mqttObj["deviceId"] = gPrefsSettings.getString("mqttDeviceId", "-1");
		mqttObj["baseTopic"] = gPrefsSettings.getString("mqttBaseTopic", "-1");
		mqttObj["server"] = gPrefsSettings.getString("mqttServer", "-1");
		mqttObj["port"].set(gPrefsSettings.getUInt("mqttPort", 0));
		mqttObj["username"] = gPrefsSettings.getString("mqttUser", "-1");
		mqttObj["password"] = gPrefsSettings.getString("mqttPassword", "-1");
		mqttObj["maxUserLength"].set(mqttUserLength - 1);
		mqttObj["maxPwdLength"].set(mqttPasswordLength - 1);
		mqttObj["maxClientIdLength"].set(mqttClientIdLength - 1);
		mqttObj["maxServerLength"].set(mqttServerLength - 1);
		mqttObj["maxBaseTopicLength"].set(mqttBaseTopicLength - 1);
		mqttObj["maxDeviceIdLength"].set(mqttDeviceIdLength - 1);
	}
#endif
// Bluetooth
#ifdef BLUETOOTH_ENABLE
	if ((section == "") || (section == "bluetooth")) {
		JsonObject btObj = obj["bluetooth"].to<JsonObject>();
		if (gPrefsSettings.isKey("btDeviceName")) {
			btObj["deviceName"] = gPrefsSettings.getString("btDeviceName", "");
		} else {
			btObj["deviceName"] = "";
		}
		if (gPrefsSettings.isKey("btPinCode")) {
			btObj["pinCode"] = gPrefsSettings.getString("btPinCode", "");
		} else {
			btObj["pinCode"] = "";
		}
	}
#endif
#ifdef IR_CONTROL_ENABLE
	// IR remote control: presence of this section reveals the web-UI tab; map = learned code->command list
	if ((section == "") || (section == "ir")) {
		JsonObject irObj = obj["ir"].to<JsonObject>();
		irObj["enabled"] = true;
		JsonArray mapArr = irObj["map"].to<JsonArray>();
		IrMapping mappings[IR_MAX_MAPPINGS];
		uint8_t count = IrReceiver_GetMappings(mappings, IR_MAX_MAPPINGS);
		for (uint8_t i = 0; i < count; i++) {
			JsonObject e = mapArr.add<JsonObject>();
			e["code"] = mappings[i].code;
			e["cmd"] = mappings[i].cmd;
		}
	}
#endif
}

// handle get settings
void handleGetSettings(AsyncWebServerRequest *request) {

	// param to get a single settings section
	String section = "";
	if (request->hasParam("section")) {
		section = request->getParam("section")->value();
	}

	AsyncJsonResponse *response = new AsyncJsonResponse(false);
	JsonObject settingsObj = response->getRoot();
	settingsToJSON(settingsObj, section);
	if (response->overflowed()) {
		// JSON buffer too small for data
		Log_Println(jsonbufferOverflow, LOGLEVEL_ERROR);
		request->send(500);
		return;
	}
	response->setLength();
	request->send(response);
}

// NVS caps a single string value at 4000 bytes; stay safely below that so a near-full
// rule set still leaves room for the JSON envelope and a final entry.
static constexpr size_t EQ_RULES_MAX_BYTES = 3900;

// Return all per-path equalizer rules as the stored JSON array.
void handleGetEqRules(AsyncWebServerRequest *request) {
	request->send(200, "application/json", gPrefsSettings.getString("eqRules", "[]"));
}

// Add or update a per-path equalizer rule (path + gains + profile name). Stored as a
// JSON array in NVS under "eqRules" and re-loaded into the audio player.
void handleSetEqRule(AsyncWebServerRequest *request) {
	if (!request->hasParam("path")) {
		request->send(400, "application/json", "{}");
		return;
	}
	const String path = request->getParam("path")->value();
	const int8_t low = request->hasParam("low") ? (int8_t) request->getParam("low")->value().toInt() : 0;
	const int8_t band = request->hasParam("band") ? (int8_t) request->getParam("band")->value().toInt() : 0;
	const int8_t high = request->hasParam("high") ? (int8_t) request->getParam("high")->value().toInt() : 0;
	const String profile = request->hasParam("profile") ? request->getParam("profile")->value() : String("");

	JsonDocument doc;
	deserializeJson(doc, gPrefsSettings.getString("eqRules", "[]"));
	if (!doc.is<JsonArray>()) {
		doc.to<JsonArray>();
	}
	JsonArray arr = doc.as<JsonArray>();
	for (size_t i = 0; i < arr.size();) {
		if (arr[i]["p"].as<String>() == path) {
			arr.remove(i);
		} else {
			i++;
		}
	}
	JsonObject o = arr.add<JsonObject>();
	o["p"] = path;
	o["l"] = low;
	o["b"] = band;
	o["h"] = high;
	o["pr"] = profile;

	String out;
	serializeJson(doc, out);
	// NVS caps a single string value at 4000 bytes. If the serialized rule set would
	// exceed that, putString() silently fails (returns 0) and the rule is lost without
	// any feedback. Reject the save up-front with a clear error instead, leaving the
	// previously stored rules untouched, and tell the UI how full the store is.
	if (out.length() > EQ_RULES_MAX_BYTES) {
		char err[160];
		snprintf(err, sizeof(err), "{\"error\":\"eqRulesFull\",\"used\":%u,\"max\":%u}", (unsigned) out.length(), (unsigned) EQ_RULES_MAX_BYTES);
		request->send(413, "application/json", err);
		return;
	}
	if (gPrefsSettings.putString("eqRules", out) == 0) {
		request->send(500, "application/json", "{\"error\":\"eqRulesSaveFailed\"}");
		return;
	}
	AudioPlayer_ReloadEqRules();
	request->send(200, "application/json", "{}");
}

// Delete the per-path equalizer rule for the given path.
void handleDeleteEqRule(AsyncWebServerRequest *request) {
	if (!request->hasParam("path")) {
		request->send(400, "application/json", "{}");
		return;
	}
	const String path = request->getParam("path")->value();

	JsonDocument doc;
	deserializeJson(doc, gPrefsSettings.getString("eqRules", "[]"));
	JsonArray arr = doc.as<JsonArray>();
	for (size_t i = 0; i < arr.size();) {
		if (arr[i]["p"].as<String>() == path) {
			arr.remove(i);
		} else {
			i++;
		}
	}
	String out;
	serializeJson(doc, out);
	gPrefsSettings.putString("eqRules", out);
	AudioPlayer_ReloadEqRules();
	request->send(200, "application/json", "{}");
}

// handle get operation mode
void handleGetOperationMode(AsyncWebServerRequest *request) {
	AsyncJsonResponse *response = new AsyncJsonResponse(false);
	JsonObject object = response->getRoot();
	object["mode"] = System_GetOperationMode();
	response->setLength();
	request->send(response);
}

// handle post operation mode
void handlePostOperationMode(AsyncWebServerRequest *request, JsonVariant &json) {
	const JsonObject &jsonObj = json.as<JsonObject>();
	if (jsonObj["mode"].is<uint8_t>()) {
		uint8_t mode = jsonObj["mode"].as<uint8_t>();
		System_SetOperationMode(mode);
		request->send(200);
	} else {
		request->send(400, "text/plain; charset=utf-8", "missing 'mode' parameter");
	}
}
