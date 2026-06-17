#pragma once

#include <stdint.h>

// Daily listening-time statistics: accumulates playback seconds per local calendar day in a
// 365-day ring buffer (persisted in NVS), so the web interface can show how long was listened
// today / yesterday / last 7 / last 30 days. Time comes from the system clock (NTP/RTC); while
// the clock is not yet valid, ticks are ignored.

void Playstats_Init(void);
void Playstats_AddSecond(void); // called once per played second from the audio loop
void Playstats_Save(void); // flush the RAM buffer to NVS if dirty (periodic + on shutdown)

uint32_t Playstats_GetToday(void); // seconds listened today (local calendar day)
uint32_t Playstats_GetYesterday(void); // seconds listened yesterday
uint32_t Playstats_GetLastDays(uint16_t days); // sum over the last <days> calendar days (incl. today)

// Raw ring-buffer access for backup/restore (JSON is built/parsed by the caller).
uint16_t Playstats_GetRingSize(void); // number of day-slots (365)
uint32_t Playstats_GetRingLastDay(void); // local day number of the most recent tracked day
uint32_t Playstats_GetRingSlot(uint16_t i); // seconds in raw slot i (0..size-1)
void Playstats_RestoreRing(uint32_t lastDay, const uint32_t *slots, uint16_t count); // restore from backup
