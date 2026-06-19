#include <Arduino.h>
#include "settings.h"

#include "Playstats.h"

#include "Log.h"
#include "System.h"

#include <string.h>
#include <time.h>

// 365-day ring buffer of playback seconds per local calendar day. Indexed by (dayNumber % DAYS),
// where dayNumber is the count of local days since the epoch. lastDay tracks the most recent day
// written so the gap since then can be zeroed when the day rolls over (or the device was off).
static constexpr uint16_t PLAYSTATS_DAYS = 365;
static constexpr uint32_t PLAYSTATS_MAGIC = 0x504C5331; // "PLS1"
static constexpr uint32_t PLAYSTATS_MIN_VALID_DAY = 18262; // ~2020-01-01, guards against pre-NTP clock

static uint32_t gDays[PLAYSTATS_DAYS] = {0};
static uint32_t gLastDay = 0; // local day number of the most recent tracked day (0 = none yet)
static bool gDirty = false;
static uint32_t gLastSaveMs = 0;

// Per-card play counters live in their own NVS namespace, keyed by the 12-digit tag id.
static Preferences gPrefsCardCnt;
static bool gCardCntReady = false;
static void Playstats_CardCntInit(void) {
	if (!gCardCntReady) {
		gPrefsCardCnt.begin("rfidPlayCnt");
		gCardCntReady = true;
	}
}
void Playstats_NoteCardPlay(const char *tagId) {
	if (!tagId || !tagId[0]) {
		return;
	}
	Playstats_CardCntInit();
	gPrefsCardCnt.putULong(tagId, gPrefsCardCnt.getULong(tagId, 0) + 1);
}
uint32_t Playstats_GetCardPlays(const char *tagId) {
	Playstats_CardCntInit();
	return gPrefsCardCnt.getULong(tagId, 0);
}
void Playstats_ClearCardPlays(const char *tagId) {
	Playstats_CardCntInit();
	gPrefsCardCnt.remove(tagId);
}

// Days since 1970-01-01 for a (proleptic Gregorian) calendar date (Howard Hinnant's algorithm).
// Avoids needing tm_gmtoff (not available in this newlib build): we feed it the local Y/M/D.
static long Playstats_DaysFromCivil(int y, unsigned m, unsigned d) {
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned) (y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long) doe - 719468;
}

// Inverse of Playstats_DaysFromCivil: turn a day number (days since 1970-01-01) back into a
// (proleptic Gregorian) calendar date (Howard Hinnant's civil_from_days algorithm).
void Playstats_DayToDate(uint32_t dayNum, int *year, int *month, int *dayOfMonth) {
	long z = (long) dayNum + 719468;
	long era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned long doe = (unsigned long) (z - era * 146097); // [0, 146096]
	unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
	long y = (long) yoe + era * 400;
	unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
	unsigned long mp = (5 * doy + 2) / 153; // [0, 11]
	unsigned long d = doy - (153 * mp + 2) / 5 + 1; // [1, 31]
	unsigned long m = mp < 10 ? mp + 3 : mp - 9; // [1, 12]
	if (year) {
		*year = (int) (y + (m <= 2));
	}
	if (month) {
		*month = (int) m;
	}
	if (dayOfMonth) {
		*dayOfMonth = (int) d;
	}
}

// Local calendar day number (days since epoch in local time). 0 if the clock is not valid yet.
static uint32_t Playstats_CurrentDay(void) {
	time_t now = time(nullptr);
	struct tm timeinfo;
	if (!localtime_r(&now, &timeinfo)) {
		return 0;
	}
	long day = Playstats_DaysFromCivil(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
	if (day < (long) PLAYSTATS_MIN_VALID_DAY) {
		return 0;
	}
	return (uint32_t) day;
}

// Roll the ring buffer forward to <today>, zeroing days that elapsed since gLastDay (so stale
// slots from ~a year ago don't leak into the windows). Resets everything if the gap exceeds a year.
static void Playstats_AdvanceTo(uint32_t today) {
	if (today == 0 || today == gLastDay) {
		return;
	}
	if (gLastDay == 0 || (today - gLastDay) >= PLAYSTATS_DAYS) {
		for (uint16_t i = 0; i < PLAYSTATS_DAYS; i++) {
			gDays[i] = 0;
		}
	} else {
		for (uint32_t d = gLastDay + 1; d <= today; d++) {
			gDays[d % PLAYSTATS_DAYS] = 0;
		}
	}
	gLastDay = today;
	gDirty = true;
}

void Playstats_Init(void) {
	size_t got = gPrefsSettings.getBytesLength("playStats");
	if (got == sizeof(uint32_t) /*magic*/ + sizeof(uint32_t) /*lastDay*/ + sizeof(gDays)) {
		uint8_t buf[sizeof(uint32_t) * 2 + sizeof(gDays)];
		gPrefsSettings.getBytes("playStats", buf, sizeof(buf));
		uint32_t magic;
		memcpy(&magic, buf, sizeof(magic));
		if (magic == PLAYSTATS_MAGIC) {
			memcpy(&gLastDay, buf + sizeof(uint32_t), sizeof(gLastDay));
			memcpy(gDays, buf + sizeof(uint32_t) * 2, sizeof(gDays));
		}
	}
	gLastSaveMs = millis();
}

void Playstats_AddSecond(void) {
	uint32_t today = Playstats_CurrentDay();
	if (today == 0) {
		return; // clock not valid yet
	}
	Playstats_AdvanceTo(today);
	gDays[today % PLAYSTATS_DAYS]++;
	gDirty = true;

	// Persist at most once per minute to spare the NVS flash; a final flush happens on shutdown.
	if (millis() - gLastSaveMs >= 60000u) {
		Playstats_Save();
	}
}

void Playstats_Save(void) {
	if (!gDirty) {
		return;
	}
	uint8_t buf[sizeof(uint32_t) * 2 + sizeof(gDays)];
	uint32_t magic = PLAYSTATS_MAGIC;
	memcpy(buf, &magic, sizeof(magic));
	memcpy(buf + sizeof(uint32_t), &gLastDay, sizeof(gLastDay));
	memcpy(buf + sizeof(uint32_t) * 2, gDays, sizeof(gDays));
	gPrefsSettings.putBytes("playStats", buf, sizeof(buf));
	gDirty = false;
	gLastSaveMs = millis();
}

uint32_t Playstats_GetToday(void) {
	uint32_t today = Playstats_CurrentDay();
	if (today == 0) {
		return 0;
	}
	Playstats_AdvanceTo(today);
	return gDays[today % PLAYSTATS_DAYS];
}

uint32_t Playstats_GetYesterday(void) {
	uint32_t today = Playstats_CurrentDay();
	if (today == 0 || gLastDay == 0) {
		return 0;
	}
	Playstats_AdvanceTo(today);
	return (today >= 1) ? gDays[(today - 1) % PLAYSTATS_DAYS] : 0;
}

uint32_t Playstats_GetLastDays(uint16_t days) {
	uint32_t today = Playstats_CurrentDay();
	if (today == 0) {
		return 0;
	}
	Playstats_AdvanceTo(today);
	if (days > PLAYSTATS_DAYS) {
		days = PLAYSTATS_DAYS;
	}
	uint32_t sum = 0;
	for (uint16_t i = 0; i < days && i <= today; i++) {
		sum += gDays[(today - i) % PLAYSTATS_DAYS];
	}
	return sum;
}

uint16_t Playstats_GetRingSize(void) {
	return PLAYSTATS_DAYS;
}
uint32_t Playstats_GetRingLastDay(void) {
	return gLastDay;
}
uint32_t Playstats_GetRingSlot(uint16_t i) {
	return (i < PLAYSTATS_DAYS) ? gDays[i] : 0;
}
void Playstats_RestoreRing(uint32_t lastDay, const uint32_t *slots, uint16_t count) {
	if (count > PLAYSTATS_DAYS) {
		count = PLAYSTATS_DAYS;
	}
	for (uint16_t i = 0; i < PLAYSTATS_DAYS; i++) {
		gDays[i] = (i < count && slots) ? slots[i] : 0;
	}
	gLastDay = lastDay;
	gDirty = true;
	Playstats_Save();
}
