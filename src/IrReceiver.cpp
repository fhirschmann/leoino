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

// Runtime mapping table (IR command -> CMD_* action). Seeded from the compiled-in RC_* defaults,
// persisted in NVS (key "irMap") and editable via the web UI.
static IrMapping sMap[IR_MAX_MAPPINGS];
static uint8_t sMapCount = 0;

// Learn mode: while armed, received codes are reported to the web UI instead of triggering actions.
static std::atomic<bool> sLearnMode = false;
static uint32_t sLearnDeadline = 0;
static constexpr uint32_t IR_LEARN_TIMEOUT = 30000; // ms; matches the web-UI learn modal timeout

static const char *irMapKey = "irMap";

// Default layout, used to seed the table on first boot (or after an NVS wipe) so an untouched device
// keeps the classic remote behaviour. Codes come from the board's RC_* defines (settings-*.h).
static void seedDefaultMappings() {
	sMapCount = 0;
	auto add = [](uint16_t code, uint8_t cmd) {
		if (sMapCount < IR_MAX_MAPPINGS) {
			sMap[sMapCount].code = code;
			sMap[sMapCount].cmd = cmd;
			sMapCount++;
		}
	};
	#ifdef RC_PLAY
	add(RC_PLAY, CMD_PLAYPAUSE);
	#endif
	#ifdef RC_PAUSE
	add(RC_PAUSE, CMD_PLAYPAUSE);
	#endif
	#ifdef RC_NEXT
	add(RC_NEXT, CMD_NEXTTRACK);
	#endif
	#ifdef RC_PREVIOUS
	add(RC_PREVIOUS, CMD_PREVTRACK);
	#endif
	#ifdef RC_FIRST
	add(RC_FIRST, CMD_FIRSTTRACK);
	#endif
	#ifdef RC_LAST
	add(RC_LAST, CMD_LASTTRACK);
	#endif
	#ifdef RC_VOL_UP
	add(RC_VOL_UP, CMD_VOLUMEUP);
	#endif
	#ifdef RC_VOL_DOWN
	add(RC_VOL_DOWN, CMD_VOLUMEDOWN);
	#endif
	#ifdef RC_MUTE
	add(RC_MUTE, CMD_MUTE);
	#endif
	#ifdef RC_SHUTDOWN
	add(RC_SHUTDOWN, CMD_SLEEPMODE);
	#endif
	#ifdef RC_BLUETOOTH
	add(RC_BLUETOOTH, CMD_TOGGLE_BLUETOOTH_SINK_MODE);
	#endif
	#ifdef RC_FTP
	add(RC_FTP, CMD_ENABLE_FTP_SERVER);
	#endif
}

static void loadMappings() {
	size_t blobLen = gPrefsSettings.getBytesLength(irMapKey);
	if ((blobLen >= sizeof(IrMapping)) && ((blobLen % sizeof(IrMapping)) == 0)) {
		uint8_t count = blobLen / sizeof(IrMapping);
		if (count > IR_MAX_MAPPINGS) {
			count = IR_MAX_MAPPINGS;
		}
		gPrefsSettings.getBytes(irMapKey, sMap, (size_t) count * sizeof(IrMapping));
		sMapCount = count;
	} else {
		// nothing (valid) stored yet -> fall back to the compiled-in default layout
		seedDefaultMappings();
	}
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

void IrReceiver_Cyclic() {
#ifdef IR_CONTROL_ENABLE
	// Auto-expire learn mode so a forgotten/aborted learn session can't suppress the remote forever.
	if (sLearnMode && ((int32_t) (millis() - sLearnDeadline) >= 0)) {
		sLearnMode = false;
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

		bool rcActionOk = false;
		if (millis() - IrReceiver_LastRcCmdTimestamp >= IR_DEBOUNCE) {
			rcActionOk = true; // not used for volume up/down
			IrReceiver_LastRcCmdTimestamp = millis();
		}

		// Look up the received command in the runtime mapping table and dispatch the mapped action.
		for (uint8_t i = 0; i < sMapCount; i++) {
			if (sMap[i].code == command) {
				const uint8_t cmd = sMap[i].cmd;
				// Volume up/down fire on every frame (incl. repeats) so holding the button ramps.
				if ((cmd == CMD_VOLUMEUP) || (cmd == CMD_VOLUMEDOWN)) {
					Cmd_Action(cmd);
				} else if (rcActionOk) {
					Cmd_Action(cmd);
				}
				return;
			}
		}
		if (rcActionOk) {
			Log_Println("RC: unknown", LOGLEVEL_NOTICE);
		}
	}
#endif
}
