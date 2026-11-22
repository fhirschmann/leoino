#include <Arduino.h>
#include "settings.h"

#include "Log.h"

#include "LogRingBuffer.h"
#include "MemX.h"

#include <new>

static LogRingBuffer *Log_RingBuffer = NULL;
static StaticSemaphore_t Log_RingMutexStorage;
static SemaphoreHandle_t Log_RingMutex = nullptr;

void Log_Init(void) {
	Serial.begin(115200);
	Log_RingMutex = xSemaphoreCreateMutexStatic(&Log_RingMutexStorage);
	// LogRingBuffer defines a malloc/PSRAM-backed class-specific operator new but no matching delete,
	// which makes constructor-failure cleanup undefined and triggers -Wmismatched-new-delete. Allocate
	// the storage explicitly, retain an internal-heap fallback, and construct it in place.
	void *storage = psramFound() ? ps_malloc(sizeof(LogRingBuffer)) : nullptr;
	if (storage == nullptr) {
		storage = malloc(sizeof(LogRingBuffer));
	}
	if (storage != nullptr) {
		Log_RingBuffer = ::new (storage) LogRingBuffer();
	}
}

static const char *getLoglevel(const uint8_t logLevel) {
	switch (logLevel) {
		case LOGLEVEL_ERROR:
			return "E";
		case LOGLEVEL_NOTICE:
			return "N";
		case LOGLEVEL_INFO:
			return "I";
		case LOGLEVEL_DEBUG:
			return "D";
		default:
			return " ";
	}
}

/* Wrapper-function for serial-logging (with newline)
   _logBuffer: char* to log
   _minLogLevel: loglevel configured for this message.
   If (SERIAL_LOGLEVEL <= _minLogLevel) message will be logged
*/
void Log_Println(const char *_logBuffer, const uint8_t _minLogLevel) {
	if (!_logBuffer) {
		return;
	}
	if (SERIAL_LOGLEVEL >= _minLogLevel) {
		uint32_t ctime = millis();
		const char *sLogLevel = getLoglevel(_minLogLevel);
		Serial.printf("%s [%" PRIu32 "] ", sLogLevel, ctime);
		Serial.println(_logBuffer);
		if (Log_RingBuffer != nullptr) {
			xSemaphoreTake(Log_RingMutex, portMAX_DELAY);
			Log_RingBuffer->printf("%s [%" PRIu32 "] ", sLogLevel, ctime);
			Log_RingBuffer->println(_logBuffer);
			xSemaphoreGive(Log_RingMutex);
		}
	}
}

/* Wrapper-function for serial-logging (without newline) */
void Log_Print(const char *_logBuffer, const uint8_t _minLogLevel, bool printTimestamp) {
	if (!_logBuffer) {
		return;
	}
	if (SERIAL_LOGLEVEL >= _minLogLevel) {
		const uint32_t ctime = millis();
		const char *sLogLevel = getLoglevel(_minLogLevel);
		if (printTimestamp) {
			Serial.printf("%s [%" PRIu32 "] ", sLogLevel, ctime);
			Serial.print(_logBuffer);
		} else {
			Serial.print(_logBuffer);
		}
		if (Log_RingBuffer != nullptr) {
			xSemaphoreTake(Log_RingMutex, portMAX_DELAY);
			if (printTimestamp) {
				Log_RingBuffer->printf("%s [%" PRIu32 "] ", sLogLevel, ctime);
			}
			Log_RingBuffer->print(_logBuffer);
			xSemaphoreGive(Log_RingMutex);
		}
	}
}

int Log_Printf(const uint8_t _minLogLevel, const char *format, ...) {
	if (!format || SERIAL_LOGLEVEL < _minLogLevel) {
		return 0;
	}
	char loc_buf[201]; // Allow a maximum buffer of 200 characters in a single log message

	va_list arg;
	va_start(arg, format);
	const int len = vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
	va_end(arg);
	if (len < 0) {
		return 0;
	}

	const bool truncated = static_cast<size_t>(len) > sizeof(loc_buf) - 1;
	const uint32_t ctime = millis();
	const char *sLogLevel = getLoglevel(_minLogLevel);
	Serial.printf("%s [%" PRIu32 "] ", sLogLevel, ctime);
	Serial.print(loc_buf);
	if (truncated) {
		Serial.print("...");
	}
	Serial.print("\n");

	if (Log_RingBuffer != nullptr) {
		xSemaphoreTake(Log_RingMutex, portMAX_DELAY);
		Log_RingBuffer->printf("%s [%" PRIu32 "] ", sLogLevel, ctime);
		Log_RingBuffer->print(loc_buf);
		if (truncated) {
			Log_RingBuffer->print("...");
		}
		Log_RingBuffer->print("\n");
		xSemaphoreGive(Log_RingMutex);
	}

	return std::min<int>(len, sizeof(loc_buf) - 1);
}

String Log_GetRingBuffer(void) {
	if (Log_RingBuffer == nullptr) {
		return String();
	}
	// LogRingBuffer is not thread-safe: its write increments the index before wrapping it.
	// Concurrent volume/audio/network logs can otherwise overwrite the index at the buffer end
	// and subsequently corrupt arbitrary memory. Readers must take the same lock as writers.
	xSemaphoreTake(Log_RingMutex, portMAX_DELAY);
	String result = Log_RingBuffer->get();
	xSemaphoreGive(Log_RingMutex);
	return result;
}
