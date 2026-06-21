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

	#if defined(RFID_READER_TYPE_RUNTIME)
extern TwoWire i2cBusTwo;
static MFRC522_I2C mfrc522I2C(MFRC522_ADDR, RST_PIN, &i2cBusTwo);
static MFRC522 mfrc522(RFID_CS, RST_PIN);
	#endif

void RfidMfrc522_Init(uint8_t readerType) {
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

template <typename Reader>
static void RfidMfrc522_TaskImpl(Reader &reader) {
	uint8_t control = 0x00;
	static byte lastValidcardId[cardIdSize];

	for (;;) {
		if (mfrcTaskResetRequested) {
			memset(lastValidcardId, 0, sizeof(lastValidcardId));
			mfrcTaskResetRequested = false;
		}
		if (RFID_SCAN_INTERVAL / 2 >= 20) {
			vTaskDelay(portTICK_PERIOD_MS * (RFID_SCAN_INTERVAL / 2));
		} else {
			vTaskDelay(portTICK_PERIOD_MS * 20);
		}
		if ((millis() - Rfid_LastRfidCheckTimestamp) >= RFID_SCAN_INTERVAL) {
			// Log_Printf(LOGLEVEL_DEBUG, "%u", uxTaskGetStackHighWaterMark(NULL));

			Rfid_LastRfidCheckTimestamp = millis();
			// Reset the loop if no new card is present on the sensor/reader. This saves the entire process when idle.

			if (!reader.PICC_IsNewCardPresent()) {
				continue;
			}

			// Select one of the cards
			if (!reader.PICC_ReadCardSerial()) {
				continue;
			}

			if (!gPlayProperties.pauseIfRfidRemoved) {
				reader.PICC_HaltA();
				reader.PCD_StopCrypto1();
			}

			Rfid_HandleCardDetected(reader.uid.uidByte, lastValidcardId, NULL);

			if (gPlayProperties.pauseIfRfidRemoved) {
				// https://github.com/miguelbalboa/rfid/issues/188; voodoo! :-)
				while (true) {
					if (RFID_SCAN_INTERVAL / 2 >= 20) {
						vTaskDelay(portTICK_PERIOD_MS * (RFID_SCAN_INTERVAL / 2));
					} else {
						vTaskDelay(portTICK_PERIOD_MS * 20);
					}
					control = 0;
					for (uint8_t i = 0u; i < 3; i++) {
						if (!reader.PICC_IsNewCardPresent()) {
							if (reader.PICC_ReadCardSerial()) {
								control |= 0x16;
							}
							if (reader.PICC_ReadCardSerial()) {
								control |= 0x16;
							}
							control += 0x1;
						}
						control += 0x4;
					}

					if (control == 13 || control == 14) {
						// card is still there
					} else {
						break;
					}
				}

				Log_Println(rfidTagRemoved, LOGLEVEL_NOTICE);
				if (!gPlayProperties.pausePlay && System_GetOperationMode() != OPMODE_BLUETOOTH_SINK) {
					AudioPlayer_SetTrackControl(gPlayProperties.stopIfRfidRemoved ? STOP : PAUSEPLAY);
					Log_Println(rfidTagReapplied, LOGLEVEL_NOTICE);
				}
				reader.PICC_HaltA();
				reader.PCD_StopCrypto1();
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
