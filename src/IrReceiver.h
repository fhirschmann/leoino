#pragma once

#include <stdint.h>

// Maximum number of IR-code -> command mappings kept in RAM / NVS.
#define IR_MAX_MAPPINGS 32

// One learned remote button: the raw IR command and the CMD_* action it triggers (see values.h).
struct __attribute__((packed)) IrMapping {
	uint16_t code; // decodedIRData.command of the remote button
	uint8_t cmd; // CMD_* action to run when this code is received
};

void IrReceiver_Init();
void IrReceiver_Cyclic();

// Web-UI configuration support
void IrReceiver_ReloadMappings(); // re-read the mapping table from NVS (after a save via the web UI)
void IrReceiver_SetLearnMode(bool enable); // arm/disarm learn mode (auto-expires after a timeout)
uint8_t IrReceiver_GetMappings(IrMapping *out, uint8_t maxCount); // copy current table out; returns count
