#include <Arduino.h>
#include "settings.h"

#include "AudioPlayer.h"
#include "HallEffectSensor.h"
#include "Log.h"
#include "MemX.h"
#include "Queues.h"
#include "Rfid.h"
#include "RfidConfig.h"
#include "System.h"

#include <esp_task_wdt.h>

#if defined(RFID_READER_TYPE_RUNTIME)
	#include <MFRC522.h>
	#define MFRC522_firmware_referenceV0_0
	#define MFRC522_firmware_referenceV1_0
	#define MFRC522_firmware_referenceV2_0
	#define FM17522_firmware_reference
	#include "Wire.h"

	#include <MFRC522_I2C.h>

extern unsigned long Rfid_LastRfidCheckTimestamp;
extern TaskHandle_t rfidTaskHandle;
static void RfidMfrc522_Task(void *parameter);
static volatile bool mfrcTaskResetRequested = false; // set from another task via RfidMfrc522_TaskReset()

// Cached once at init rather than read from NVS on every task-loop iteration; a restart is required
// for a change to take effect, same as the other MFRC522/PN5180-specific settings.
static uint16_t rfidScanInterval = 100;

	#if defined(RFID_READER_TYPE_RUNTIME)
extern TwoWire i2cBusTwo;
static MFRC522_I2C mfrc522I2C(MFRC522_ADDR, RST_PIN, &i2cBusTwo);
static MFRC522 mfrc522(RFID_CS, RST_PIN);
	#endif

void RfidMfrc522_Init(uint8_t readerType) {
	rfidScanInterval = gPrefsRfid.getUShort("rfidScanIntv", 100);
	uint8_t rfidGain = gPrefsRfid.getUChar("mfrc522Gain", 7u); // default to maximum gain
	rfidGain = (rfidGain & 0x07) << 4; // only lower 3 bits are valid, shift to correct position for register
	if (readerType == RfidReaderType::TYPE_MFRC522_SPI) {
		SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_CS);
		SPI.setFrequency(1000000);
		mfrc522.PCD_Init();
		delay(10);
		// byte firmwareVersion = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
		// Log_Printf(LOGLEVEL_DEBUG, "RC522 firmware version=%#lx", firmwareVersion);
		mfrc522.PCD_SetAntennaGain(rfidGain);
	} else if (readerType == RfidReaderType::TYPE_MFRC522_I2C) {
	#if defined(I2C_2_ENABLE)
		mfrc522I2C.PCD_Init();
		delay(10);
		// byte firmwareVersion = mfrc522I2C.PCD_ReadRegister(MFRC522_I2C::VersionReg);
		// Log_Printf(LOGLEVEL_DEBUG, "RC522 I2C firmware version=%#lx", firmwareVersion);
		mfrc522I2C.PCD_SetAntennaGain(rfidGain);
	#endif
	} else {
		Log_Println("RfidMfrc522_Init: unsupported reader type", LOGLEVEL_ERROR);
		return;
	}

	delay(50);
	Log_Println(rfidScannerReady, LOGLEVEL_DEBUG);

	if (rfidTaskHandle == NULL) {
		xTaskCreatePinnedToCore(
			RfidMfrc522_Task, /* Function to implement the task */
			"rfid", /* Name of the task */
			3072, /* Stack size in words */
			NULL, /* Task input parameter */
			2 | portPRIVILEGE_BIT, /* Priority of the task */
			&rfidTaskHandle, /* Task handle. */
			0 /* Core where the task should run */
		);
	}
}

void RfidMfrc522_TaskReset(void) {
	Rfid_LastRfidCheckTimestamp = millis();
	mfrcTaskResetRequested = true;
}

// Deterministic "is a card still on the antenna?" poll used by pauseIfRfidRemoved
// mode. The card is kept parked in the ISO-14443 HALT state between polls; WUPA
// (PICC_WakeupA, 0x52) is the only REQ-family command that wakes a HALTed card.
// Return the raw status: STATUS_TIMEOUT means the field is empty, while another
// transmission error means a card answered but the frame was corrupted.
// This replaces the old REQA-based detection (PICC_IsNewCardPresent sends REQA,
// 0x26, which only invites cards in the IDLE state) whose non-deterministic misses
// on a stationary card forced an ever-growing debounce. Reader is either MFRC522
// or the I2C MFRC522_I2C class; the Reader:: register/status constants and the
// PICC_WakeupA return type both differ between the two libraries, so we let the
// compiler pick the right ones per instantiation.
template <typename Reader>
static uint8_t RfidMfrc522_PollCardPresence(Reader &reader) {
	byte bufferATQA[2];
	byte bufferSize = sizeof(bufferATQA);
	// Reset baud-rate / modulation-width registers exactly like
	// PICC_IsNewCardPresent() does internally; some readers won't answer WUPA
	// reliably otherwise. Reader::* resolves to the correct (SPI-shifted vs I2C)
	// register addresses for whichever library this is instantiated with.
	reader.PCD_WriteRegister(Reader::TxModeReg, 0x00);
	reader.PCD_WriteRegister(Reader::RxModeReg, 0x00);
	reader.PCD_WriteRegister(Reader::ModWidthReg, 0x26);
	auto result = reader.PICC_WakeupA(bufferATQA, &bufferSize);
	// Immediately park the card back in HALT so the next WUPA is meaningful.
	reader.PICC_HaltA();
	return static_cast<uint8_t>(result);
}

template <typename Reader>
static void RfidMfrc522_TaskImpl(Reader &reader) {
	static byte lastValidcardId[cardIdSize];

	for (;;) {
		if (mfrcTaskResetRequested) {
			memset(lastValidcardId, 0, sizeof(lastValidcardId));
			mfrcTaskResetRequested = false;
		}
		if (rfidScanInterval / 2 >= 20) {
			vTaskDelay(portTICK_PERIOD_MS * (rfidScanInterval / 2));
		} else {
			vTaskDelay(portTICK_PERIOD_MS * 20);
		}
		if (Rfid_ConsumeLastTagReset()) {
			// An assignment changed (or the web UI started playback): the card on/near the reader may now
			// mean something else, so it must not be treated as "same card re-applied" any more.
			memset(lastValidcardId, 0, sizeof(lastValidcardId));
		}
		if ((millis() - Rfid_LastRfidCheckTimestamp) >= rfidScanInterval) {
			// Log_Printf(LOGLEVEL_DEBUG, "%u", uxTaskGetStackHighWaterMark(NULL));

			Rfid_LastRfidCheckTimestamp = millis();
			// Reset the loop if no new card is present on the sensor/reader. This saves the entire process when idle.

			// Each reader access is serialized on the shared i2cBusTwo (RC522-I2C variant) so it can't
			// interleave with the OLED frame transfer on the main loop and desync the panel. The lock
			// is taken per call, never across the vTaskDelay in the card-removal poll below.
			I2cBusTwo_Lock();
			const bool newCard = reader.PICC_IsNewCardPresent();
			I2cBusTwo_Unlock();
			if (!newCard) {
				continue;
			}

			// Select one of the cards
			I2cBusTwo_Lock();
			const bool readOk = reader.PICC_ReadCardSerial();
			I2cBusTwo_Unlock();
			if (!readOk) {
				continue;
			}

			if (!gPlayProperties.pauseIfRfidRemoved) {
				I2cBusTwo_Lock();
				reader.PICC_HaltA();
				reader.PCD_StopCrypto1();
				I2cBusTwo_Unlock();
			}

			Rfid_HandleCardDetected(reader.uid.uidByte, lastValidcardId, NULL);

			if (gPlayProperties.pauseIfRfidRemoved) {
				// Park the freshly-selected card in the HALT state so the WUPA-based
				// presence poll below can wake it deterministically. Without this the
				// card is left ACTIVE and only REQA (which ignores ACTIVE/HALT cards)
				// was available, causing the notorious pause/resume flap on stationary
				// cards. See RfidMfrc522_PollCardPresence().
				I2cBusTwo_Lock();
				reader.PICC_HaltA();
				reader.PCD_StopCrypto1();
				I2cBusTwo_Unlock();

				// Poll until the card is physically removed. Each WUPA poll is a clean
				// yes/no, so a small debounce is enough to swallow the rare genuinely
				// dropped poll (RF noise) without the old REQA "voodoo". Set to 1 to
				// test raw WUPA reliability with zero tolerance for a missed poll.
				constexpr uint8_t removalDebounceCycles = 2;
				constexpr uint32_t noAnswerTimeoutMs = 1500;
				uint8_t consecutiveMisses = 0;
				uint32_t lastAnswerAt = millis();
				while (true) {
					if (rfidScanInterval / 2 >= 20) {
						vTaskDelay(portTICK_PERIOD_MS * (rfidScanInterval / 2));
					} else {
						vTaskDelay(portTICK_PERIOD_MS * 20);
					}
					I2cBusTwo_Lock();
					const uint8_t wupaStatus = RfidMfrc522_PollCardPresence(reader);
					I2cBusTwo_Unlock();
					if (wupaStatus == static_cast<uint8_t>(Reader::STATUS_OK) || wupaStatus == static_cast<uint8_t>(Reader::STATUS_COLLISION)) {
						consecutiveMisses = 0;
						lastAnswerAt = millis();
					} else if (wupaStatus != static_cast<uint8_t>(Reader::STATUS_TIMEOUT)) {
						// A parity/protocol/CRC error means a card did answer; do not count it
						// as an empty-field miss. The timeout below remains the backstop.
					} else if (++consecutiveMisses >= removalDebounceCycles) {
						break;
					}
					if ((millis() - lastAnswerAt) >= noAnswerTimeoutMs) {
						break;
					}
				}

				Log_Println(rfidTagRemoved, LOGLEVEL_NOTICE);
				// Only pause if there's actually something to pause -- otherwise removing a card after the
				// playlist has already finished naturally queues a PAUSEPLAY that AudioPlayer_Cyclic() then
				// rejects with "no playmode change while idle", which is a confusing error for a normal action.
				if (!gPlayProperties.pausePlay && !gPlayProperties.playlistFinished && gPlayProperties.playMode != NO_PLAYLIST && System_GetOperationMode() != OPMODE_BLUETOOTH_SINK) {
					AudioPlayer_SetTrackControl(gPlayProperties.stopIfRfidRemoved ? STOP : PAUSEPLAY);
					Log_Println(rfidTagReapplied, LOGLEVEL_NOTICE);
				}
				I2cBusTwo_Lock();
				reader.PICC_HaltA();
				reader.PCD_StopCrypto1();
				// A false removal can leave a still-present card HALTed, while the next
				// PICC_IsNewCardPresent() uses REQA and cannot see HALTed cards. Cycle the
				// field so such a card returns to IDLE and can be detected again.
				reader.PCD_AntennaOff();
				vTaskDelay(portTICK_PERIOD_MS * 10);
				reader.PCD_AntennaOn();
				I2cBusTwo_Unlock();
			}
		}
	}
}

void RfidMfrc522_Task(void *parameter) {
	if (RfidConfig_GetReaderType() == RfidReaderType::TYPE_MFRC522_I2C) {
	#if defined(I2C_2_ENABLE)
		RfidMfrc522_TaskImpl(mfrc522I2C);
	#endif
	} else {
		RfidMfrc522_TaskImpl(mfrc522);
	}
}

void RfidMfrc522_Cyclic(void) {
	// Not necessary as cyclic stuff performed by task Rfid_Task()
}

void RfidMfrc522_Exit(void) {
	Log_Println("shutdown MFRC522..", LOGLEVEL_NOTICE);
	if (RfidConfig_GetReaderType() != RfidReaderType::TYPE_MFRC522_I2C) {
		mfrc522.PCD_SoftPowerDown();
	}
	if (rfidTaskHandle != NULL) {
		vTaskDelete(rfidTaskHandle);
		rfidTaskHandle = NULL;
	}
}

void RfidMfrc522_WakeupCheck(void) {
}

#endif
