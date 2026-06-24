#pragma once
#include "values.h" // OPMODE_* constants used by System_UsesLocalAudio()

#include <Preferences.h>

extern Preferences gPrefsRfid;
extern Preferences gPrefsSettings;

void System_Init_Rfid_Prefs(void);
void System_Init(void);
void System_Cyclic(void);

// Serialize all access to the shared secondary I2C bus (i2cBusTwo). The OLED frame transfer runs on
// the main loop while the RC522-I2C reader runs in its own task and the port-expander is poked from
// several places; without this lock those interleave on the non-thread-safe TwoWire and desync the
// SH1106 (OLED goes black). Recursive, so same-task nesting (e.g. Port_Cyclic -> Port_Write) is safe.
// Created in System_Init (single-threaded boot, before any I2C task starts); a no-op until then.
void I2cBusTwo_Lock(void);
void I2cBusTwo_Unlock(void);
void System_UpdateActivityTimer(void);
void System_RequestSleep(void);
void System_ReloadSleepSettings(void); // re-read "no sleep while powered" config from NVS
bool System_IsExternallyPowered(void); // heuristic: battery voltage at/above the powered threshold
void System_Restart(void);
bool System_SetSleepTimer(uint8_t minutes);
void System_DisableSleepTimer();
bool System_IsSleepTimerEnabled(void);
uint32_t System_GetSleepTimerTimeStamp(void);
bool System_IsSleepPending(void);
uint8_t System_GetSleepTimer(void);
uint32_t System_GetSleepTimerRemainingSeconds(void);
void System_SetLockControls(bool value);
void System_ToggleLockControls(void);
bool System_AreControlsLocked(void);
void System_IndicateError(void);
void System_IndicateOk(void);
bool System_IsWebControlAllowed(void);
void System_SetOperationMode(uint8_t opMode);
uint8_t System_GetOperationMode(void);
// True when the current operation mode routes audio through the local player (normal playback or
// Bluetooth-source). Used to gate playback-control commands that only make sense for local audio.
static inline bool System_UsesLocalAudio(void) {
	uint8_t m = System_GetOperationMode();
	return m == OPMODE_NORMAL || m == OPMODE_BLUETOOTH_SOURCE;
}
uint8_t System_GetOperationModeFromNvs(void);
void System_esp_print_tasks(void);
void System_ShowWakeUpReason();
void System_PauseTasksDuringUpload(bool pause);
bool System_IsColdStart();
