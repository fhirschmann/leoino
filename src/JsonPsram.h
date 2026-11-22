#pragma once

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Allocator that places JSON documents in PSRAM instead of the scarce internal heap, while
// automatically falling back to default 8-bit heap if PSRAM is absent or exhausted (note that
// Arduino-ESP32's raw ps_malloc() returns NULL without fallback when PSRAM is absent).
struct SpiRamAllocator : ArduinoJson::Allocator {
	void *allocate(size_t size) override {
		return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
	}
	void deallocate(void *pointer) override {
		free(pointer);
	}
	void *reallocate(void *ptr, size_t new_size) override {
		return heap_caps_realloc_prefer(ptr, new_size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
	}
};
