#include <Arduino.h>

#include "MemX.h"

#include <esp_heap_caps.h>
#include <limits.h>
#include <string.h>

// Wraps strdup(). Prefers PSRAM when available and falls back to internal heap automatically.
char *x_strdup(const char *_str) {
	if (!_str) {
		return nullptr;
	}
	size_t len = strlen(_str) + 1;
	char *dst = static_cast<char *>(x_malloc(len));
	if (!dst) {
		return nullptr;
	}
	memcpy(dst, _str, len);
	return dst;
}

// Prefers PSRAM when available and falls back to internal 8-bit RAM when PSRAM is absent or full.
void *x_malloc(uint32_t _allocSize) {
	if (_allocSize == 0) {
		_allocSize = 1;
	}
	return heap_caps_malloc_prefer(_allocSize, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
}

void *x_realloc(void *_ptr, size_t _newSize) {
	return heap_caps_realloc_prefer(_ptr, _newSize, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
}

// Prefers PSRAM when available and falls back to internal 8-bit RAM when PSRAM is absent or full.
char *x_calloc(uint32_t _allocSize, uint32_t _unitSize) {
	if (_unitSize != 0 && _allocSize > (UINT32_MAX / _unitSize)) {
		return nullptr;
	}
	return static_cast<char *>(heap_caps_calloc_prefer(_allocSize, _unitSize, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT));
}
