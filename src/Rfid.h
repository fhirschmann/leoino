#pragma once

constexpr uint8_t cardIdSize = 4u;
constexpr uint8_t cardIdStringSize = (cardIdSize * 3u) + 1u;

extern char gCurrentRfidTagId[cardIdStringSize];

void Rfid_ResetOldRfid(void);
void Rfid_Init(void);
void Rfid_Cyclic(void);
void Rfid_Exit(void);
void Rfid_TaskPause(void);
void Rfid_TaskResume(void);
void Rfid_TaskReset(void);
void Rfid_WakeupCheck(void);
void Rfid_PreferenceLookupHandler(void);
const char *Rfid_GetReaderFirmwareVersion(void); // PN5180 reader firmware version (RFID_READER_TYPE_RUNTIME builds only)

// Parse a stored tag assignment "#file#pos#mode#track" into its fields (any out-param may be NULL).
bool Rfid_ParseAssignment(const char *stored, char *fileOut, size_t fileLen, uint32_t *posOut, uint32_t *modeOut, uint16_t *trackOut);
// Convert a raw card id to the 12-digit decimal queue key (out >= cardIdStringSize bytes).
void Rfid_CardIdToString(const byte *id, char *out);
// Shared post-read card handling for both RFID drivers (see RfidCommon.cpp). cardType is the ISO
// protocol string logged on PN5180, or NULL (MFRC522).
void Rfid_HandleCardDetected(const byte *uid, byte *lastValidcardId, const char *cardType);
