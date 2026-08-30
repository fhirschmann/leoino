#pragma once

#include <Arduino.h>

struct I2cSupervisorStatus {
	bool started = false;
	bool sdaHigh = true;
	bool sclHigh = true;
	bool initialSdaLow = false;
	bool initialSclLow = false;
	uint8_t lastClockPulses = 0;
	uint32_t recoveryAttempts = 0;
	uint32_t recoverySuccesses = 0;
};

// Starts the shared secondary I2C bus after first releasing a possibly stuck slave. Recovery only
// drives lines LOW (open drain); HIGH is always obtained by releasing the line to its pull-up.
bool I2cSupervisor_Begin(void);
bool I2cSupervisor_Recover(const char *reason);
const I2cSupervisorStatus &I2cSupervisor_GetStatus(void);
