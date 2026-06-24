#pragma once

#include <time.h> // time_t

// Battery-backed hardware real-time-clock (DS3231 on i2cBusTwo).
// Keeps ESPuino's system time correct even without WiFi/NTP. The RTC always
// stores UTC; the system timezone (settings.h: timeZone) is applied on top.

void Rtc_Init(void);
void Rtc_Cyclic(void);

// true if a DS3231 was detected on the bus during Rtc_Init()
bool Rtc_IsAvailable(void);

// true if the RTC lost power since it was last set (time is not trustworthy)
bool Rtc_LostPower(void);

// Write the current system time (UTC) into the RTC. Called after a successful
// NTP-sync (NTP is the master clock) and via CMD_RTC_RESYNC.
void Rtc_SetFromSystemTime(void);

// Set the system clock (and the battery-backed RTC, if present) from the given
// UTC unix-time. Used by the web UI to set the time from the browser on offline
// devices that have no WiFi/NTP. Returns false if the timestamp is implausible
// (before 2024). Works even when no RTC is present (system clock only).
bool Rtc_SetTime(time_t utcEpoch);

// DS3231 die-temperature in degrees Celsius (NaN if no RTC present)
float Rtc_GetTemperature(void);
