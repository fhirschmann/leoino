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

// Per-card play counter (most-played statistics). Incremented when a music card starts a playlist.
void Playstats_NoteCardPlay(const char *tagId);
uint32_t Playstats_GetCardPlays(const char *tagId);
void Playstats_ClearCardPlays(const char *tagId); // drop a card's play counter (e.g. when deleted)

// Raw ring-buffer access for backup/restore (JSON is built/parsed by the caller).
uint16_t Playstats_GetRingSize(void); // number of day-slots (365)
uint32_t Playstats_GetRingLastDay(void); // local day number of the most recent tracked day
uint32_t Playstats_GetRingSlot(uint16_t i); // seconds in raw slot i (0..size-1)
void Playstats_RestoreRing(uint32_t lastDay, const uint32_t *slots, uint16_t count); // restore from backup

// Convert a ring-buffer day number (local days since 1970-01-01) back to a calendar date, e.g. for
// a CSV export. Any of the out-pointers may be null.
void Playstats_DayToDate(uint32_t dayNum, int *year, int *month, int *dayOfMonth);
