#include <Arduino.h>
#include "settings.h"

#include "I2cSupervisor.h"

#include "Log.h"
#include "System.h"

#ifdef I2C_2_ENABLE
	#include <Wire.h>

extern TwoWire i2cBusTwo;

namespace {
constexpr uint32_t i2cClockHz = 100000UL;
constexpr uint16_t i2cTransactionTimeoutMs = 25;
I2cSupervisorStatus s_status;

void releaseLine(uint8_t pin) {
	pinMode(pin, INPUT_PULLUP);
}

void driveLineLow(uint8_t pin) {
	digitalWrite(pin, LOW);
	pinMode(pin, OUTPUT_OPEN_DRAIN);
}

bool waitForLineHigh(uint8_t pin, uint32_t timeoutUs) {
	const uint32_t startedAt = micros();
	while (!digitalRead(pin) && (micros() - startedAt < timeoutUs)) {
		delayMicroseconds(10);
	}
	return digitalRead(pin);
}

// UM10204 bus-clear procedure: give a slave up to nine clocks to release SDA, then synthesize a
// STOP. Never drive a line HIGH, because another device may legitimately be holding it LOW.
uint8_t clearBusLines(void) {
	releaseLine(ext_IIC_DATA);
	releaseLine(ext_IIC_CLK);
	delayMicroseconds(10);

	// A slave may be stretching SCL briefly. Wait first; if it stays LOW there is no safe clock we
	// can generate, so leave both lines released and let the caller retry later.
	if (!waitForLineHigh(ext_IIC_CLK, 10000)) {
		s_status.sdaHigh = digitalRead(ext_IIC_DATA);
		s_status.sclHigh = false;
		return 0;
	}

	uint8_t pulses = 0;
	while (!digitalRead(ext_IIC_DATA) && pulses < 9) {
		driveLineLow(ext_IIC_CLK);
		delayMicroseconds(5);
		releaseLine(ext_IIC_CLK);
		if (!waitForLineHigh(ext_IIC_CLK, 1000)) {
			break;
		}
		delayMicroseconds(5);
		pulses++;
	}

	// Generate STOP (SDA low -> release SCL -> release SDA) if SCL can be released. Open-drain
	// drive makes this harmless even if a slave has not let go yet.
	driveLineLow(ext_IIC_DATA);
	delayMicroseconds(5);
	releaseLine(ext_IIC_CLK);
	if (waitForLineHigh(ext_IIC_CLK, 1000)) {
		delayMicroseconds(5);
		releaseLine(ext_IIC_DATA);
		delayMicroseconds(5);
	} else {
		releaseLine(ext_IIC_DATA);
	}

	s_status.sdaHigh = digitalRead(ext_IIC_DATA);
	s_status.sclHigh = digitalRead(ext_IIC_CLK);
	return pulses;
}

bool startController(void) {
	const bool started = i2cBusTwo.begin(ext_IIC_DATA, ext_IIC_CLK, i2cClockHz);
	if (started) {
		i2cBusTwo.setTimeOut(i2cTransactionTimeoutMs);
	}
	s_status.started = started;
	return started;
}
} // namespace
#else
namespace {
I2cSupervisorStatus s_status;
} // namespace
#endif

bool I2cSupervisor_Begin(void) {
#ifdef I2C_2_ENABLE
	I2cBusTwo_Lock();
	releaseLine(ext_IIC_DATA);
	releaseLine(ext_IIC_CLK);
	delayMicroseconds(10);
	s_status.initialSdaLow = !digitalRead(ext_IIC_DATA);
	s_status.initialSclLow = !digitalRead(ext_IIC_CLK);
	s_status.lastClockPulses = clearBusLines();
	const bool started = startController();
	I2cBusTwo_Unlock();

	if (!started || !s_status.sdaHigh || !s_status.sclHigh) {
		Log_Printf(LOGLEVEL_ERROR, "I2C supervisor: start=%u SDA=%u SCL=%u pulses=%u", started, s_status.sdaHigh, s_status.sclHigh, s_status.lastClockPulses);
	} else if (s_status.initialSdaLow || s_status.initialSclLow) {
		Log_Printf(LOGLEVEL_NOTICE, "I2C supervisor cleared startup bus (SDA-low=%u SCL-low=%u pulses=%u)", s_status.initialSdaLow, s_status.initialSclLow, s_status.lastClockPulses);
	} else {
		Log_Println("I2C supervisor: bus ready", LOGLEVEL_DEBUG);
	}
	return started && s_status.sdaHigh && s_status.sclHigh;
#else
	s_status.started = true;
	return true;
#endif
}

bool I2cSupervisor_Recover(const char *reason) {
#ifdef I2C_2_ENABLE
	s_status.recoveryAttempts++;
	I2cBusTwo_Lock();
	if (s_status.started) {
		i2cBusTwo.end();
		s_status.started = false;
		delay(2);
	}
	s_status.lastClockPulses = clearBusLines();
	const bool started = startController();
	I2cBusTwo_Unlock();

	const bool recovered = started && s_status.sdaHigh && s_status.sclHigh;
	if (recovered) {
		s_status.recoverySuccesses++;
		Log_Printf(LOGLEVEL_NOTICE, "I2C recovery succeeded (%s, pulses=%u)", reason ? reason : "unspecified", s_status.lastClockPulses);
	} else {
		Log_Printf(LOGLEVEL_ERROR, "I2C recovery failed (%s, start=%u SDA=%u SCL=%u pulses=%u)", reason ? reason : "unspecified", started, s_status.sdaHigh, s_status.sclHigh, s_status.lastClockPulses);
	}
	return recovered;
#else
	(void) reason;
	return true;
#endif
}

const I2cSupervisorStatus &I2cSupervisor_GetStatus(void) {
	return s_status;
}
