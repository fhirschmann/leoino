#include <Arduino.h>
#include "settings.h"

#include "Log.h"
#include "MemX.h"
#include "Queues.h"
#include "Rfid.h"
#include "RfidConfig.h"

extern void RfidMfrc522_Init(uint8_t readerType);
extern void RfidMfrc522_Cyclic(void);
extern void RfidMfrc522_Exit(void);
extern void RfidMfrc522_TaskReset(void);
extern void RfidMfrc522_WakeupCheck(void);

extern void RfidPn5180_Init(void);
extern void RfidPn5180_Cyclic(void);
extern void RfidPn5180_Exit(void);
extern void RfidPn5180_TaskReset(void);
extern void RfidPn5180_WakeupCheck(void);
extern bool RfidPn5180_IsCardApplied(void);

TaskHandle_t rfidTaskHandle = NULL;

void Rfid_Init(void) {
#if defined(RFID_READER_TYPE_RUNTIME)
	RfidConfig_Init();
	RfidReaderType readerType = RfidConfig_GetReaderType();
	if ((readerType == RfidReaderType::TYPE_MFRC522_SPI) || (readerType == RfidReaderType::TYPE_MFRC522_I2C)) {
		RfidMfrc522_Init(readerType);
	} else {
		RfidPn5180_Init();
	}
#endif
}

void Rfid_Cyclic(void) {
#if defined(RFID_READER_TYPE_RUNTIME)
	if (RfidConfig_GetReaderType() == RfidReaderType::TYPE_PN5180) {
		RfidPn5180_Cyclic();
	} else {
		RfidMfrc522_Cyclic();
	}
#endif
}

void Rfid_Exit(void) {
#if defined(RFID_READER_TYPE_RUNTIME)
	Log_Println("shutdown rfid-reader..", LOGLEVEL_NOTICE);
	if (RfidConfig_GetReaderType() == RfidReaderType::TYPE_PN5180) {
		RfidPn5180_Exit();
	} else {
		RfidMfrc522_Exit();
	}
#endif
}

// Rfid_TaskPause and Rfid_TaskResume are implemented in RfidCommon.cpp

void Rfid_TaskReset(void) {
#if defined(RFID_READER_TYPE_RUNTIME)
	if (RfidConfig_GetReaderType() == RfidReaderType::TYPE_PN5180) {
		RfidPn5180_TaskReset();
	} else {
		RfidMfrc522_TaskReset();
	}
#endif
}

void Rfid_WakeupCheck(void) {
#if defined(RFID_READER_TYPE_RUNTIME)
	if (RfidConfig_GetReaderType() == RfidReaderType::TYPE_PN5180) {
		RfidPn5180_WakeupCheck();
	} else {
		RfidMfrc522_WakeupCheck();
	}
#endif
}

// True while a tag physically sits on the reader (the PN5180 driver tracks presence including
// its removal grace period). The MFRC522 driver has no presence tracking, so there this always
// reports false and features gated on it are inactive.
bool Rfid_IsCardApplied(void) {
#if defined(RFID_READER_TYPE_RUNTIME)
	if (RfidConfig_GetReaderType() == RfidReaderType::TYPE_PN5180) {
		return RfidPn5180_IsCardApplied();
	}
#endif
	return false;
}
