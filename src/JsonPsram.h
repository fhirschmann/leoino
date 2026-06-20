#pragma once

#include <ArduinoJson.h>

// Allocator that places JSON documents in PSRAM instead of the small (~90 kB) internal heap.
// Large payloads (sync manifests, full RFID-tag lists) would otherwise hit NoMemory and fail
// to parse/serialize. Boards targeted by this firmware all carry PSRAM; ps_malloc falls back
// to the internal heap automatically when PSRAM is absent.
struct SpiRamAllocator : ArduinoJson::Allocator {
	void *allocate(size_t size) override {
		return ps_malloc(size);
	}
	void deallocate(void *pointer) override {
		free(pointer);
	}
	void *reallocate(void *ptr, size_t new_size) override {
		return ps_realloc(ptr, new_size);
	}
};
