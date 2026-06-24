#include <Arduino.h>
#include "settings.h"

#include "Rtc.h"

#include "Log.h"
#include "Mqtt.h"
#include "System.h" // I2cBusTwo_Lock/Unlock: serialize DS3231 access against the OLED frame + RC522-I2C task
#include "Wlan.h"

#ifdef RTC_ENABLE
	#include <RTClib.h>
	#include <Wire.h>

	#include <time.h>

extern TwoWire i2cBusTwo;

static RTC_DS3231 rtc;
static bool rtcAvailable = false;

// Reject timestamps before 2024-01-01T00:00:00Z as implausible (e.g. a browser
// with a wildly wrong clock, or an unset value)
static constexpr time_t rtcMinPlausibleEpoch = 1704067200;

// How often the (NTP-disciplined) system time is written back to the RTC
static constexpr uint32_t rtcResyncInterval = 3600UL * 1000UL; // 1h
// How often the current time is published via MQTT
static constexpr uint32_t rtcPublishInterval = 60UL * 1000UL; // 1min

static uint32_t lastResyncTimestamp = 0;
static uint32_t lastPublishTimestamp = 0;

// Returns true if the system clock holds a plausible (NTP-set) time
static bool systemTimeIsValid(struct tm *timeinfo) {
	if (!getLocalTime(timeinfo, 5)) {
		return false;
	}
	return (timeinfo->tm_year + 1900) >= 2024;
}

void Rtc_Init(void) {
	if (!rtc.begin(&i2cBusTwo)) {
		Log_Println("RTC> DS3231 not found on i2cBusTwo", LOGLEVEL_NOTICE);
		return;
	}
	rtcAvailable = true;

	if (rtc.lostPower()) {
		Log_Println("RTC> DS3231 lost power - waiting for NTP to set the time", LOGLEVEL_NOTICE);
		return;
	}

	// RTC stores UTC; seed the system clock from it so the time is correct
	// immediately, even before WiFi/NTP are up.
	const DateTime now = rtc.now();
	if (now.year() < 2024) {
		Log_Println("RTC> stored time implausible - waiting for NTP", LOGLEVEL_NOTICE);
		return;
	}
	struct timeval tv = {.tv_sec = (time_t) now.unixtime(), .tv_usec = 0};
	settimeofday(&tv, nullptr);
	Log_Printf(LOGLEVEL_NOTICE, "RTC> system time set from DS3231: %04d-%02d-%02d %02d:%02d:%02d UTC", now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
}

void Rtc_SetFromSystemTime(void) {
	if (!rtcAvailable) {
		return;
	}
	struct tm timeinfo;
	if (!systemTimeIsValid(&timeinfo)) {
		return;
	}
	// store UTC in the RTC (DateTime(uint32_t) interprets the value as unix-time)
	I2cBusTwo_Lock();
	rtc.adjust(DateTime((uint32_t) time(nullptr)));
	I2cBusTwo_Unlock();
	Log_Println("RTC> DS3231 disciplined from system time", LOGLEVEL_NOTICE);
}

bool Rtc_SetTime(time_t utcEpoch) {
	if (utcEpoch < rtcMinPlausibleEpoch) {
		return false;
	}
	// set the system clock first so the time is correct even without an RTC
	struct timeval tv = {.tv_sec = utcEpoch, .tv_usec = 0};
	settimeofday(&tv, nullptr);
	// then persist it into the battery-backed RTC so it survives a power loss
	if (rtcAvailable) {
		I2cBusTwo_Lock();
		rtc.adjust(DateTime((uint32_t) utcEpoch));
		I2cBusTwo_Unlock();
	}
	Log_Printf(LOGLEVEL_NOTICE, "RTC> system time set from browser: %lu UTC", (unsigned long) utcEpoch);
	return true;
}

bool Rtc_IsAvailable(void) {
	return rtcAvailable;
}

bool Rtc_LostPower(void) {
	if (!rtcAvailable) {
		return false;
	}
	I2cBusTwo_Lock();
	const bool lost = rtc.lostPower();
	I2cBusTwo_Unlock();
	return lost;
}

float Rtc_GetTemperature(void) {
	if (!rtcAvailable) {
		return NAN;
	}
	I2cBusTwo_Lock();
	const float temp = rtc.getTemperature();
	I2cBusTwo_Unlock();
	return temp;
}

void Rtc_Cyclic(void) {
	if (!rtcAvailable) {
		return;
	}

	const uint32_t millisNow = millis();

	// Periodically write the (NTP-disciplined) system time back to the RTC to
	// compensate for drift. Only when WiFi is up so we trust the system clock.
	if (millisNow - lastResyncTimestamp >= rtcResyncInterval) {
		lastResyncTimestamp = millisNow;
		if (Wlan_IsConnected()) {
			Rtc_SetFromSystemTime();
		}
	}

	// Publish the current time via MQTT
	#ifdef MQTT_ENABLE
	if (millisNow - lastPublishTimestamp >= rtcPublishInterval) {
		lastPublishTimestamp = millisNow;
		struct tm timeinfo;
		if (systemTimeIsValid(&timeinfo)) {
			char timeStringBuff[32];
			strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
			publishMqtt(topicRtc, timeStringBuff, false);
		}
	}
	#endif
}

#else // RTC_ENABLE not set: provide empty stubs

void Rtc_Init(void) {
}
void Rtc_Cyclic(void) {
}
void Rtc_SetFromSystemTime(void) {
}

bool Rtc_SetTime(time_t utcEpoch) {
	(void) utcEpoch;
	return false;
}

bool Rtc_IsAvailable(void) {
	return false;
}

bool Rtc_LostPower(void) {
	return false;
}

float Rtc_GetTemperature(void) {
	return NAN;
}

#endif
