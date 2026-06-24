#pragma once

#include <stdlib.h>
#include <vector>

// Custom allocator for PSRAM if available
template <typename T>
class PSRAMAllocator {
public:
	using value_type = T;

	PSRAMAllocator() = default;
	template <typename U>
	PSRAMAllocator(const PSRAMAllocator<U> &) { }

	T *allocate(size_t n) {
		if (psramFound()) {
			T *ptr = static_cast<T *>(ps_malloc(n * sizeof(T)));
			if (ptr) {
				return ptr;
			}
		}
		return static_cast<T *>(malloc(n * sizeof(T)));
	}

	void deallocate(T *ptr, size_t) {
		free(ptr);
	}
};

template <typename T, typename U>
bool operator==(const PSRAMAllocator<T> &, const PSRAMAllocator<U> &) {
	return true;
}
template <typename T, typename U>
bool operator!=(const PSRAMAllocator<T> &, const PSRAMAllocator<U> &) {
	return false;
}

using Playlist = std::vector<char *, PSRAMAllocator<char *>>;

// Allocate Playlist in PSRAM if available
inline Playlist *allocatePlaylist() {
	// Allocate raw storage from PSRAM (preferred) or the internal heap, then construct in place. Both paths
	// use a malloc-family allocation so freePlaylist() can always pair the explicit ~Playlist() with free();
	// pairing operator new with free() would be undefined behaviour.
	void *mem = nullptr;
	if (psramFound()) {
		mem = ps_malloc(sizeof(Playlist));
	}
	if (mem == nullptr) {
		mem = malloc(sizeof(Playlist)); // PSRAM absent or exhausted -> fall back to the internal heap
	}
	if (mem == nullptr) {
		return nullptr;
	}
	return new (mem) Playlist();
}

// Release previously allocated memory
inline void freePlaylist(Playlist *(&playlist)) {
	if (playlist == nullptr) {
		return;
	}
	for (auto e : *playlist) {
		free(e);
	}
	playlist->~Playlist(); // Call destructor explicitly
	free(playlist); // Use free instead of delete since it might be ps_malloc'd
	playlist = nullptr;
}
