#include <Arduino.h>
#include "settings.h"

#include "IrReceiver.h"

#include "Cmd.h"
#include "Log.h"
#include "System.h"
#include "Web.h"
#include "values.h"

#ifdef IR_CONTROL_ENABLE
	#include <IRremote.hpp>
	#include <atomic>
#endif

#ifdef IR_CONTROL_ENABLE
// Debounce timestamp for non-volume actions
std::atomic<uint32_t> IrReceiver_LastRcCmdTimestamp = 0u;

// Runtime mapping table (IR command -> short/long CMD_* actions). Persisted in NVS (key "irMap2",
// migrated from the legacy "irMap" layout) and editable via the web UI.
static IrMapping sMap[IR_MAX_MAPPINGS];
static uint8_t sMapCount = 0;

// Learn mode: while armed, received codes are reported to the web UI instead of triggering actions.
static std::atomic<bool> sLearnMode = false;
static uint32_t sLearnDeadline = 0;
static constexpr uint32_t IR_LEARN_TIMEOUT = 30000; // ms; matches the web-UI learn modal timeout

// Long-press handling. A held IR button keeps re-sending (repeat) frames; once they stop arriving
// the button is considered released. Buttons that carry a long action fire the short action on
// release and the long action once the hold passes the (configurable) threshold.
static constexpr uint16_t IR_LONG_PRESS_DEFAULT = 600; // ms a button must be held for the long action
static constexpr uint16_t IR_LONG_PRESS_MIN = 200;
static constexpr uint16_t IR_LONG_PRESS_MAX = 5000;
static constexpr uint32_t IR_HOLD_RELEASE_MS = 250; // no frame for this long -> button released
static uint16_t sLongPressMs = IR_LONG_PRESS_DEFAULT;

// State of the button currently being held (only tracked for mappings that have a long action).
static bool sHoldActive = false;
static uint8_t sHoldIndex = 0; // index into sMap of the held mapping
static uint32_t sHoldStartMs = 0; // when the genuine first frame arrived
static uint32_t sHoldLastFrameMs = 0; // last time a frame for this button was seen
static bool sHoldLongFired = false; // long action already triggered during this hold

static const char *irMapKey = "irMap2"; // current {code,cmd,longCmd} layout (4 bytes/entry)
static const char *irLegacyMapKey = "irMap"; // pre-long-press {code,cmd} layout (3 bytes/entry), migration only
static const char *irLongMsKey = "irLongMs";

static void loadMappings() {
	sHoldActive = false; // a reload (e.g. after a web save) invalidates any in-flight hold
	size_t blobLen = gPrefsSettings.getBytesLength(irMapKey);
	if ((blobLen >= sizeof(IrMapping)) && ((blobLen % sizeof(IrMapping)) == 0)) {
		uint8_t count = blobLen / sizeof(IrMapping);
		if (count > IR_MAX_MAPPINGS) {
			count = IR_MAX_MAPPINGS;
		}
		gPrefsSettings.getBytes(irMapKey, sMap, (size_t) count * sizeof(IrMapping));
		sMapCount = count;
	} else if (size_t legacyLen = gPrefsSettings.getBytesLength(irLegacyMapKey); (legacyLen >= 3) && ((legacyLen % 3) == 0)) {
		// Migrate the legacy 3-byte {code,cmd} layout (pre long-press) so existing remotes survive
		// the firmware update. Each entry gains longCmd = 0 (no long action). The migrated table is
		// rewritten under the new key and the old blob is dropped so this runs exactly once.
		uint8_t count = legacyLen / 3;
		if (count > IR_MAX_MAPPINGS) {
			count = IR_MAX_MAPPINGS;
		}
		uint8_t legacy[IR_MAX_MAPPINGS * 3];
		gPrefsSettings.getBytes(irLegacyMapKey, legacy, (size_t) count * 3);
		for (uint8_t i = 0; i < count; i++) {
			sMap[i].code = (uint16_t) legacy[i * 3] | ((uint16_t) legacy[i * 3 + 1] << 8);
			sMap[i].cmd = legacy[i * 3 + 2];
			sMap[i].longCmd = 0;
		}
		sMapCount = count;
		gPrefsSettings.putBytes(irMapKey, sMap, (size_t) count * sizeof(IrMapping));
		gPrefsSettings.remove(irLegacyMapKey);
	} else {
		// No (valid) mapping stored yet -> start EMPTY: by default the remote does nothing at all.
		// The user assigns every button explicitly in the web UI. This deliberately drops the old
		// compiled-in RC_* layout so the upstream sample-remote codes can't accidentally match a
		// real remote and trigger unwanted actions (e.g. shutdown / Bluetooth toggle).
		sMapCount = 0;
	}

	// Long-press threshold (configurable via the web UI), clamped to a sane range.
	uint32_t ms = gPrefsSettings.getUInt(irLongMsKey, IR_LONG_PRESS_DEFAULT);
	if (ms < IR_LONG_PRESS_MIN) {
		ms = IR_LONG_PRESS_MIN;
	}
	if (ms > IR_LONG_PRESS_MAX) {
		ms = IR_LONG_PRESS_MAX;
	}
	sLongPressMs = (uint16_t) ms;
}
#endif

void IrReceiver_Init() {
#ifdef IR_CONTROL_ENABLE
	IrReceiver.begin(IRLED_PIN);
	loadMappings();
#endif
}

void IrReceiver_ReloadMappings() {
#ifdef IR_CONTROL_ENABLE
	loadMappings();
#endif
}

void IrReceiver_SetLearnMode(bool enable) {
#ifdef IR_CONTROL_ENABLE
	sLearnMode = enable;
	if (enable) {
		sLearnDeadline = millis() + IR_LEARN_TIMEOUT;
	}
#else
	(void) enable;
#endif
}

uint8_t IrReceiver_GetMappings(IrMapping *out, uint8_t maxCount) {
#ifdef IR_CONTROL_ENABLE
	uint8_t n = (sMapCount < maxCount) ? sMapCount : maxCount;
	for (uint8_t i = 0; i < n; i++) {
		out[i] = sMap[i];
	}
	return n;
#else
	(void) out;
	(void) maxCount;
	return 0;
#endif
}

uint16_t IrReceiver_GetLongPressMs() {
#ifdef IR_CONTROL_ENABLE
	return sLongPressMs;
#else
	return 0;
#endif
}

void IrReceiver_Cyclic() {
#ifdef IR_CONTROL_ENABLE
	// Auto-expire learn mode so a forgotten/aborted learn session can't suppress the remote forever.
	if (sLearnMode && ((int32_t) (millis() - sLearnDeadline) >= 0)) {
		sLearnMode = false;
	}

	// IR has no "button released" event - a held button simply stops sending frames. Once the frames
	// for a long-press candidate stop arriving, treat it as released: fire the short action unless the
	// long action already fired while it was held.
	if (sHoldActive && ((millis() - sHoldLastFrameMs) > IR_HOLD_RELEASE_MS)) {
		if (!sHoldLongFired) {
			Cmd_Action(sMap[sHoldIndex].cmd);
		}
		sHoldActive = false;
	}

	if (IrReceiver.decode()) {
		// Print a short summary of received data (serial-console learning still works as a fallback)
		IrReceiver.printIRResultShort(&Serial);
		Serial.println();
		IrReceiver.resume(); // Enable receiving of the next value

		const uint16_t command = IrReceiver.decodedIRData.command;
		const bool isRepeat = (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) != 0;

		if (sLearnMode) {
			// Learn mode: report the first genuine (non-repeat) press to the web UI, then disarm.
			if (!isRepeat) {
				sLearnMode = false;
				Log_Printf(LOGLEVEL_NOTICE, "RC: learned command 0x%X", command);
				Web_NotifyIrCode(command);
			}
			return;
		}

		// Look up the received command in the runtime mapping table.
		int16_t mi = -1;
		for (uint8_t i = 0; i < sMapCount; i++) {
			if (sMap[i].code == command) {
				mi = i;
				break;
			}
		}

		// A button with a long action uses hold detection: nothing fires on the first frame, the long
		// action fires once the hold passes the threshold, the short action fires on release (above).
		if ((mi >= 0) && (sMap[mi].longCmd != 0)) {
			if (sHoldActive && (sMap[sHoldIndex].code == command)) {
				// Still holding the same button.
				sHoldLastFrameMs = millis();
				if (!sHoldLongFired && ((millis() - sHoldStartMs) >= sLongPressMs)) {
					Cmd_Action(sMap[mi].longCmd);
					sHoldLongFired = true;
				}
			} else {
				// A new button started: finalize the previous candidate's short action first.
				if (sHoldActive && !sHoldLongFired) {
					Cmd_Action(sMap[sHoldIndex].cmd);
				}
				sHoldActive = true;
				sHoldIndex = (uint8_t) mi;
				sHoldStartMs = millis();
				sHoldLastFrameMs = millis();
				sHoldLongFired = false;
			}
			return;
		}

		// Plain button (no long action) or unknown code: a different mapped button means any pending
		// long-press candidate was released, so flush its short action now.
		if (sHoldActive && (mi >= 0)) {
			if (!sHoldLongFired) {
				Cmd_Action(sMap[sHoldIndex].cmd);
			}
			sHoldActive = false;
		}

		bool rcActionOk = false;
		if (millis() - IrReceiver_LastRcCmdTimestamp >= IR_DEBOUNCE) {
			rcActionOk = true; // not used for volume up/down
			IrReceiver_LastRcCmdTimestamp = millis();
		}

		if (mi >= 0) {
			const uint8_t cmd = sMap[mi].cmd;
			// Volume up/down fire on every frame (incl. repeats) so holding the button ramps.
			if ((cmd == CMD_VOLUMEUP) || (cmd == CMD_VOLUMEDOWN)) {
				Cmd_Action(cmd);
			} else if (rcActionOk) {
				Cmd_Action(cmd);
			}
			return;
		}
		if (rcActionOk) {
			Log_Println("RC: unknown", LOGLEVEL_NOTICE);
		}
	}
#endif
}
