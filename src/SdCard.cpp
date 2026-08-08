#include <Arduino.h>
#include "settings.h"

#include "SdCard.h"

#include "Common.h"
#include "Led.h"
#include "Log.h"
#include "MemX.h"
#include "System.h"

#include <esp_random.h>
#include <esp_vfs_fat.h>

#ifdef SD_MMC_1BIT_MODE
	#define HARDWARE_FS SD_MMC
#else
SPIClass spiSD(HSPI);
	#define HARDWARE_FS SD
#endif
SanitizedFS gFSystem(HARDWARE_FS);

uint8_t maxRecursionDepth;

void SdCard_Init(void) {
#ifdef NO_SDCARD
	// Initialize without any SD card, e.g. for webplayer only
	Log_Println("Init without SD card ", LOGLEVEL_NOTICE);
	return;
#endif

#ifndef SINGLE_SPI_ENABLE
	#ifdef SD_MMC_1BIT_MODE
	pinMode(2, INPUT_PULLUP);
	while (!SD_MMC.begin("/sdcard", true)) {
	#else
	pinMode(SPISD_CS, OUTPUT);
	digitalWrite(SPISD_CS, HIGH);
	spiSD.begin(SPISD_SCK, SPISD_MISO, SPISD_MOSI, SPISD_CS);
	spiSD.setFrequency(1000000);
	while (!SD.begin(SPISD_CS, spiSD)) {
	#endif
#else
	#ifdef SD_MMC_1BIT_MODE
	pinMode(2, INPUT_PULLUP);
	while (!SD_MMC.begin("/sdcard", true)) {
	#else
	while (!SD.begin(SPISD_CS)) {
	#endif
#endif
		Log_Println(unableToMountSd, LOGLEVEL_ERROR);
		delay(500);
#ifdef SHUTDOWN_IF_SD_BOOT_FAILS
		if (millis() >= deepsleepTimeAfterBootFails * 1000) {
			Log_Println(sdBootFailedDeepsleep, LOGLEVEL_ERROR);
			Led_Exit();
			esp_deep_sleep_start();
		}
#endif
	}

	// Used when building recursive playlists
	maxRecursionDepth = gPrefsSettings.getUInt("nvsRecDepth", 255);
	if (maxRecursionDepth == 255) {
		gPrefsSettings.putUInt("nvsRecDepth", 2);
		maxRecursionDepth = 2;
	}

	// Prime FatFs' free-cluster count now: with a stale FSINFO sector the first f_getfree
	// scans the whole FAT under the filesystem mutex (multi-second on a large card). Doing
	// it here, before any playback exists, means the first Info-tab open in the web UI
	// can't stall SD access mid-listening.
	SdCard_GetFreeSize();
}

void SdCard_Exit(void) {
// SD card goto idle mode
#ifdef SD_MMC_1BIT_MODE
	Log_Println("shutdown SD card (SD_MMC)..", LOGLEVEL_NOTICE);
	SD_MMC.end();
#else
	Log_Println("shutdown SD card (SPI)..", LOGLEVEL_NOTICE);
	SD.end();
#endif
}

#ifdef SD_MMC_1BIT_MODE
// The Arduino SDMMCFS keeps its sdmmc_card_t* protected. esp_vfs_fat_sdcard_format() needs that
// handle, so a no-data-member subclass exposes it without patching the core (layout-compatible).
namespace {
struct SdMmcCardAccessor : public fs::SDMMCFS {
	sdmmc_card_t *card() const {
		return _card;
	}
};
} // namespace
#endif

// Reformats the mounted SD card with a fresh FAT filesystem. ERASES ALL DATA. The card stays
// mounted afterwards, so no reboot is needed. Only supported in SD_MMC mode.
bool SdCard_Format(void) {
#if defined(NO_SDCARD)
	Log_Println("SD format: no SD card configured", LOGLEVEL_ERROR);
	return false;
#elif defined(SD_MMC_1BIT_MODE)
	sdmmc_card_t *card = static_cast<SdMmcCardAccessor &>(SD_MMC).card();
	if (card == nullptr) {
		Log_Println("SD format: no card handle available", LOGLEVEL_ERROR);
		return false;
	}
	Log_Println("SD format: reformatting card -- all data will be erased..", LOGLEVEL_NOTICE);
	const esp_err_t err = esp_vfs_fat_sdcard_format("/sdcard", card);
	if (err != ESP_OK) {
		Log_Printf(LOGLEVEL_ERROR, "SD format: failed (%s)", esp_err_to_name(err));
		return false;
	}
	Log_Println("SD format: done", LOGLEVEL_NOTICE);
	return true;
#else
	Log_Println("SD format: only supported in SD_MMC mode", LOGLEVEL_ERROR);
	return false;
#endif
}

sdcard_type_t SdCard_GetType(void) {
	sdcard_type_t cardType;
#ifdef SD_MMC_1BIT_MODE
	Log_Println(sdMountedMmc1BitMode, LOGLEVEL_NOTICE);
	cardType = SD_MMC.cardType();
#else
	Log_Println(sdMountedSpiMode, LOGLEVEL_NOTICE);
	cardType = SD.cardType();
#endif
	return cardType;
}

uint64_t SdCard_GetSize() {
#ifdef SD_MMC_1BIT_MODE
	return SD_MMC.cardSize();
#else
	return SD.cardSize();
#endif
}

uint64_t SdCard_GetFreeSize() {
#ifdef SD_MMC_1BIT_MODE
	return SD_MMC.cardSize() - SD_MMC.usedBytes();
#else
	return SD.cardSize() - SD.usedBytes();
#endif
}

uint8_t SdCard_GetMaxRecursionDepth(void) {
	return maxRecursionDepth;
}

// Returns recursion depth that's used then playlists are generated for recursive playmodes
size_t SdCard_SetMaxRecursionDepth(uint8_t _maxRecursionDepth) {
	maxRecursionDepth = _maxRecursionDepth;
	return gPrefsSettings.putUInt("nvsRecDepth", SdCard_GetMaxRecursionDepth());
}

void SdCard_PrintInfo() {
	// show SD card type
	sdcard_type_t cardType = SdCard_GetType();
	const char *type = "UNKNOWN";
	switch (cardType) {
		case CARD_MMC:
			type = "MMC";
			break;

		case CARD_SD:
			type = "SDSC";
			break;

		case CARD_SDHC:
			type = "SDHC";
			break;

		default:
			break;
	}
	Log_Printf(LOGLEVEL_DEBUG, "SD card type: %s", type);
	// show SD card size / free space
	uint64_t cardSize = SdCard_GetSize() / (1024 * 1024);
	uint64_t freeSize = SdCard_GetFreeSize() / (1024 * 1024);
	;
	Log_Printf(LOGLEVEL_NOTICE, sdInfo, cardSize, freeSize);
}

// Check if file-type is correct
bool fileValid(const char *_fileItem) {
	// clang-format off
	// all supported extension
	constexpr std::array audioFileSufix = {
		".mp3",
		".aac",
		".m4a",
		".wav",
		".flac",
		".ogg",
		".oga",
		".opus",
		// playlists
		".m3u",
		".m3u8",
		".pls",
		".asx"
	};
	// clang-format on
	constexpr size_t maxExtLen = strlen(*std::max_element(audioFileSufix.begin(), audioFileSufix.end(), [](const char *a, const char *b) {
		return strlen(a) < strlen(b);
	}));

	if (!_fileItem || !strlen(_fileItem)) {
		// invalid entry
		return false;
	}

	// check for streams
	if (strncasecmp(_fileItem, "http://", strlen("http://")) == 0 || strncasecmp(_fileItem, "https://", strlen("https://")) == 0) {
		// this is a stream
		return true;
	}

	// check for files which start with "/."
	const char *lastSlashPtr = strrchr(_fileItem, '/');
	if (lastSlashPtr == nullptr) {
		// we have a relative filename without any slashes...
		// set the pointer so that it points to the first character AFTER a +1
		lastSlashPtr = _fileItem - 1;
	}
	if (*(lastSlashPtr + 1) == '.') {
		// we have a hidden file
		// Log_Printf(LOGLEVEL_DEBUG, "File is hidden: %s", _fileItem);
		return false;
	}

	// extract the file extension
	const char *extStartPtr = strrchr(_fileItem, '.');
	if (extStartPtr == nullptr) {
		// no extension found
		// Log_Printf(LOGLEVEL_DEBUG, "File has no extension: %s", _fileItem);
		return false;
	}
	const size_t extLen = strlen(extStartPtr);
	if (extLen > maxExtLen) {
		// extension too long, we do not care anymore
		// Log_Printf(LOGLEVEL_DEBUG, "File not supported (extension to long): %s", _fileItem);
		return false;
	}
	char extBuffer[maxExtLen + 1] = {0};
	memcpy(extBuffer, extStartPtr, extLen);

	// make the extension lower case (without using non standard C functions)
	for (size_t i = 0; i < extLen; i++) {
		extBuffer[i] = tolower(extBuffer[i]);
	}

	// check extension against all supported values
	for (const auto &e : audioFileSufix) {
		if (strcmp(extBuffer, e) == 0) {
			// hit we found the extension
			return true;
		}
	}
	// miss, we did not find the extension
	// Log_Printf(LOGLEVEL_DEBUG, "File not supported: %s", _fileItem);
	return false;
}

// Takes a directory as input and returns a random subdirectory from it
const String SdCard_pickRandomSubdirectory(const char *_directory) {
	// Look if folder requested really exists and is a folder. If not => break.
	File directory = gFSystem.open(_directory);
	if (!directory || !directory.isDirectory()) {
		Log_Printf(LOGLEVEL_ERROR, dirOrFileDoesNotExist, _directory);
		return String();
	}
	Log_Printf(LOGLEVEL_NOTICE, tryToPickRandomDir, _directory);

	// iterate through and count all dirs
	size_t dirCount = 0;
	while (1) {
		bool isDir;
		const String name = gFSystem.nextFileName(directory, &isDir);
		if (name.isEmpty()) {
			break;
		}
		if (isDir) {
			dirCount++;
		}
	}
	if (!dirCount) {
		// no paths in folder
		directory.close();
		return String();
	}

	const uint32_t randomNumber = esp_random() % dirCount;
	directory.rewindDirectory();
	dirCount = 0;
	while (1) {
		bool isDir;
		const String name = gFSystem.nextFileName(directory, &isDir);
		if (name.isEmpty()) {
			break;
		}
		if (isDir) {
			if (dirCount == randomNumber) {
				directory.close();
				return name;
			}
			dirCount++;
		}
	}

	directory.close();
	// if we reached here, something went wrong
	return String();
}

static bool SdCard_allocAndSave(Playlist *playlist, const String &s) {
	const size_t len = s.length() + 1;
	char *entry = static_cast<char *>(x_malloc(len));
	if (!entry) {
		Log_Println(unableToAllocateMemForLinearPlaylist, LOGLEVEL_ERROR);
		return false;
	}
	s.toCharArray(entry, len);
	playlist->push_back(entry);
	return true;
};

static bool SdCard_IsWebstream(const String &entry) {
	String scheme = entry.substring(0, std::min((size_t) 8, (size_t) entry.length()));
	scheme.toLowerCase();
	return scheme.startsWith("http://") || scheme.startsWith("https://");
}

// Resolve a local M3U entry against the playlist's folder and collapse '.'/'..' segments. Relative
// entries are common in playlists copied from a PC; passing them straight to FS::open() made them
// resolve against the SD root (or fail), while unchecked '..' segments could escape the SD root.
static bool SdCard_ResolveM3UPath(String entry, const String &baseDir, String &resolved) {
	entry.replace('\\', '/');
	String lowerPrefix = entry.substring(0, std::min((size_t) 8, (size_t) entry.length()));
	lowerPrefix.toLowerCase();
	if (lowerPrefix.startsWith("file://")) {
		entry.remove(0, 7);
	}
	String combined = entry.startsWith("/") ? entry : baseDir + "/" + entry;
	resolved = "";
	size_t cursor = 0;
	while (cursor <= combined.length()) {
		const int slash = combined.indexOf('/', cursor);
		const size_t end = slash < 0 ? combined.length() : (size_t) slash;
		const String segment = combined.substring(cursor, end);
		if (segment == "..") {
			const int previousSlash = resolved.lastIndexOf('/');
			if (previousSlash < 0) {
				return false;
			}
			resolved.remove(previousSlash);
		} else if (segment.length() > 0 && segment != ".") {
			resolved += "/";
			resolved += segment;
		}
		if (slash < 0) {
			break;
		}
		cursor = end + 1;
	}
	return !resolved.isEmpty();
}

static std::optional<Playlist *> SdCard_ParseM3UPlaylist(File file) {
	Playlist *playlist = allocatePlaylist();
	if (!playlist) {
		Log_Println(unableToAllocateMemForLinearPlaylist, LOGLEVEL_ERROR);
		return std::nullopt;
	}

	// reserve a sane amount of memory to reduce heap fragmentation
	playlist->reserve(64);
	// normal m3u is just a bunch of filenames, 1 / line
	// extended m3u file format can also include comments or special directives, prefaced by the "#" character
	// -> ignore all lines starting with '#'

	static constexpr size_t maxLineLength = 2048;
	static constexpr size_t maxEntries = 4096;
	const String playlistPath = gFSystem.path(file);
	const int lastSlash = playlistPath.lastIndexOf('/');
	const String baseDir = lastSlash > 0 ? playlistPath.substring(0, lastSlash) : "/";
	String line;
	if (!line.reserve(256)) {
		freePlaylist(playlist);
		return std::nullopt;
	}
	bool lineTooLong = false;
	bool firstLine = true;
	bool parseFailed = false;
	auto appendLine = [&]() {
		if (lineTooLong) {
			Log_Println("M3U: skipped over-long line", LOGLEVEL_ERROR);
			line = "";
			lineTooLong = false;
			firstLine = false;
			return;
		}
		if (firstLine && line.length() >= 3 && (uint8_t) line[0] == 0xEF && (uint8_t) line[1] == 0xBB && (uint8_t) line[2] == 0xBF) {
			line.remove(0, 3); // UTF-8 BOM before #EXTM3U
		}
		firstLine = false;
		line.trim();
		if (line.isEmpty() || line.startsWith("#")) {
			line = "";
			return;
		}
		if (playlist->size() >= maxEntries) {
			Log_Println("M3U: too many entries", LOGLEVEL_ERROR);
			parseFailed = true;
			return;
		}
		String entry;
		if (SdCard_IsWebstream(line)) {
			entry = line;
		} else if (!SdCard_ResolveM3UPath(line, baseDir, entry)) {
			Log_Printf(LOGLEVEL_ERROR, "M3U: rejected unsafe path %s", line.c_str());
			line = "";
			return;
		}
		if (!fileValid(entry.c_str())) {
			Log_Printf(LOGLEVEL_ERROR, "M3U: skipped unsupported entry %s", entry.c_str());
			line = "";
			return;
		}
		if (!SdCard_allocAndSave(playlist, entry)) {
			parseFailed = true;
			return;
		}
		line = "";
	};

	while (file.available() && !parseFailed) {
		const int value = file.read();
		if (value == '\n') {
			appendLine();
		} else if (value >= 0 && value != '\r') {
			if (line.length() < maxLineLength) {
				line += (char) value;
			} else {
				lineTooLong = true;
			}
		}
	}
	if (!parseFailed && (line.length() > 0 || lineTooLong)) {
		appendLine();
	}
	if (parseFailed) {
		freePlaylist(playlist);
		return std::nullopt;
	}

	// resize std::vector memory to fit our count
	playlist->shrink_to_fit();
	return playlist;
}

static bool SdCard_AppendDirectory(File &directory, Playlist *playlist, const uint8_t maxDepth, const uint8_t currentDepth, size_t &hiddenFiles) {
	while (true) {
		bool isDir = false;
		const String name = gFSystem.nextFileName(directory, &isDir);
		if (name.isEmpty()) {
			break;
		}
		if (isDir) {
			if (currentDepth < maxDepth) {
				File child = gFSystem.open(name);
				if (!child || !child.isDirectory() || !SdCard_AppendDirectory(child, playlist, maxDepth, currentDepth + 1, hiddenFiles)) {
					child.close();
					return false;
				}
				child.close();
			}
			continue; // a directory whose name ends in .mp3 is never an audio track
		}
		if (fileValid(name.c_str())) {
			if (!SdCard_allocAndSave(playlist, name)) {
				return false;
			}
		} else {
			hiddenFiles++;
		}
	}
	return true;
}

/* Puts SD-file(s) or directory into a playlist
	First element of array always contains the number of payload-items. */
std::optional<Playlist *> SdCard_ReturnPlaylist(const char *fileName, const uint32_t _playMode, const uint8_t _maxRecursionDepth, bool _recursionMode) {
	(void) _recursionMode; // retained for source compatibility; recursion now carries explicit state
	// Look if file/folder requested really exists. If not => break.
	File fileOrDirectory = gFSystem.open(fileName);
	if (!fileOrDirectory) {
		Log_Printf(LOGLEVEL_ERROR, dirOrFileDoesNotExist, fileName);
		return std::nullopt;
	}

	// Parse m3u-playlist and create linear-playlist out of it
	if (_playMode == LOCAL_M3U) {
		String lowerName = fileName;
		lowerName.toLowerCase();
		if (fileOrDirectory.isDirectory() || (!lowerName.endsWith(".m3u") && !lowerName.endsWith(".m3u8"))) {
			fileOrDirectory.close();
			Log_Printf(LOGLEVEL_ERROR, "M3U: invalid playlist path %s", fileName);
			return std::nullopt;
		}
		// Empty playlists intentionally parse to an empty vector; the player reports them instead of
		// trying to decode the .m3u file itself as audio.
		return SdCard_ParseM3UPlaylist(fileOrDirectory);
	}

	// if we reach this code, it was not a m3u

	Log_Printf(LOGLEVEL_DEBUG, freeMemory, ESP.getFreeHeap());
	Playlist *playlist = allocatePlaylist();
	if (!playlist) {
		fileOrDirectory.close();
		Log_Println(unableToAllocateMemForLinearPlaylist, LOGLEVEL_ERROR);
		return std::nullopt;
	}
	Log_Printf(LOGLEVEL_NOTICE, playlistRecDepth, _maxRecursionDepth);

	// File-mode
	if (!fileOrDirectory.isDirectory()) {
		if (!SdCard_allocAndSave(playlist, gFSystem.path(fileOrDirectory))) {
			fileOrDirectory.close();
			freePlaylist(playlist);
			return std::nullopt;
		}
		fileOrDirectory.close();
		return playlist;
	}

	// Directory-mode (linear-playlist)
	playlist->reserve(64); // reserve a sane amount of memory to reduce the number of reallocs
	size_t hiddenFiles = 0;
	if (!SdCard_AppendDirectory(fileOrDirectory, playlist, _maxRecursionDepth, 0, hiddenFiles)) {
		fileOrDirectory.close();
		freePlaylist(playlist);
		return std::nullopt;
	}
	playlist->shrink_to_fit();

	Log_Printf(LOGLEVEL_NOTICE, numberOfValidFiles, playlist->size());
	Log_Printf(LOGLEVEL_DEBUG, "Hidden files: %u", hiddenFiles);
	fileOrDirectory.close();

	return playlist;
}

// Extracts basepath out of a given filepath
std::string_view SdCard_Basepath(const char *filepath) {
	if (!filepath) {
		return std::string_view();
	}
	std::string_view str(filepath);
	auto pos = str.find_last_of('/');
	if (pos == std::string::npos) {
		return std::string_view();
	}
	return str.substr(0, pos + 1);
}

// Used for recursive playmodes. Allows to jump forwards and backwards between folders using
// CMD_PREVFOLDER (backwards) and CMD_NEXTFOLDER (forwards) to previous / next folder in playlist.
// Returns -1 if no prev or next folder was found or no playlist is available
// Returns >=0 if folderjump is possible. Number represents the index of the current playlist's track to jump to.
int16_t SdCard_findNextOrPrevDirectoryTrack(const Playlist &_playlist, size_t currentTrackIndexInPlaylist, SearchDirection direction) {
	// Look if index requested is out of bounds
	if (currentTrackIndexInPlaylist >= _playlist.size()) {
		return -1;
	}

	std::string_view basepathOfCurrentTrack = SdCard_Basepath(_playlist[currentTrackIndexInPlaylist]); // Get basepath of current track

	// Look forwards
	if (direction == SearchDirection::Forward) {
		if (_playlist[currentTrackIndexInPlaylist] != nullptr) {
			for (uint16_t i = (currentTrackIndexInPlaylist + 1); i < _playlist.size(); ++i) { // Iterate through playlist and start with current track +1
				std::string_view basepathOfTrackToLookUp = SdCard_Basepath(_playlist[i]);
				if (basepathOfTrackToLookUp != basepathOfCurrentTrack) {
					Log_Printf(LOGLEVEL_DEBUG, jumpForwardsToFolder, basepathOfTrackToLookUp.data(), "\n");
					return i; // Return first track after basepath change
				}
			}
		} else {
			return -1;
		}

		// Look backwards
	} else if (direction == SearchDirection::Backward) {
		//  Go back as long as we don't hit 0
		if (!currentTrackIndexInPlaylist) {
			return currentTrackIndexInPlaylist;
		}

		if (_playlist[currentTrackIndexInPlaylist] != nullptr) {
			for (uint16_t i = (currentTrackIndexInPlaylist - 1); i > 0; i--) {
				std::string_view basepathOfTrackToLookUp = SdCard_Basepath(_playlist[i]);
				if (basepathOfTrackToLookUp != basepathOfCurrentTrack) { // Look for the 1st basepath change...
					for (uint16_t j = i - 1; j > 0; j--) {
						std::string_view basepathOfTrackToLookUpInner = SdCard_Basepath(_playlist[j]);
						if (basepathOfTrackToLookUpInner != basepathOfTrackToLookUp) { // ...but keep on looking for the 2nd change...
							Log_Printf(LOGLEVEL_DEBUG, jumpBackwardsToFolder, basepathOfTrackToLookUpInner.data(), "\n");
							return j + 1; // ...just to add +1 to get the previous element before the 2nd change
						}
					}
				}
			}
		} else {
			return -1;
		}
		// If index 0 (first track) was hit meanwhile -> return it!
		return 0;
	}

	// If no jump possible, return -1
	return -1;
}

const String SdCard_GetVolumeLabel() {
#if FF_USE_LABEL
	char label[24];
	memset(label, 0, sizeof(label));

	DWORD vsn = 0;
	FRESULT res = f_getlabel("", label, &vsn);

	if (res == FR_OK && strlen(label) > 0) {
		return String(label);
	}
#endif
	return String("/");
}
