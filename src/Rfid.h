#pragma once

constexpr uint8_t cardIdSize = 4u;
constexpr uint8_t cardIdStringSize = (cardIdSize * 3u) + 1u;

extern char gCurrentRfidTagId[cardIdStringSize];

void Rfid_ResetOldRfid(void);
// Lay a "virtual" RFID card: push a 12-digit id into the same queue a physical scan uses,
// so the assigned content plays. Shared by the web endpoint (/rfidtrigger) and MQTT.
void Rfid_QueueCardString(const char *cardIdStr);
void Rfid_Init(void);
void Rfid_Cyclic(void);
void Rfid_Exit(void);
void Rfid_TaskPause(void);
void Rfid_TaskResume(void);
void Rfid_TaskReset(void);
void Rfid_WakeupCheck(void);
void Rfid_PreferenceLookupHandler(void);
const char *Rfid_GetReaderFirmwareVersion(void); // PN5180 reader firmware version (RFID_READER_TYPE_RUNTIME builds only)
