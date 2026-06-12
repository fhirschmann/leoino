#include <Arduino.h>
#include "settings.h"

#include "Display.h"

#include "AudioPlayer.h"
#include "Battery.h"
#include "Log.h"
#include "System.h"
#include "Wlan.h"

#ifdef DISPLAY_ENABLE

	#include <U8g2lib.h>
	#include <Wire.h>

// The external I2C bus is created and begun in main.cpp; the display shares it
// instead of touching the global Wire1 to avoid two drivers on one peripheral.
extern TwoWire i2cBusTwo;

// Default SH1106 / SSD1306 address (8-bit form for u8x8)
	#ifndef DISPLAY_I2C_ADDRESS
		#define DISPLAY_I2C_ADDRESS 0x3C
	#endif

	#define DISPLAY_REFRESH_INTERVAL_MS 400u // how often the display is redrawn at most
	#define DISPLAY_SCROLL_STEP_PX		5u	 // horizontal scroll speed for long titles

// I2C clock for the display. 100 kHz is the safe default and matches the rest of
// the system; externally wired OLEDs (long Dupont cables, extra bus capacitance)
// often fail at 400 kHz. Override in settings-*.h if your wiring is short.
	#ifndef DISPLAY_I2C_CLOCK
		#define DISPLAY_I2C_CLOCK 100000UL
	#endif

// ----- U8g2 byte transport routed through ESPuino's shared i2cBusTwo -----
// Streams straight into i2cBusTwo (like the canonical Arduino HW-I2C callback,
// but on the shared bus instead of the global Wire) so large page writes are
// not truncated. Counts NACKs so a wrong address / wiring is visible in the log.
static volatile uint8_t displayI2cErrors = 0;
static uint8_t Display_I2CByteCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
	switch (msg) {
		case U8X8_MSG_BYTE_SEND:
			i2cBusTwo.write((uint8_t *) arg_ptr, arg_int);
			break;
		case U8X8_MSG_BYTE_INIT:
			// bus already initialized in main.cpp (i2cBusTwo.begin)
			break;
		case U8X8_MSG_BYTE_SET_DC:
			break;
		case U8X8_MSG_BYTE_START_TRANSFER:
			i2cBusTwo.beginTransmission(u8x8_GetI2CAddress(u8x8) >> 1);
			break;
		case U8X8_MSG_BYTE_END_TRANSFER:
			if (i2cBusTwo.endTransmission() != 0) {
				if (displayI2cErrors < 255) {
					displayI2cErrors++;
				}
			}
			break;
		default:
			return 0;
	}
	return 1;
}

// Probe an I2C device on the shared bus (true if it ACKs)
static bool Display_ProbeAddress(uint8_t addr7) {
	i2cBusTwo.beginTransmission(addr7);
	return i2cBusTwo.endTransmission() == 0;
}

static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

static bool displayEnabled = false;
static bool displayInitialized = false;
static bool displayPoweredOff = false;
static uint32_t lastRefresh = 0;
static uint16_t scrollOffset = 0;

// remember last rendered state to skip redundant redraws (saves I2C bandwidth)
static char lastTitle[255] = {0};
static bool lastPausePlay = false;
static uint8_t lastVolume = 255;
static int lastProgress = -1;
static uint8_t lastOpMode = 255;

static bool Display_StateChanged(void) {
	if (strncmp(lastTitle, gPlayProperties.title, sizeof(lastTitle)) != 0) {
		return true;
	}
	if (lastPausePlay != gPlayProperties.pausePlay) {
		return true;
	}
	if (lastVolume != AudioPlayer_GetCurrentVolume()) {
		return true;
	}
	if (lastProgress != (int) gPlayProperties.currentRelPos) {
		return true;
	}
	if (lastOpMode != System_GetOperationMode()) {
		return true;
	}
	return false;
}

static void Display_DrawTopBar(void) {
	u8g2.setFont(u8g2_font_5x7_tf);
	const uint8_t opmode = System_GetOperationMode();
	// left: connection / mode
	char left[20];
	if (opmode == OPMODE_BLUETOOTH_SINK) {
		snprintf(left, sizeof(left), "BT SPEAKER");
	} else if (opmode == OPMODE_BLUETOOTH_SOURCE) {
		snprintf(left, sizeof(left), "BT HEADPHONE");
	} else if (Wlan_IsConnected()) {
		snprintf(left, sizeof(left), "WiFi %ddBm", Wlan_GetRssi());
	} else {
		snprintf(left, sizeof(left), "NO WiFi");
	}
	u8g2.drawStr(0, 6, left);

	// right: battery percent if available
	#ifdef BATTERY_MEASURE_ENABLE
	char right[8];
	int soc = (int) (Battery_EstimateLevel() * 100);
	if (soc < 0) {
		soc = 0;
	}
	if (soc > 100) {
		soc = 100;
	}
	snprintf(right, sizeof(right), "%d%%", soc);
	const uint8_t rw = u8g2.getStrWidth(right);
	// little battery glyph
	u8g2.drawFrame(128 - rw - 16, 0, 12, 7);
	u8g2.drawBox(128 - rw - 4, 2, 2, 3);
	const uint8_t fill = (uint8_t) ((soc * 10) / 100);
	if (fill > 0) {
		u8g2.drawBox(128 - rw - 15, 1, fill, 5);
	}
	u8g2.drawStr(128 - rw, 6, right);
	#endif

	u8g2.drawHLine(0, 9, 128);
}

static void Display_DrawIdle(void) {
	u8g2.setFont(u8g2_font_7x13B_tf);
	const char *brand = "LEO INDUSTRIES";
	u8g2.drawStr((128 - u8g2.getStrWidth(brand)) / 2, 30, brand);
	u8g2.setFont(u8g2_font_6x12_tf);
	const char *model = "AT-1";
	u8g2.drawStr((128 - u8g2.getStrWidth(model)) / 2, 44, model);

	u8g2.setFont(u8g2_font_5x7_tf);
	if (Wlan_IsConnected()) {
		String ip = Wlan_GetIpAddress();
		u8g2.drawStr((128 - u8g2.getStrWidth(ip.c_str())) / 2, 60, ip.c_str());
	} else {
		const char *hint = "waiting...";
		u8g2.drawStr((128 - u8g2.getStrWidth(hint)) / 2, 60, hint);
	}
}

static void Display_DrawPlayIcon(uint8_t x, uint8_t y, bool playing) {
	// playing == pausePlay flag false -> show play triangle, otherwise pause bars
	if (playing) {
		u8g2.drawBox(x, y, 2, 8);
		u8g2.drawBox(x + 4, y, 2, 8);
	} else {
		for (uint8_t i = 0; i < 8; i++) {
			u8g2.drawHLine(x, y + i, (i < 4) ? (i + 1) : (8 - i));
		}
	}
}

static void Display_DrawTrack(void) {
	// title with horizontal scroll when wider than the display
	u8g2.setFont(u8g2_font_6x12_tf);
	const uint16_t titleWidth = u8g2.getStrWidth(gPlayProperties.title);
	const uint8_t titleY = 26;
	if (titleWidth <= 128) {
		u8g2.drawStr((128 - titleWidth) / 2, titleY, gPlayProperties.title);
		scrollOffset = 0;
	} else {
		// scroll: draw twice with a gap for a seamless wrap-around
		const uint16_t gap = 24;
		const uint16_t span = titleWidth + gap;
		int x = -(int) (scrollOffset % span);
		u8g2.drawStr(x, titleY, gPlayProperties.title);
		u8g2.drawStr(x + span, titleY, gPlayProperties.title);
		scrollOffset += DISPLAY_SCROLL_STEP_PX;
	}

	// track counter
	u8g2.setFont(u8g2_font_5x7_tf);
	const uint16_t numTracks = gPlayProperties.playlist ? gPlayProperties.playlist->size() : 0;
	if (numTracks > 0) {
		char counter[16];
		snprintf(counter, sizeof(counter), "%u / %u", gPlayProperties.currentTrackNumber + 1, numTracks);
		u8g2.drawStr((128 - u8g2.getStrWidth(counter)) / 2, 38, counter);
	}

	// play/pause icon + progress bar
	Display_DrawPlayIcon(0, 44, !gPlayProperties.pausePlay);
	const uint8_t barX = 10;
	const uint8_t barW = 128 - barX;
	u8g2.drawFrame(barX, 45, barW, 6);
	int progress = (int) gPlayProperties.currentRelPos;
	if (progress < 0) {
		progress = 0;
	}
	if (progress > 100) {
		progress = 100;
	}
	u8g2.drawBox(barX + 1, 46, (uint8_t) ((barW - 2) * progress / 100), 4);

	// bottom: elapsed/duration left, volume right
	u8g2.setFont(u8g2_font_5x7_tf);
	const uint32_t cur = AudioPlayer_GetCurrentTime();
	const uint32_t dur = AudioPlayer_GetFileDuration();
	char timeStr[16];
	if (dur > 0) {
		snprintf(timeStr, sizeof(timeStr), "%lu:%02lu/%lu:%02lu", cur / 60, cur % 60, dur / 60, dur % 60);
	} else {
		snprintf(timeStr, sizeof(timeStr), "%lu:%02lu", cur / 60, cur % 60);
	}
	u8g2.drawStr(0, 63, timeStr);

	char volStr[10];
	snprintf(volStr, sizeof(volStr), "VOL %u", AudioPlayer_GetCurrentVolume());
	u8g2.drawStr(128 - u8g2.getStrWidth(volStr), 63, volStr);
}

static void Display_Render(void) {
	u8g2.clearBuffer();
	Display_DrawTopBar();
	if (gPlayProperties.playMode == NO_PLAYLIST || gPlayProperties.title[0] == '\0') {
		Display_DrawIdle();
	} else {
		Display_DrawTrack();
	}
	u8g2.sendBuffer();

	strncpy(lastTitle, gPlayProperties.title, sizeof(lastTitle) - 1);
	lastTitle[sizeof(lastTitle) - 1] = '\0';
	lastPausePlay = gPlayProperties.pausePlay;
	lastVolume = AudioPlayer_GetCurrentVolume();
	lastProgress = (int) gPlayProperties.currentRelPos;
	lastOpMode = System_GetOperationMode();
}

void Display_Init(void) {
	displayEnabled = gPrefsSettings.getBool("displayEnable", true);
	if (!displayEnabled) {
		Log_Println("Display disabled in settings", LOGLEVEL_NOTICE);
		return;
	}
	// conservative bus clock - externally wired OLEDs often don't ACK at 400 kHz
	i2cBusTwo.setClock(DISPLAY_I2C_CLOCK);

	// log every device found on the external bus (helps diagnose wiring/address)
	String found;
	for (uint8_t a = 0x08; a <= 0x77; a++) {
		if (Display_ProbeAddress(a)) {
			char buf[8];
			snprintf(buf, sizeof(buf), "0x%02X ", a);
			found += buf;
		}
	}
	Log_Printf(LOGLEVEL_NOTICE, "Display: I2C devices on external bus @%lukHz: %s", (unsigned long) (DISPLAY_I2C_CLOCK / 1000), found.length() ? found.c_str() : "(none)");

	// auto-detect the display address: prefer the configured one, fall back to the
	// alternate (0x3C <-> 0x3D) so both common module variants work out of the box
	uint8_t addr = DISPLAY_I2C_ADDRESS;
	const uint8_t alt = (DISPLAY_I2C_ADDRESS == 0x3C) ? 0x3D : 0x3C;
	if (!Display_ProbeAddress(addr) && Display_ProbeAddress(alt)) {
		addr = alt;
	}
	if (!Display_ProbeAddress(addr)) {
		Log_Printf(LOGLEVEL_ERROR, "Display: no OLED found at 0x%02X or 0x%02X - check wiring (SDA=%d, SCL=%d)", DISPLAY_I2C_ADDRESS, alt, ext_IIC_DATA, ext_IIC_CLK);
		// still attempt the configured address in case the probe is unreliable
	}

	u8g2.setI2CAddress(addr << 1);
	// route the U8g2 byte transport through the shared bus instead of the global Wire
	u8g2.getU8g2()->u8x8.byte_cb = Display_I2CByteCb;
	displayI2cErrors = 0;
	u8g2.begin(); // always returns 1; rely on the probe / error counter instead
	u8g2.setContrast(255);
	u8g2.setPowerSave(0);
	displayInitialized = true;
	displayPoweredOff = false;
	Display_Render();
	Log_Printf(LOGLEVEL_NOTICE, "Display initialized at 0x%02X (%u I2C errors during init)", addr, displayI2cErrors);
}

void Display_Cyclic(void) {
	if (!displayEnabled) {
		return;
	}
	if (!displayInitialized) {
		// allow a late init (e.g. after the user enabled it at runtime)
		Display_Init();
		return;
	}
	if (millis() - lastRefresh < DISPLAY_REFRESH_INTERVAL_MS) {
		return;
	}
	lastRefresh = millis();

	const bool scrolling = (gPlayProperties.playMode != NO_PLAYLIST) && (u8g2.getStrWidth(gPlayProperties.title) > 128);
	if (Display_StateChanged() || scrolling) {
		Display_Render();
	}
}

void Display_Exit(void) {
	if (displayInitialized && !displayPoweredOff) {
		u8g2.setPowerSave(1);
		displayPoweredOff = true;
	}
}

void Display_SetEnabled(bool enabled) {
	gPrefsSettings.putBool("displayEnable", enabled);
	if (enabled == displayEnabled) {
		return;
	}
	displayEnabled = enabled;
	if (enabled) {
		displayInitialized = false; // re-init on next cyclic
	} else if (displayInitialized) {
		u8g2.setPowerSave(1);
		displayPoweredOff = true;
	}
	Log_Printf(LOGLEVEL_NOTICE, "Display %s", enabled ? "enabled" : "disabled");
}

bool Display_IsEnabled(void) {
	return displayEnabled;
}

#else // DISPLAY_ENABLE not defined: provide no-op stubs

void Display_Init(void) { }
void Display_Cyclic(void) { }
void Display_Exit(void) { }
void Display_SetEnabled(bool enabled) {
	(void) enabled;
}
bool Display_IsEnabled(void) {
	return false;
}

#endif
