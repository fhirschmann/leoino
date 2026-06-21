#include <Arduino.h>
#include "settings.h"

#include "AudioPlayer.h"
#include "Cmd.h"
#include "Common.h"
#include "HallEffectSensor.h"
#include "Log.h"
#include "MemX.h"
#include "Mqtt.h"
#include "Playstats.h"
#include "Queues.h"
#include "Rfid.h"
#include "RfidConfig.h"
#include "System.h"
#include "Web.h"

unsigned long Rfid_LastRfidCheckTimestamp = 0;
char gCurrentRfidTagId[cardIdStringSize] = ""; // No crap here as otherwise it could be shown in GUI
char gOldRfidTagId[cardIdStringSize] = "X"; // Init with crap

// Tries to lookup RFID-tag-string in NVS and extracts parameter from it if found
void Rfid_PreferenceLookupHandler(void) {
#if defined(RFID_READER_TYPE_RUNTIME)
	BaseType_t rfidStatus;
	char rfidTagId[cardIdStringSize];
	char _file[255];
	uint32_t _lastPlayPos = 0;
	uint16_t _trackLastPlayed = 0;
	uint32_t _playMode = 1;

	rfidStatus = xQueueReceive(gRfidCardQueue, &rfidTagId, 0);
	if (rfidStatus == pdPASS) {
		System_UpdateActivityTimer();
		strncpy(gCurrentRfidTagId, rfidTagId, cardIdStringSize - 1);
		Log_Printf(LOGLEVEL_INFO, "%s: %s", rfidTagReceived, gCurrentRfidTagId);
		Web_SendWebsocketData(0, WebsocketCodeType::CurrentRfid); // Push new rfidTagId to all websocket-clients
		String s = "-1";
		if (gPrefsRfid.isKey(gCurrentRfidTagId)) {
			s = gPrefsRfid.getString(gCurrentRfidTagId, "-1"); // Try to lookup rfidId in NVS
		}
		if (!s.compareTo("-1")) {
			Log_Println(rfidTagUnknownInNvs, LOGLEVEL_ERROR);
			System_IndicateError();
			// allow to escape from bluetooth mode with an unknown card, switch back to normal mode
			System_SetOperationMode(OPMODE_NORMAL);
			return;
		}

		char *token;
		uint8_t i = 1;
		token = strtok((char *) s.c_str(), stringDelimiter);
		while (token != NULL) { // Try to extract data from string after lookup
			if (i == 1) {
				strncpy(_file, token, sizeof(_file) / sizeof(_file[0]));
			} else if (i == 2) {
				_lastPlayPos = strtoul(token, NULL, 10);
			} else if (i == 3) {
				_playMode = strtoul(token, NULL, 10);
			} else if (i == 4) {
				_trackLastPlayed = strtoul(token, NULL, 10);
			}
			i++;
			token = strtok(NULL, stringDelimiter);
		}

		if (i != 5) {
			Log_Println(errorOccuredNvs, LOGLEVEL_ERROR);
			System_IndicateError();
		} else {
			// Only pass file to queue if strtok revealed 3 items
			if (_playMode >= 100) {
				// Modification-cards can change some settings (e.g. introducing track-looping or sleep after track/playlist).
				Cmd_Action(_playMode);
			} else {
				if (gPlayProperties.dontAcceptRfidTwice) {
					if (strncmp(gCurrentRfidTagId, gOldRfidTagId, 12) == 0) {
						Log_Printf(LOGLEVEL_ERROR, dontAccepctSameRfid, gCurrentRfidTagId);
						// System_IndicateError(); // Enable to have shown error @neopixel every time
						return;
					} else {
						strncpy(gOldRfidTagId, gCurrentRfidTagId, 12);
					}
				}
	#ifdef MQTT_ENABLE
				publishMqtt(topicRfid, gCurrentRfidTagId, false);
	#endif

	#ifdef BLUETOOTH_ENABLE
				// if music rfid was read, go back to normal mode
				if (System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) {
					System_SetOperationMode(OPMODE_NORMAL);
				}
	#endif

				// Restart an audiobook from the beginning if it has not been played for a configured
				// span (default 24 h; 0 = disabled). After such a gap the listener rarely remembers
				// where they were, so resuming mid-chapter is more annoying than starting over. Only
				// the position-saving audiobook modes carry a resume point worth discarding.
				const uint32_t freshAfterHrs = gPrefsSettings.getUInt("freshAfterHrs", 24);
				const bool isAudiobook = (_playMode == AUDIOBOOK || _playMode == AUDIOBOOK_LOOP || _playMode == AUDIOBOOK_RECURSIVE);
				if (freshAfterHrs > 0 && isAudiobook && (_lastPlayPos > 0 || _trackLastPlayed > 0) && Playstats_CardSeenAgoExceeds(gCurrentRfidTagId, freshAfterHrs * 3600UL)) {
					Log_Printf(LOGLEVEL_NOTICE, "Audiobook idle > %u h -> restarting from the beginning", freshAfterHrs);
					_lastPlayPos = 0;
					_trackLastPlayed = 0;
				}
				Playstats_NoteCardSeen(gCurrentRfidTagId); // remember when this card was last started

				Playstats_NoteCardPlay(gCurrentRfidTagId); // count plays per music card (most-played stats)
				AudioPlayer_SetPlaylist(_file, _lastPlayPos, _playMode, _trackLastPlayed);
			}
		}
	}
#endif
}

void Rfid_ResetOldRfid() {
	Log_Println("RFID: Resetting old card state", LOGLEVEL_INFO);
	strncpy(gCurrentRfidTagId, "000000000000", cardIdStringSize - 1);
	strncpy(gOldRfidTagId, "X", cardIdStringSize - 1);
	Rfid_TaskReset();
}

// Parse a tag assignment stored in NVS ("#<fileOrUrl>#<playPos>#<playMode>#<trackLastPlayed>")
// into its four fields. Any out-param may be NULL; absent fields are left at 0 / "". Returns false
// for an empty / "-1" / unset value (an unassigned tag).
bool Rfid_ParseAssignment(const char *stored, char *fileOut, size_t fileLen, uint32_t *posOut, uint32_t *modeOut, uint16_t *trackOut) {
	if (fileOut && fileLen > 0) {
		fileOut[0] = '\0';
	}
	if (posOut) {
		*posOut = 0;
	}
	if (modeOut) {
		*modeOut = 0;
	}
	if (trackOut) {
		*trackOut = 0;
	}
	if (!stored || stored[0] == '\0' || strcmp(stored, "-1") == 0) {
		return false;
	}
	char buf[512];
	strncpy(buf, stored, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	uint8_t i = 1;
	for (char *token = strtok(buf, stringDelimiter); token != NULL; token = strtok(NULL, stringDelimiter), i++) {
		if (i == 1) {
			if (fileOut && fileLen > 0) {
				strncpy(fileOut, token, fileLen - 1);
				fileOut[fileLen - 1] = '\0';
			}
		} else if (i == 2 && posOut) {
			*posOut = strtoul(token, NULL, 10);
		} else if (i == 3 && modeOut) {
			*modeOut = strtoul(token, NULL, 10);
		} else if (i == 4 && trackOut) {
			*trackOut = (uint16_t) strtoul(token, NULL, 10);
		}
	}
	return true;
}

// Convert a raw card id into the 12-digit "%03d"-per-byte string used as the RFID-queue key
// (e.g. {9,0,0,1} -> "009000000001"). out must hold at least cardIdStringSize bytes.
void Rfid_CardIdToString(const byte *id, char *out) {
	out[0] = '\0';
	for (uint8_t i = 0u; i < cardIdSize; i++) {
		char num[4];
		snprintf(num, sizeof(num), "%03d", id[i]);
		strcat(out, num);
	}
}

// Shared post-read handling for both RFID drivers (PN5180 + MFRC522): apply the optional
// hall-effect offset, log the detected tag, build the queue id and run the pause/stop/accept-same
// decision tree that enqueues the card (or toggles play/pause on a re-apply). uid is the raw
// card id (pre-hall-offset); lastValidcardId is the driver's per-task "last accepted card" buffer
// (updated here). cardType, if non-NULL, is logged as the ISO protocol (PN5180 only; NULL for MFRC522).
void Rfid_HandleCardDetected(const byte *uid, byte *lastValidcardId, const char *cardType) {
	byte cardId[cardIdSize];
	memcpy(cardId, uid, cardIdSize);

#ifdef HALLEFFECT_SENSOR_ENABLE
	cardId[cardIdSize - 1] = cardId[cardIdSize - 1] + gHallEffectSensor.waitForState(HallEffectWaitMS);
#endif

	bool sameCardReapplied = false;
	if (memcmp((const void *) lastValidcardId, (const void *) cardId, sizeof(cardId)) == 0) {
		sameCardReapplied = true;
	}

	String hexString;
	for (uint8_t i = 0u; i < cardIdSize; i++) {
		char str[4];
		snprintf(str, sizeof(str), "%02x%c", cardId[i], (i < cardIdSize - 1u) ? '-' : ' ');
		hexString += str;
	}
	Log_Printf(LOGLEVEL_NOTICE, rfidTagDetected, hexString.c_str());
	if (cardType != NULL) {
		Log_Printf(LOGLEVEL_NOTICE, "Card type: %s", cardType);
	}

	char cardIdString[cardIdStringSize];
	Rfid_CardIdToString(cardId, cardIdString);

	if (gPlayProperties.pauseIfRfidRemoved) {
		if (gPlayProperties.stopIfRfidRemoved) {
			// stop-mode: removal fully stops playback (back to the idle animation). The resume point is
			// saved on removal (see the STOP case in AudioPlayer_Loop), so re-applying the card continues
			// an audiobook where it left off; non-position-saving content simply starts over.
			xQueueSend(gRfidCardQueue, cardIdString, 0);
		} else {
#ifdef ACCEPT_SAME_RFID_AFTER_TRACK_END
			if (!sameCardReapplied || gPlayProperties.trackFinished || gPlayProperties.playlistFinished) { // Don't allow to send card to queue if it's the same card again if track or playlist is unfnished
#else
			if (!sameCardReapplied) { // Don't allow to send card to queue if it's the same card again...
#endif
				xQueueSend(gRfidCardQueue, cardIdString, 0);
			} else {
				// If pause-button was pressed while card was not applied, playback could be active. If so: don't pause when card is reapplied again as the desired functionality would be reversed in this case.
				if (gPlayProperties.pausePlay && System_GetOperationMode() != OPMODE_BLUETOOTH_SINK) {
					AudioPlayer_SetTrackControl(PAUSEPLAY); // ... play/pause instead
					Log_Println(rfidTagReapplied, LOGLEVEL_NOTICE);
				}
			}
		}
		memcpy(lastValidcardId, uid, cardIdSize);
	} else {
		xQueueSend(gRfidCardQueue, cardIdString, 0); // If pauseIfRfidRemoved isn't active, every card-apply leads to new playlist-generation
	}
}

#if defined(RFID_READER_TYPE_RUNTIME)
extern TaskHandle_t rfidTaskHandle;
#endif

void Rfid_TaskPause(void) {
#if defined(RFID_READER_TYPE_RUNTIME)
	if (rfidTaskHandle != NULL) {
		vTaskSuspend(rfidTaskHandle);
	}
#endif
}
void Rfid_TaskResume(void) {
#if defined(RFID_READER_TYPE_RUNTIME)
	if (rfidTaskHandle != NULL) {
		Rfid_TaskReset(); // Reset state machine to initial state
		vTaskResume(rfidTaskHandle);
	}
#endif
}
