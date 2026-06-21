#pragma once

#include <Arduino.h>

#include <stdarg.h>

// Thread-safe fixed-size status-message slot shared by the background tasks (Sync, Backup, GitHub
// OTA): the task (core 1) writes the current step/result, the web server (core 0) reads it for the
// progress UI / MQTT. A short spinlock guards the buffer so a reader can never see a half-written
// string (which would otherwise show up as a brief garbage line in the progress UI). The buffer is
// a plain fixed array (no heap) to fit the heap-constrained ESP32, and the critical section stays
// as short as possible (a single memcpy, no formatting/IO inside the lock).
struct StatusMessage {
	static constexpr size_t Capacity = 96;
	char buf[Capacity] = "";
	portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

	void set(const char *msg) {
		char tmp[Capacity];
		snprintf(tmp, sizeof(tmp), "%s", msg ? msg : "");
		taskENTER_CRITICAL(&mux);
		memcpy(buf, tmp, sizeof(buf));
		taskEXIT_CRITICAL(&mux);
	}

	void setf(const char *fmt, ...) {
		char tmp[Capacity];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(tmp, sizeof(tmp), fmt, ap);
		va_end(ap);
		taskENTER_CRITICAL(&mux);
		memcpy(buf, tmp, sizeof(buf));
		taskEXIT_CRITICAL(&mux);
	}

	// Copy the current message into a caller-provided buffer (NUL-terminated, clamped to dstLen).
	void copy(char *dst, size_t dstLen) {
		if (!dst || dstLen == 0) {
			return;
		}
		taskENTER_CRITICAL(&mux);
		size_t n = strnlen(buf, sizeof(buf) - 1);
		if (n >= dstLen) {
			n = dstLen - 1;
		}
		memcpy(dst, buf, n);
		taskEXIT_CRITICAL(&mux);
		dst[n] = '\0';
	}
};
