#include <Arduino.h>
#include "settings.h"

#include "AudioPlayer.h"

#include "Audio.h"
#include "Bluetooth.h"
#include "Cmd.h"
#include "Common.h"
#include "EnumUtils.h"
#include "Led.h"
#include "Log.h"
#include "MemX.h"
#include "Mqtt.h"
#include "Playstats.h"
#include "Port.h"
#include "Queues.h"
#include "Rfid.h"
#include "RotaryEncoder.h"
#include "SdCard.h"
#include "Sync.h"
#include "System.h"
#include "VolumeCurveLut.h"
#include "Web.h"
#include "Wlan.h"
#include "main.h"
#include "strnatcmp.h"

#include <ArduinoJson.h>
#include <atomic>
#include <esp_task_wdt.h>
#include <freertos/task.h>
#include <random>
#include <vector>

// Allocate gPlayProperties in PSRAM if available
EXT_RAM_BSS_ATTR playProps gPlayProperties;

// Pending relative seek in seconds, written from the button/rotary/web tasks and drained by the audio loop.
static std::atomic<int16_t> AudioPlayer_PendingSeekSeconds {0};

void AudioPlayer_AddSeekOffset(const int16_t seconds) {
	AudioPlayer_PendingSeekSeconds.fetch_add(seconds, std::memory_order_relaxed);
}

// Playlist
static playlistSortMode AudioPlayer_PlaylistSortMode = AUDIOPLAYER_PLAYLIST_SORT_MODE_DEFAULT;

// Volume
static uint8_t AudioPlayer_CurrentVolume = AUDIOPLAYER_VOLUME_INIT;
static uint8_t AudioPlayer_MaxVolume = AUDIOPLAYER_VOLUME_MAX;
static uint8_t AudioPlayer_MaxVolumeSpeaker = AUDIOPLAYER_VOLUME_MAX;
static uint8_t AudioPlayer_MinVolume = AUDIOPLAYER_VOLUME_MIN;
static uint8_t AudioPlayer_InitVolume = AUDIOPLAYER_VOLUME_INIT;

// current playtime
uint32_t AudioPlayer_CurrentTime = 0;
uint32_t AudioPlayer_FileDuration = 0;

// current stream format (cached from the audio task; 0/"" when nothing is decoding)
uint32_t AudioPlayer_BitRate = 0;
uint32_t AudioPlayer_SampleRate = 0;
uint8_t AudioPlayer_Channels = 0;
char AudioPlayer_CodecName[16] = "";

// Playtime stats
time_t playTimeSecTotal = 0;
time_t playTimeSecSinceStart = 0;

// current station logo url
static String AudioPlayer_StationLogoUrl;

#ifdef HEADPHONE_ADJUST_ENABLE
static bool AudioPlayer_HeadphoneLastDetectionState;
static uint32_t AudioPlayer_HeadphoneLastDetectionTimestamp = 0u;
static uint8_t AudioPlayer_MaxVolumeHeadphone = 11u; // Maximum volume that can be adjusted in headphone-mode (default; can be changed later via GUI)
#endif

static bool AudioPlayer_IsHeadphoneModeActive() {
#ifdef HEADPHONE_ADJUST_ENABLE
	const bool wiredHeadphoneConnected = !Audio_Detect_Mode_HP(Port_Read(HP_DETECT));
#else
	const bool wiredHeadphoneConnected = false;
#endif

	const bool bluetoothHeadphoneConnected = (System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE) && Bluetooth_Device_Connected();

	return wiredHeadphoneConnected || bluetoothHeadphoneConnected;
}

// dummy class to allocate audio object in PSRAM if available
class AudioCustom : public Audio {
public:
	void *operator new(size_t size) {
		return psramFound() ? ps_malloc(size) : malloc(size);
	}
};

Audio *audio = nullptr;

// new old varibles
constexpr uint32_t playbackTimeout = 2000;
uint32_t playbackTimeoutStart = millis();
uint8_t currentVolume;
BaseType_t trackQStatus = pdFAIL;
uint8_t trackCommand = NO_ACTION;
bool audioReturnCode;
uint32_t AudioPlayer_LastPlaytimeStatsTimestamp = 0u;
// Resume fade-in (see settings.h RESUME_FADEIN_*): millis() when the fade started after a
// rewound audiobook-resume; 0 = no fade active. Output starts muted and is ramped up so the
// brief cold-start glitch is masked and lands on the rewound (already-heard) audio.
uint32_t AudioPlayer_ResumeFadeStartMs = 0u;
// Sleep-timer fade-out (web setting "sleepFadeSec"): true while the output gain is currently
// being ramped down ahead of the running sleep timer, so we know to restore full volume once
// when the timer is cancelled/extended mid-fade.
static bool AudioPlayer_SleepFadeApplied = false;
Playlist *newPlayList = nullptr;
std::atomic<bool> newPlayListAvailable {false}; // producer publishes newPlayList *before* setting this (handover flag)

static bool AudioPlayer_UploadActive = false;
static bool AudioPlayer_WasPausedBeforeUpload = false; // remember pre-upload pause state
static bool gResetOldRfidOnIdle = false; // release the "don't accept same rfid twice"-lock on next idle-state

// Remember an RFID-tag whose webstream could not be started because WiFi is not (yet) connected, so it can
// be re-injected into the RFID-queue once WiFi is up (see handleWifiStateConnectionSuccess() in Wlan.cpp).
static void AudioPlayer_RememberRfidForWifiRetry(const char *rfidTagId) {
	strncpy(gRetryRfidTagId, rfidTagId, cardIdStringSize - 1);
	gRetryRfidTagId[cardIdStringSize - 1] = '\0';
	gRetryRfidOnWifiConnect = true;
}

// "Arm" the release of the DONT_ACCEPT_SAME_RFID_TWICE-lock: called by the RFID-handler the moment a new
// tag is accepted, it records that this playback-attempt happened. The lock is then actually released the
// next time the player becomes idle (see AudioPlayer_Cyclic()), which re-allows the same tag to be applied
// again. Arming on acceptance - rather than on playback becoming active - is deliberate: it ensures the
// release still fires even if the very first track fails immediately (e.g. a webstream applied before WiFi
// is connected), which would otherwise leave the tag locked forever.
void AudioPlayer_ArmRfidResetOnIdle(void) {
	gResetOldRfidOnIdle = true;
}

// Guards the per-path equalizer cache (AudioPlayer_EqRules, defined below): rebuilt by
// AudioPlayer_ReloadEqRules() from a web handler (async_tcp task) while AudioPlayer_ApplyEqualizerForPath()
// iterates it on the loop task. Held only briefly, never across NVS/JSON work. Created in AudioPlayer_Init()
// before the web server can call reload.
static SemaphoreHandle_t AudioPlayer_EqRulesMutex = NULL;

static void AudioPlayer_HeadphoneVolumeManager(void);
static std::optional<Playlist *> AudioPlayer_ReturnPlaylistFromWebstream(const char *_webUrl);
static bool AudioPlayer_ArrSortHelper_strcmp(const char *a, const char *b);
static bool AudioPlayer_ArrSortHelper_strnatcmp(const char *a, const char *b);
static bool AudioPlayer_ArrSortHelper_strnatcasecmp(const char *a, const char *b);
static void AudioPlayer_SortPlaylist(Playlist *playlist);
static void AudioPlayer_RandomizePlaylist(Playlist *playlist);
static size_t AudioPlayer_NvsRfidWriteWrapper(const char *_rfidCardId, const uint32_t _playPosition, const uint8_t _playMode, const uint16_t _trackLastPlayed);
static void AudioPlayer_ClearCover(void);
static void Audio_SetMeta(char *dest, size_t destSize, const char *line); // store an ID3/Vorbis field value (after ':'/'=') + notify the web UI
static void audio_id3image(File &file, const size_t pos, const size_t size);
static void audio_oggimage(File &file, std::vector<uint32_t> v);

// Resume playback if currently paused (mirrors the next/previous-track behavior): toggle the
// audio object, clear the pause flag and notify MQTT. No-op when already playing.
static void AudioPlayer_ResumeIfPaused(void) {
	if (gPlayProperties.pausePlay) {
		audio->pauseResume();
		gPlayProperties.pausePlay = false;
#ifdef MQTT_ENABLE
		publishMqtt(topicPausePlay, "play", false);
#endif
	}
}

// Persist the play-position checkpoint (track start, position 0) for the active RFID-tag, but only
// when saving is enabled for the current play-mode. Optionally logs the audiobook track-start.
static void AudioPlayer_SaveTrackStart(uint16_t trackNo, bool logTrackStart = false) {
	if (gPlayProperties.saveLastPlayPosition) {
		AudioPlayer_NvsRfidWriteWrapper(gPlayProperties.playRfidTag, 0, gPlayProperties.playMode, trackNo);
		if (logTrackStart) {
			Log_Println(trackStartAudiobook, LOGLEVEL_INFO);
		}
	}
}

void AudioPlayer_NotifyUploadStart(void) {
	if (AudioPlayer_UploadActive) {
		return; // already suspended – ignore nested calls
	}
	AudioPlayer_UploadActive = true;
	if (!gPlayProperties.pausePlay && gPlayProperties.playMode != NO_PLAYLIST && gPlayProperties.playMode != BUSY) {
		AudioPlayer_WasPausedBeforeUpload = false;
		audio->pauseResume();
	} else {
		AudioPlayer_WasPausedBeforeUpload = true; // was already paused / idle
	}
}

void AudioPlayer_NotifyUploadEnd(void) {
	if (!AudioPlayer_UploadActive) {
		return;
	}
	if (!AudioPlayer_WasPausedBeforeUpload) {
		audio->pauseResume();
	}
	AudioPlayer_UploadActive = false;
}

// Set (from the audio task) once the decoder knows a trustworthy bitrate for the current
// track: fired on the Xing/Info-header parse or once the measured average stabilizes. The
// resume fallback-seek waits for this signal — earlier seeks race the library's own
// connecttoFS seek and are computed from a still-swinging average bitrate.
static volatile bool gAudioBitrateKnown = false;

void Audio_InfoCallback(Audio::msg_t m) {
	switch (m.e) {
		case Audio::evt_info: {
			// Log_Printf(LOGLEVEL_INFO, "info:         %s", m.msg); // disabled to reduce log especially from files with numerous comments
			if (startsWith((char *) m.msg, "slow stream, dropouts")) {
				// websocket notify for slow stream
				Web_SendWebsocketData(0, WebsocketCodeType::Dropout);
			}
			break;
		}
		case Audio::evt_eof: { // end of file
			Log_Printf(LOGLEVEL_INFO, "end of file:  %s", m.msg);
			gPlayProperties.trackFinished = true;
			gPlayProperties.currentSpeechActive = false;
			break;
		}
		case Audio::evt_bitrate: {
			Log_Printf(LOGLEVEL_INFO, "bitrate:      %s", m.msg);
			gAudioBitrateKnown = true;
			break;
		}
		case Audio::evt_icyurl: {
			Log_Printf(LOGLEVEL_INFO, "icy URL:      %s", m.msg);
			if (m.msg && m.msg[0] != '\0' && AudioPlayer_StationLogoUrl.isEmpty()) {
				// has station homepage, get favicon url
				AudioPlayer_StationLogoUrl = "https://www.google.com/s2/favicons?sz=256&domain_url=" + String(m.msg);
				// websocket and mqtt notify station logo has changed
				Web_SendWebsocketData(0, WebsocketCodeType::CoverImg);
			}
			break;
		}
		case Audio::evt_id3data: {
			if (!m.msg) {
				break;
			}
			// Log_Printf(LOGLEVEL_INFO, "ID3 data:     %s", m.msg); // disabled to prevent log spam from files with numerous metadata
			// get title
			if (startsWith((char *) m.msg, "Title") || startsWith((char *) m.msg, "TITLE=") || startsWith((char *) m.msg, "title=")) { // ID3v1, ID3v2.3 and ID3v2.4: "Title:", VORBISCOMMENT: "TITLE=", "title=", "Title="
				int titleStart = 6;
				if (m.msg[5] == '/') { // ID3v2.2 "Title/Songname/Content description:"
					titleStart = 36;
				}
				const char *titleVal = (const char *) m.msg + titleStart;
				while (*titleVal == ' ') { // trim the leading space after "Title:"
					titleVal++;
				}
				if (gPlayProperties.playlist->size() > 1) {
					Audio_setTitle("(%u/%u): %s", gPlayProperties.currentTrackNumber + 1, gPlayProperties.playlist->size(), titleVal);
				} else {
					Audio_setTitle("%s", titleVal);
				}
			}
			// get artist / album (ID3 "Artist:"/"Album:" + VORBISCOMMENT "ARTIST="/"ALBUM=")
			else if (startsWith((char *) m.msg, "Artist") || startsWith((char *) m.msg, "ARTIST=") || startsWith((char *) m.msg, "artist=")) {
				Audio_SetMeta(gPlayProperties.artist, sizeof(gPlayProperties.artist), (const char *) m.msg);
#ifdef MQTT_ENABLE
				publishMqtt(topicTrackArtist, gPlayProperties.artist, false);
#endif
			} else if (startsWith((char *) m.msg, "Album") || startsWith((char *) m.msg, "ALBUM=") || startsWith((char *) m.msg, "album=")) {
				Audio_SetMeta(gPlayProperties.album, sizeof(gPlayProperties.album), (const char *) m.msg);
#ifdef MQTT_ENABLE
				publishMqtt(topicTrackAlbum, gPlayProperties.album, false);
#endif
			}
			break;
		}
		case Audio::evt_lasthost: { // stream URL played
			Log_Printf(LOGLEVEL_INFO, "last URL:     %s", m.msg);
			break;
		}
		case Audio::evt_name: { // station name or icy-name
			Log_Printf(LOGLEVEL_NOTICE, "station name: %s", m.msg);
			if (m.msg && m.msg[0] != '\0') {
				if (gPlayProperties.playlist->size() > 1) {
					Audio_setTitle("(%u/%u): %s", gPlayProperties.currentTrackNumber + 1, gPlayProperties.playlist->size(), m.msg);
				} else {
					Audio_setTitle("%s", m.msg);
				}
			}
			break;
		}
		case Audio::evt_streamtitle: {
			if (!gPlayProperties.isWebstream) {
				break; // prevents overwriting correct title for local files
			}
			Log_Printf(LOGLEVEL_INFO, "stream title: %s", m.msg);
			if (m.msg && m.msg[0] != '\0') {
				if (gPlayProperties.playlist->size() > 1) {
					Audio_setTitle("(%u/%u): %s", gPlayProperties.currentTrackNumber + 1, gPlayProperties.playlist->size(), m.msg);
				} else {
					Audio_setTitle("%s", m.msg);
				}
			}
			break;
		}
		case Audio::evt_icylogo: { // logo
			Log_Printf(LOGLEVEL_INFO, "icy logo:     %s", m.msg);
			if (m.msg && m.msg[0] != '\0') {
				AudioPlayer_StationLogoUrl = m.msg;
				// websocket and mqtt notify station logo has changed
				Web_SendWebsocketData(0, WebsocketCodeType::CoverImg);
			}
			break;
		}
		case Audio::evt_image: {
			if (!gPlayProperties.playlist || gPlayProperties.currentTrackNumber >= gPlayProperties.playlist->size()) {
				break;
			}
			const char *fileName = gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber);
			File file = gFSystem.open(fileName, FILE_READ);
			if (!file) {
				Log_Printf(LOGLEVEL_ERROR, "Failed to open file: %s", fileName);
				break;
			}
			char fileType[4];
			if (file.readBytes(fileType, 4) == 4) {
				if (strncmp(fileType, "OggS", 4) == 0) {
					audio_oggimage(file, m.vec);
				} else {
					audio_id3image(file, m.vec[0], m.vec[1]);
				}
			}
			file.close();
			break;
		}
		default: // ignored events: evt_icydescription, evt_lyrics, evt_log
			break;
	}
}

// Cached volume-curve selection. Audio_GetVolume() is called from the audio decode task on every
// gain-ramp tick (i.e. continuously during playback); reading it from NVS each time was a needless
// flash-namespace lookup in the hottest audio path. Seeded in AudioPlayer_Init() and refreshed by
// AudioPlayer_SetVolumeCurve() when the web setting changes. Single byte => atomic across cores.
static uint8_t gVolumeCurveType = 0;

void AudioPlayer_SetVolumeCurve(uint8_t curveType) {
	gVolumeCurveType = (curveType < VOL_LUT_CURVES) ? curveType : VOL_CURVE_PERCEPTUAL;
}

float Audio_GetVolume(float t) {
	uint8_t curve_type = gVolumeCurveType;

	// 1. Safety Checks
	if (curve_type >= VOL_LUT_CURVES) {
		curve_type = VOL_CURVE_PERCEPTUAL;
	}
	if (t <= 0.0f) {
		return pgm_read_float(&(VOLUME_TABLE[curve_type][0]));
	}

	// 2. Calculate indices
	float index_f = t * (VOL_LUT_STEPS - 1);
	int index = (int) index_f;

	// Safety clamp for the edge case where index_f is exactly 63.0
	if (index >= VOL_LUT_STEPS - 1) {
		return pgm_read_float(&(VOLUME_TABLE[curve_type][VOL_LUT_STEPS - 1]));
	}

	float fraction = index_f - (float) index;

	// 3. Interpolate
	float val1 = pgm_read_float(&(VOLUME_TABLE[curve_type][index]));
	float val2 = pgm_read_float(&(VOLUME_TABLE[curve_type][index + 1]));

	return val1 + (val2 - val1) * fraction;
}

// When true, the audiobook play-position is checkpointed to NVS periodically while playing
// (see AudioPlayer_Cyclic). Mirrors the "savePosPeriodic" web setting; default on.
static bool gSavePosPeriodic = true;

void AudioPlayer_SetSavePosPeriodic(bool enabled) {
	gSavePosPeriodic = enabled;
}

// Step (in seconds) used by CMD_SMART_FORWARDS/BACKWARDS for in-file seeking. Cached from the
// "seekStep" web setting so smart-seek presses don't hit NVS. Default see settings.h (seekStepDefault).
static uint16_t gSeekStep = seekStepDefault;

void AudioPlayer_SetSeekStep(uint16_t seconds) {
	gSeekStep = (seconds == 0) ? seekStepDefault : seconds;
}

uint16_t AudioPlayer_GetSeekStep(void) {
	return gSeekStep;
}

// Cached from the "minResumeSec" / "shortTrackSec" web settings so the resume-point saves
// (periodic checkpoint, pause, card removal) don't take the NVS mutex for reads — a
// concurrent NVS writer (RFID sync, settings save) could otherwise delay the save path.
static uint32_t gMinResumeSec = 20;
static uint32_t gShortTrackSec = 300;

void AudioPlayer_SetMinResumeSec(uint32_t seconds) {
	gMinResumeSec = seconds;
}

void AudioPlayer_SetShortTrackSec(uint32_t seconds) {
	gShortTrackSec = seconds;
}

// Fade-out span (in seconds) before the sleep timer expires. Cached from the "sleepFadeSec" web
// setting so the audio loop doesn't hit NVS each iteration. 0 = feature off (hard stop as before).
static uint16_t gSleepFadeSec = 0;

void AudioPlayer_SetSleepFadeSec(uint16_t seconds) {
	gSleepFadeSec = seconds;
}

uint16_t AudioPlayer_GetSleepFadeSec(void) {
	return gSleepFadeSec;
}

// Daily listening-time limit (screentime) in minutes. Cached from the "dailyLimitMin" web setting
// so the per-second audio tick doesn't hit NVS. 0 = feature off.
static uint16_t gDailyLimitMin = 0;

void AudioPlayer_SetDailyLimitMin(uint16_t minutes) {
	gDailyLimitMin = minutes;
}

uint16_t AudioPlayer_GetDailyLimitMin(void) {
	return gDailyLimitMin;
}

// true once today's accumulated listening time has reached the configured daily limit. Enforced
// only while the system clock is valid: Playstats_GetToday() returns 0 with an unknown date, so
// playback is never blocked without a trustworthy day.
bool AudioPlayer_DailyLimitReached(void) {
	return (gDailyLimitMin > 0) && (Playstats_GetToday() >= (uint32_t) gDailyLimitMin * 60u);
}

#ifdef MQTT_ENABLE
// Publish the screentime state to MQTT, but only on change so we don't spam the broker: today's
// listened minutes (whole-minute granularity) and whether the daily limit has been reached.
void AudioPlayer_PublishListeningStatsMqtt(void) {
	static int32_t lastMin = -1;
	static int8_t lastReached = -1;
	const uint32_t todayMin = Playstats_GetToday() / 60u;
	if ((int32_t) todayMin != lastMin) {
		lastMin = (int32_t) todayMin;
		publishMqtt(topicListenedToday, todayMin, false);
	}
	const bool reached = AudioPlayer_DailyLimitReached();
	if ((int8_t) reached != lastReached) {
		lastReached = (int8_t) reached;
		publishMqtt(topicDailyLimitReached, reached ? "ON" : "OFF", false);
	}
}
#endif

// Guards the lifetime of gPlayProperties.playlist across cores: the audio task frees + reassigns it
// when a new playlist arrives, while the web task (/currenttrack, /cover) reads playlist->at()/size().
// Without this a web read could dereference a just-freed vector/string (use-after-free). The audio
// task's own reads need no lock (same task as the free). Held only briefly - never across SD/network.
static SemaphoreHandle_t gPlaylistMutex = NULL;
void AudioPlayer_LockPlaylist(void) {
	if (gPlaylistMutex) {
		xSemaphoreTake(gPlaylistMutex, portMAX_DELAY);
	}
}
void AudioPlayer_UnlockPlaylist(void) {
	if (gPlaylistMutex) {
		xSemaphoreGive(gPlaylistMutex);
	}
}

void AudioPlayer_Init(void) {
	// create audio object
	audio = new AudioCustom();

	// guards the playlist pointer between the audio task and the web readers (created before the web server starts)
	gPlaylistMutex = xSemaphoreCreateMutex();

	// load playtime total from NVS
	playTimeSecTotal = gPrefsSettings.getULong("playTimeTotal", 0);
	Playstats_Init(); // daily listening-time statistics (today / yesterday / 7d / 30d)

	uint8_t playListSortModeValue = gPrefsSettings.getUChar("PLSortMode", EnumUtils::underlying_value(AudioPlayer_PlaylistSortMode));
	AudioPlayer_PlaylistSortMode = EnumUtils::to_enum<playlistSortMode>(playListSortModeValue);

	AudioPlayer_SetVolumeCurve(gPrefsSettings.getUChar("volumeCurve", 0)); // cache for the audio-task hot path

	uint32_t nvsInitialVolume;
	if (!gPrefsSettings.getBool("recoverVolBoot", false)) {
		// Get initial volume from NVS
		nvsInitialVolume = gPrefsSettings.getUInt("initVolume", 0);
	} else {
		// Get volume used at last shutdown
		nvsInitialVolume = gPrefsSettings.getUInt("previousVolume", 999);
		if (nvsInitialVolume == 999) {
			gPrefsSettings.putUInt("previousVolume", AudioPlayer_GetInitVolume());
			nvsInitialVolume = AudioPlayer_GetInitVolume();
		} else {
			Log_Println(rememberLastVolume, LOGLEVEL_ERROR);
		}
	}

	if (nvsInitialVolume) {
		AudioPlayer_SetInitVolume(nvsInitialVolume);
		Log_Printf(LOGLEVEL_INFO, restoredInitialLoudnessFromNvs, nvsInitialVolume);
	} else {
		gPrefsSettings.putUInt("initVolume", AudioPlayer_GetInitVolume());
		Log_Println(wroteInitialLoudnessToNvs, LOGLEVEL_ERROR);
	}

	// Get maximum volume for speaker from NVS
	uint32_t nvsMaxVolumeSpeaker = gPrefsSettings.getUInt("maxVolumeSp", 0);
	if (nvsMaxVolumeSpeaker) {
		AudioPlayer_SetMaxVolumeSpeaker(nvsMaxVolumeSpeaker);
		AudioPlayer_SetMaxVolume(nvsMaxVolumeSpeaker);
		Log_Printf(LOGLEVEL_INFO, restoredMaxLoudnessForSpeakerFromNvs, nvsMaxVolumeSpeaker);
	} else {
		// Set max volume to max per default. Can be adjusted later via webinterface.
		gPrefsSettings.putUInt("maxVolumeSp", 21);
		Log_Println(wroteMaxLoudnessForSpeakerToNvs, LOGLEVEL_ERROR);
	}

#ifdef HEADPHONE_ADJUST_ENABLE
	#if (HP_DETECT >= 0 && HP_DETECT <= MAX_GPIO)
	pinMode(HP_DETECT, INPUT_PULLUP);
	#endif
	AudioPlayer_HeadphoneLastDetectionState = Audio_Detect_Mode_HP(Port_Read(HP_DETECT));

	// Get maximum volume for headphone from NVS
	uint32_t nvsAudioPlayer_MaxVolumeHeadphone = gPrefsSettings.getUInt("maxVolumeHp", 0);
	if (nvsAudioPlayer_MaxVolumeHeadphone) {
		AudioPlayer_MaxVolumeHeadphone = nvsAudioPlayer_MaxVolumeHeadphone;
		Log_Printf(LOGLEVEL_INFO, restoredMaxLoudnessForHeadphoneFromNvs, nvsAudioPlayer_MaxVolumeHeadphone);
	} else {
		// Set max volume to max per default. Can be adjusted later via webinterface.
		gPrefsSettings.putUInt("maxVolumeHp", 21);
		Log_Println(wroteMaxLoudnessForHeadphoneToNvs, LOGLEVEL_ERROR);
	}
#endif
	// initialize gPlayProperties
	gPlayProperties = {};

	// Adjust volume depending on headphone is connected and volume-adjustment is enabled
	// (loads the NVS playMono flag into gPlayProperties, so it must run *after* the struct is zeroed)
	AudioPlayer_SetupVolumeAndAmps();

	gPlayProperties.playlistFinished = true;
	gPlayProperties.jumpToFolderTrack = -1;
	gPlayProperties.gainLowPass = 0;
	gPlayProperties.gainBandPass = 0;
	gPlayProperties.gainHighPass = 0;

	// clear title and cover image
	gPlayProperties.title[0] = '\0';
	gPlayProperties.coverFilePos = 0;
	AudioPlayer_StationLogoUrl = "";
	gPlayProperties.playlist = allocatePlaylist();
	gPlayProperties.SavePlayPosRfidChange = gPrefsSettings.getBool("savePosRfidChge", false); // SAVE_PLAYPOS_WHEN_RFID_CHANGE
	gSavePosPeriodic = gPrefsSettings.getBool("savePosPeriodic", true); // periodic audiobook play-position checkpoint
	AudioPlayer_SetSeekStep(gPrefsSettings.getUInt("seekStep", seekStepDefault)); // step for smart forward/backward (routes through the 0 -> default guard)
	AudioPlayer_SetMinResumeSec(gPrefsSettings.getUInt("minResumeSec", 20));
	AudioPlayer_SetShortTrackSec(gPrefsSettings.getUInt("shortTrackSec", 300));
	AudioPlayer_SetSleepFadeSec(gPrefsSettings.getUInt("sleepFadeSec", 0)); // fade out over the last N s before the sleep timer expires (0 = off)
	AudioPlayer_SetDailyLimitMin(gPrefsSettings.getUInt("dailyLimitMin", 0)); // daily listening-time limit in minutes (0 = off)
	gPlayProperties.pauseOnMinVolume = gPrefsSettings.getBool("pauseOnMinVol", false); // PAUSE_ON_MIN_VOLUME
#ifdef PAUSE_WHEN_RFID_REMOVED
	gPlayProperties.pauseIfRfidRemoved = gPrefsSettings.getBool("pauseRfidRem", true);
#else
	gPlayProperties.pauseIfRfidRemoved = gPrefsSettings.getBool("pauseRfidRem", false);
#endif
	gPlayProperties.stopIfRfidRemoved = gPrefsSettings.getBool("stopRfidRem", false); // stop (instead of pause) when RFID is removed
#ifdef DONT_ACCEPT_SAME_RFID_TWICE
	gPlayProperties.dontAcceptRfidTwice = gPrefsSettings.getBool("dAccRfidTwice", true);
#else
	gPlayProperties.dontAcceptRfidTwice = gPrefsSettings.getBool("dAccRfidTwice", false);
#endif
#ifdef RESUME_ON_SAME_RFID
	gPlayProperties.resumeOnSameRfid = gPrefsSettings.getBool("p2pSameRfid", true);
#else
	gPlayProperties.resumeOnSameRfid = gPrefsSettings.getBool("p2pSameRfid", false);
#endif
	if (gPlayProperties.pauseIfRfidRemoved) {
		// ignore feature silently if PAUSE_WHEN_RFID_REMOVED is active
		Log_Println("pauseIfRfidRemoved is enabled -> deactivate dontAcceptRfidTwice", LOGLEVEL_NOTICE);
		gPlayProperties.dontAcceptRfidTwice = false;
	}

#ifdef I2S_COMM_FMT_LSB_ENABLE
	audio->setI2SCommFMT_LSB(true);
#endif

	AudioPlayer_CurrentVolume = AudioPlayer_GetInitVolume();
	// DMA-settings must be adjusted before setting the pinout
	// (ESP32-audioI2S v3.4.7h defaults to 16-bit output, so the former setOutput16Bit(true) is gone.)
	// 48 descriptors x 256 frames ~= 279 ms of buffered output at 44.1 kHz (~17 KB more
	// internal RAM than the previous 32/186 ms). The extra cushion absorbs the cache
	// freezes that NVS flash writes/erases inflict on both cores mid-playback.
	audio->settings.DMA_DESC_NUM = 48;
	audio->settings.DMA_FRAME_NUM = 256; // not too high, so safe SRAM
	if (System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE) {
		audio->setOutputSampleRate(Audio::OutputSR_t::SR_44100);
		audio->settings.DMA_FRAME_NUM = 192; // not too high, to safe some SRAM
	} else if (System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) {
		audio->settings.DMA_FRAME_NUM = 192; // not too high, to safe some SRAM
	} else {
		// just use default-values
	}

	audio->setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
	audio->setVolumeSteps(AUDIOPLAYER_VOLUME_MAX);
	audio->setVolumeCurve(Audio_GetVolume);
	audio->setVolume(AudioPlayer_CurrentVolume);
	audio->forceMono(gPlayProperties.currentPlayMono);
	audio->setTone(
		gPrefsSettings.getChar("gainLowPass", 0),
		gPrefsSettings.getChar("gainBandPass", 0),
		gPrefsSettings.getChar("gainHighPass", 0));
	// guards the per-path equalizer cache between the loop task and web readers (created before the web server starts)
	AudioPlayer_EqRulesMutex = xSemaphoreCreateMutex();
	AudioPlayer_ReloadEqRules();

	audio->setAudioTaskCore(1);
	audio->audio_info_callback = Audio_InfoCallback;
}

void AudioPlayer_Exit(void) {
	Log_Println("shutdown audioplayer..", LOGLEVEL_NOTICE);
	// save playtime total to NVS
	playTimeSecTotal += playTimeSecSinceStart;
	gPrefsSettings.putULong("playTimeTotal", playTimeSecTotal);
	Playstats_Save(); // flush daily listening-time statistics on shutdown
	// Make sure last playposition for audiobook is saved when playback is active while shutdown was initiated
	if (gPrefsSettings.getBool("savePosShutdown", false) && !gPlayProperties.pausePlay && (gPlayProperties.playMode == AUDIOBOOK || gPlayProperties.playMode == AUDIOBOOK_LOOP || gPlayProperties.playMode == AUDIOBOOK_RECURSIVE)) {
		AudioPlayer_SetTrackControl(PAUSEPLAY);
		// Call the loop explicitely to make sure that PAUSE is set (because this saves the current playpos)
		AudioPlayer_Loop();
	}
	delete audio;
	audio = nullptr;
}

static uint32_t lastPlayingTimestamp = 0;

// Periodically persist the audiobook play-position to NVS while playing, so an *ungraceful*
// power-off (dead battery, hard power switch) doesn't lose the whole book back to the last
// track boundary. The existing save points (track change / RFID swap / graceful shutdown)
// don't cover a sudden loss of power mid-track. Throttled so NVS wear stays negligible.
static constexpr uint32_t SAVE_POS_CHECKPOINT_INTERVAL_MS = 30000;
static uint32_t lastPosCheckpointMs = 0;

// An audiobook resumed mid-file plays from byte 0 for a brief moment until the decoder knows the
// bitrate and can seek to the saved position; audio->getAudioCurrentTime() reads a transient near-0
// value during that window. Persisting it then would clobber real progress with a near-start value
// (and minResumeSec would round it down to 0), so a card pulled right after re-applying would lose
// the whole book. Suppress position-saves until the resume seek has actually landed (or, as a safety
// net, until a timeout elapses) so NVS keeps the correct resume point in the meantime.
static bool gResumeSeekPending = false; // a cold-start resume seek is still settling
static bool gResumeFallbackSeekDone = false; // the one player-driven fallback seek was already fired
static uint32_t gResumeTargetSec = 0; // file-time (s) the decoder is seeking to
static uint32_t gResumeSeekStartedMs = 0; // millis() the resume began (safety-timeout anchor)
static uint8_t gResumeLowTimeTicks = 0; // consecutive 250ms ticks the decoder sat near the file start
static constexpr uint32_t RESUME_SEEK_SETTLE_TIMEOUT_MS = 10000;
// Longest span the resume fade-in start may be deferred while waiting for the fallback seek
// to fire; past this the fade just runs so an unseekable file can't hold the output silent.
static constexpr uint32_t RESUME_HOLD_CAP_MS = 4000;

// True when it's safe to persist the audiobook play-position. While a resume seek is still settling
// the reported time is bogus, so callers should skip the save (NVS keeps the correct resume point).
// Self-clears once the decoder has caught up to the target or the safety timeout elapses.
static bool AudioPlayer_ResumePositionSettled(void) {
	if (!gResumeSeekPending) {
		return true;
	}
	const bool caughtUp = audio && audio->isRunning() && (audio->getAudioCurrentTime() + 1 >= gResumeTargetSec);
	const bool timedOut = (millis() - gResumeSeekStartedMs) >= RESUME_SEEK_SETTLE_TIMEOUT_MS;
	if (caughtUp || timedOut) {
		gResumeSeekPending = false;
		return true;
	}
	return false;
}

void AudioPlayer_Cyclic(void) {
	if (AudioPlayer_UploadActive) {
		return;
	}

	AudioPlayer_HeadphoneVolumeManager();
	if ((millis() - lastPlayingTimestamp >= 1000) && gPlayProperties.playMode != NO_PLAYLIST && gPlayProperties.playMode != BUSY && !gPlayProperties.pausePlay) {
		// audio is playing, update the playtime since start
		lastPlayingTimestamp = millis();
		playTimeSecSinceStart += 1;
		Playstats_AddSecond(); // accumulate per-day listening-time statistics

		// Daily listening-time limit (screentime): once today's accumulated time reaches the
		// configured limit, stop playback. Re-starting is refused in AudioPlayer_SetPlaylist
		// until the next calendar day. Only enforced while the clock is valid (see
		// AudioPlayer_DailyLimitReached), so a device without a trustworthy date never locks out.
		if (AudioPlayer_DailyLimitReached()) {
			Log_Println("Daily listening limit reached - stopping playback", LOGLEVEL_NOTICE);
			AudioPlayer_SetTrackControl(STOP);
		}
#ifdef MQTT_ENABLE
		AudioPlayer_PublishListeningStatsMqtt();
#endif

		// periodic audiobook play-position checkpoint (see note above). saveLastPlayPosition
		// is only set for the position-saving audiobook modes, so this is a no-op otherwise.
		if (gSavePosPeriodic && gPlayProperties.saveLastPlayPosition && gPlayProperties.playlist != nullptr && audio && audio->isRunning()) {
			if (millis() - lastPosCheckpointMs >= SAVE_POS_CHECKPOINT_INTERVAL_MS && AudioPlayer_ResumePositionSettled()) {
				lastPosCheckpointMs = millis();
				AudioPlayer_NvsRfidWriteWrapper(gPlayProperties.playRfidTag, audio->getAudioCurrentTime(), gPlayProperties.playMode, gPlayProperties.currentTrackNumber);
			}
		}
	}

	// Actual loop stuff
	AudioPlayer_Loop();
}

// Wrapper-function to reverse detection of connected headphones.
// Normally headphones are supposed to be plugged in if a given GPIO/channel is LOW/false.
bool Audio_Detect_Mode_HP(bool _state) {
#ifndef DETECT_HP_ON_HIGH
	return _state;
#else
	return !_state;
#endif
}

playlistSortMode AudioPlayer_GetPlaylistSortMode(void) {
	return AudioPlayer_PlaylistSortMode;
}

bool AudioPlayer_SetPlaylistSortMode(playlistSortMode value) {
	AudioPlayer_PlaylistSortMode = value;
	size_t written = gPrefsSettings.putUChar("PLSortMode", EnumUtils::underlying_value(AudioPlayer_PlaylistSortMode));
	return (written == 1);
}

bool AudioPlayer_SetPlaylistSortMode(uint8_t value) {
	return AudioPlayer_SetPlaylistSortMode(EnumUtils::to_enum<playlistSortMode>(value));
}

uint8_t AudioPlayer_GetCurrentVolume(void) {
	return AudioPlayer_CurrentVolume;
}

void AudioPlayer_SetCurrentVolume(uint8_t value) {
	AudioPlayer_CurrentVolume = value;
}

uint8_t AudioPlayer_GetMaxVolume(void) {
	return AudioPlayer_MaxVolume;
}

void AudioPlayer_SetMaxVolume(uint8_t value) {
	AudioPlayer_MaxVolume = value;
}

uint8_t AudioPlayer_GetMaxVolumeSpeaker(void) {
	return AudioPlayer_MaxVolumeSpeaker;
}

void AudioPlayer_SetMaxVolumeSpeaker(uint8_t value) {
	AudioPlayer_MaxVolumeSpeaker = value;
}

uint8_t AudioPlayer_GetMinVolume(void) {
	return AudioPlayer_MinVolume;
}

void AudioPlayer_SetMinVolume(uint8_t value) {
	AudioPlayer_MinVolume = value;
}

uint8_t AudioPlayer_GetInitVolume(void) {
	return AudioPlayer_InitVolume;
}

void AudioPlayer_SetInitVolume(uint8_t value) {
	AudioPlayer_InitVolume = value;
}

time_t AudioPlayer_GetPlayTimeAllTime(void) {
	return (playTimeSecTotal + playTimeSecSinceStart) * 1000;
}

time_t AudioPlayer_GetPlayTimeSinceStart(void) {
	return (playTimeSecSinceStart * 1000);
}

uint32_t AudioPlayer_GetCurrentTime(void) {
	return AudioPlayer_CurrentTime;
}

uint32_t AudioPlayer_GetFileDuration(void) {
	return AudioPlayer_FileDuration;
}

uint32_t AudioPlayer_GetBitRate(void) {
	return AudioPlayer_BitRate;
}
uint32_t AudioPlayer_GetSampleRate(void) {
	return AudioPlayer_SampleRate;
}
uint8_t AudioPlayer_GetChannels(void) {
	return AudioPlayer_Channels;
}
const char *AudioPlayer_GetCodecName(void) {
	return AudioPlayer_CodecName;
}

String AudioPlayer_GetStationLogoUrl(void) {
	return AudioPlayer_StationLogoUrl;
}

void Audio_setTitle(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vsnprintf(gPlayProperties.title, sizeof(gPlayProperties.title) / sizeof(gPlayProperties.title[0]), format, args);
	va_end(args);

	// notify web ui and mqtt
	Web_SendWebsocketData(0, WebsocketCodeType::TrackInfo);
#ifdef MQTT_ENABLE
	publishMqtt(topicTrack, gPlayProperties.title, false);
#endif
}

// Store an ID3/Vorbis metadata field (e.g. "Artist: X" or "ARTIST=X"): copies the value after the
// first ':' or '=' delimiter (leading spaces trimmed) into dest, then notifies the web UI.
static void Audio_SetMeta(char *dest, size_t destSize, const char *line) {
	const char *v = strchr(line, '=');
	if (!v) {
		v = strchr(line, ':');
	}
	if (v) {
		v++;
		while (*v == ' ') {
			v++;
		}
	} else {
		v = "";
	}
	strncpy(dest, v, destSize - 1);
	dest[destSize - 1] = '\0';
	Web_SendWebsocketData(0, WebsocketCodeType::TrackInfo);
}

// Set maxVolume depending on headphone-adjustment is enabled and headphone is/is not connected
// Enable/disable PA/HP-amps initially
void AudioPlayer_SetupVolumeAndAmps(void) {
	const bool playMono = gPrefsSettings.getBool("playMono", false);
	gPlayProperties.currentPlayMono = playMono;
	gPlayProperties.newPlayMono = playMono;

#ifndef HEADPHONE_ADJUST_ENABLE
	AudioPlayer_MaxVolume = AudioPlayer_MaxVolumeSpeaker;
	// If automatic HP-detection is not used, we enabled both (PA / HP) if defined
	#ifdef GPIO_PA_EN
	Port_Write(GPIO_PA_EN, true, true);
	#endif
	#ifdef GPIO_HP_EN
	Port_Write(GPIO_HP_EN, true, true);
	#endif
#else

	if (!AudioPlayer_IsHeadphoneModeActive()) {
		AudioPlayer_MaxVolume = AudioPlayer_MaxVolumeSpeaker; // 1 if headphone is not connected
	#ifdef GPIO_PA_EN
		Port_Write(GPIO_PA_EN, true, true);
	#endif
	#ifdef GPIO_HP_EN
		Port_Write(GPIO_HP_EN, false, true);
	#endif
	} else {
		AudioPlayer_MaxVolume = AudioPlayer_MaxVolumeHeadphone; // 0 if headphone is connected (put to GND)
		gPlayProperties.newPlayMono = false; // always stereo for headphones!

	#ifdef GPIO_PA_EN
		Port_Write(GPIO_PA_EN, false, true);
	#endif
	#ifdef GPIO_HP_EN
		Port_Write(GPIO_HP_EN, true, true);
	#endif
	}
	Log_Printf(LOGLEVEL_INFO, maxVolumeSet, AudioPlayer_MaxVolume);
	return;
#endif
}

void AudioPlayer_HeadphoneVolumeManager(void) {
#ifdef HEADPHONE_ADJUST_ENABLE
	const bool currentHeadPhoneDetectionState = Audio_Detect_Mode_HP(Port_Read(HP_DETECT));

	if (AudioPlayer_HeadphoneLastDetectionState != currentHeadPhoneDetectionState && (millis() - AudioPlayer_HeadphoneLastDetectionTimestamp >= headphoneLastDetectionDebounce)) {
		if (!AudioPlayer_IsHeadphoneModeActive()) {
			AudioPlayer_MaxVolume = AudioPlayer_MaxVolumeSpeaker;
			gPlayProperties.newPlayMono = gPrefsSettings.getBool("playMono", false);

	#ifdef GPIO_PA_EN
			Port_Write(GPIO_PA_EN, true, false);
	#endif
	#ifdef GPIO_HP_EN
			Port_Write(GPIO_HP_EN, false, false);
	#endif
		} else {
			AudioPlayer_MaxVolume = AudioPlayer_MaxVolumeHeadphone;
			gPlayProperties.newPlayMono = false; // Always stereo for headphones
			if (AudioPlayer_GetCurrentVolume() > AudioPlayer_MaxVolume) {
				AudioPlayer_SetVolume(AudioPlayer_MaxVolume); // Lower volume for headphone if headphone's maxvolume is exceeded by volume set in speaker-mode
			}

	#ifdef GPIO_PA_EN
			Port_Write(GPIO_PA_EN, false, false);
	#endif
	#ifdef GPIO_HP_EN
			Port_Write(GPIO_HP_EN, true, false);
	#endif
		}
		AudioPlayer_HeadphoneLastDetectionState = currentHeadPhoneDetectionState;
		AudioPlayer_HeadphoneLastDetectionTimestamp = millis();
		Log_Printf(LOGLEVEL_INFO, maxVolumeSet, AudioPlayer_MaxVolume);
	}
#endif
}

// Function to play music as task
void AudioPlayer_Loop() {
	// Resume fade-in: ramp the output volume from 0 back to the user's volume over
	// RESUME_FADEIN_DURATION_MS after a rewound audiobook-resume, so the brief cold-start
	// glitch is masked. Only the audio-lib gain is ramped; AudioPlayer_CurrentVolume (and
	// thus the UI/MQTT-reported volume) is left untouched.
	if (AudioPlayer_ResumeFadeStartMs && (RESUME_FADEIN_DURATION_MS > 0)) {
		// While the resume seek is still outstanding, keep re-arming the fade start so both
		// the wrong-content phase (track plays from its beginning until the seek fires) and
		// the seek's buffer flush happen at volume 0. This only slides an already-armed
		// anchor (a manual volume change cancels the fade for good), only for targets the
		// fallback seek will actually chase (>= 15 s), never during a TTS announcement, and
		// is capped so an unseekable file can't keep the output silent. Every exit path
		// simply lets the existing self-completing ramp below run.
		if (gResumeSeekPending && !gResumeFallbackSeekDone
			&& (gResumeTargetSec >= 15) && !gPlayProperties.currentSpeechActive
			&& ((millis() - gResumeSeekStartedMs) < RESUME_HOLD_CAP_MS)) {
			AudioPlayer_ResumeFadeStartMs = millis();
		}
		const uint8_t target = AudioPlayer_GetCurrentVolume();
		const uint32_t elapsed = millis() - AudioPlayer_ResumeFadeStartMs;
		if (elapsed >= RESUME_FADEIN_DURATION_MS) {
			audio->setVolume(target);
			AudioPlayer_ResumeFadeStartMs = 0;
		} else {
			audio->setVolume((uint8_t) ((uint32_t) target * elapsed / RESUME_FADEIN_DURATION_MS));
		}
	}

	// Sleep-timer fade-out: during the last gSleepFadeSec seconds before a running (timed) sleep
	// timer expires, ramp the output gain from the user's volume down towards 0 so playback fades
	// out gently instead of being cut off. Only the audio-lib gain is touched (not
	// AudioPlayer_CurrentVolume), so cancelling/extending the timer restores the real volume.
	if (gSleepFadeSec > 0 && System_GetSleepTimerTimeStamp() > 0) {
		const uint32_t remaining = System_GetSleepTimerRemainingSeconds();
		if (remaining <= gSleepFadeSec) {
			const uint8_t target = AudioPlayer_GetCurrentVolume();
			audio->setVolume((uint8_t) ((uint32_t) target * remaining / gSleepFadeSec));
			AudioPlayer_SleepFadeApplied = true;
		} else if (AudioPlayer_SleepFadeApplied) {
			// timer was extended back out of the fade window -> restore full volume once
			audio->setVolume(AudioPlayer_GetCurrentVolume());
			AudioPlayer_SleepFadeApplied = false;
		}
	} else if (AudioPlayer_SleepFadeApplied) {
		// timer disarmed (or feature switched off) mid-fade -> restore full volume once
		audio->setVolume(AudioPlayer_GetCurrentVolume());
		AudioPlayer_SleepFadeApplied = false;
	}

	// Update playtime stats every 250 ms
	if ((millis() - AudioPlayer_LastPlaytimeStatsTimestamp) > 250) {
		AudioPlayer_LastPlaytimeStatsTimestamp = millis();
		// Update current playtime and duration
		AudioPlayer_CurrentTime = audio->getAudioCurrentTime();
		AudioPlayer_FileDuration = audio->getAudioFileDuration();
		// While a mid-file resume seek is still settling, the decoder briefly reports a
		// near-0 time. Report the seek target instead, so the neopixel/web progress bars
		// (and the OLED elapsed time) don't jump to "start of track" after a card is
		// re-applied, only to leap back to the real position once the seek lands.
		if (!AudioPlayer_ResumePositionSettled() && AudioPlayer_CurrentTime < gResumeTargetSec) {
			AudioPlayer_CurrentTime = gResumeTargetSec;
		}
		// The audio library only executes the connecttoFS() start-time seek for files
		// whose header yields a nominal bitrate (Xing/Info frame within the first 50
		// bytes of frame data); other files silently play from the beginning, losing
		// the audiobook resume position. Fire ONE fallback seek from here — but only
		// after the library's own attempt had time to land, and only when playback
		// demonstrably still sits near the file start on two consecutive ticks (the
		// library seek briefly reports ~0 while it lands; re-seeking on top of it
		// corrupts the playtime bookkeeping and tracks then appear to end early).
		if (gResumeSeekPending && gAudioBitrateKnown && (audio->getAudioCurrentTime() < 10)) {
			// Counted only once the bitrate is known: for files with a Xing/Info header the
			// library's own seek fires right at that moment and the playtime jumps to the
			// target, so these ticks never accumulate and the fallback stays quiet.
			if (gResumeLowTimeTicks < UINT8_MAX) {
				gResumeLowTimeTicks++;
			}
		} else {
			gResumeLowTimeTicks = 0;
		}
		if (gResumeSeekPending && !gResumeFallbackSeekDone && audio->isRunning()
			&& !gPlayProperties.currentSpeechActive
			&& (audio->getAudioFileDuration() > 0)
			&& ((millis() - gResumeSeekStartedMs) >= 750)
			&& (gResumeLowTimeTicks >= 2) && (gResumeTargetSec >= 15)) {
			gResumeFallbackSeekDone = true;
			if (AudioPlayer_ResumeFadeStartMs) {
				// Restart the fade ramp at the seek: its buffer flush and the content jump
				// land within the first ~10% of the ramp, i.e. effectively at volume 0.
				AudioPlayer_ResumeFadeStartMs = millis();
			}
			if (audio->setAudioPlayTime(gResumeTargetSec)) {
				Log_Printf(LOGLEVEL_DEBUG, "resume-seek to %u s driven from player (library seek didn't land)", (unsigned) gResumeTargetSec);
			}
		}
		// Cache the current stream format for the now-playing info dialog (read cross-core)
		AudioPlayer_BitRate = audio->getBitRate();
		AudioPlayer_SampleRate = audio->getSampleRate();
		AudioPlayer_Channels = audio->getChannels();
		{
			const char *cn = audio->getCodecname();
			strncpy(AudioPlayer_CodecName, cn ? cn : "", sizeof(AudioPlayer_CodecName) - 1);
			AudioPlayer_CodecName[sizeof(AudioPlayer_CodecName) - 1] = '\0';
		}
		// Calculate relative position in file (for trackprogress neopixel & web-ui)
		gPlayProperties.audioFileDuration = AudioPlayer_FileDuration;
		if (!gPlayProperties.playlistFinished && AudioPlayer_FileDuration > 0) {
			// for local files and web files with known size
			if (!gPlayProperties.pausePlay && (gPlayProperties.seekmode != SEEK_POS_PERCENT)) { // To progress necessary when paused
				gPlayProperties.currentRelPos = ((float) AudioPlayer_CurrentTime / AudioPlayer_FileDuration) * 100.0f;
			}
		} else {
			if (gPlayProperties.isWebstream && (System_GetOperationMode() != OPMODE_BLUETOOTH_SINK) && (audio->getInBufferSize() > 0)) {
				// calc current fillbuffer percent for webstream with unknown size/end
				gPlayProperties.currentRelPos = (double) (audio->inBufferFilled() / (double) audio->getInBufferSize()) * 100;
			} else {
				gPlayProperties.currentRelPos = 0;
			}
		}
	}

	// A track-control keypress (play/pause, next, ...) during a running IP-/time-announcement
	// aborts the announcement and resumes the audiobook, instead of acting on the speech
	// audio-object. Clearing currentSpeechActive lets the speech-resume below re-inject the
	// RFID-tag and continue the book from its saved position.
	if (gPlayProperties.currentSpeechActive && trackCommand != NO_ACTION) {
		trackCommand = NO_ACTION;
		gPlayProperties.tellMode = TTS_NONE;
		audio->stopSong();
		gPlayProperties.currentSpeechActive = false;
		return;
	}

	if (newPlayListAvailable || gPlayProperties.trackFinished || trackCommand != NO_ACTION) {
		if (newPlayListAvailable) {
			newPlayListAvailable = false;
			audio->stopSong();

			// destroy the old playlist and assign the new one (locked: a web reader may be mid-read)
			AudioPlayer_LockPlaylist();
			freePlaylist(gPlayProperties.playlist);
			gPlayProperties.playlist = newPlayList;
			newPlayList = nullptr; // consumed - drop our copy so a stale pointer can't be re-used
			AudioPlayer_UnlockPlaylist();
			Log_Printf(LOGLEVEL_NOTICE, newPlaylistReceived, gPlayProperties.playlist->size());
			Log_Printf(LOGLEVEL_DEBUG, "Free heap: %u", ESP.getFreeHeap());
			playbackTimeoutStart = millis();
			gPlayProperties.pausePlay = false;
			gPlayProperties.trackFinished = false;
			gPlayProperties.playlistFinished = false;
			gPlayProperties.smartSeekPendingSec = 0; // drop any not-yet-applied smart-seek from the previous track
#ifdef MQTT_ENABLE
			publishMqtt(topicPausePlay, "play", false);
			publishMqtt(topicPlaymode, static_cast<uint32_t>(gPlayProperties.playMode), false);
			publishMqtt(topicRepeatMode, static_cast<uint32_t>(AudioPlayer_GetRepeatMode()), false);
#endif

			// If we're in audiobook-mode and apply a modification-card, we don't
			// want to save lastPlayPosition for the mod-card but for the card that holds the playlist
			if (strlen(gCurrentRfidTagId) > 0) {
				strncpy(gPlayProperties.playRfidTag, gCurrentRfidTagId, sizeof(gPlayProperties.playRfidTag) / sizeof(gPlayProperties.playRfidTag[0]));
			}
		}
		if (gPlayProperties.trackFinished) {
			gPlayProperties.trackFinished = false;
			if (gPlayProperties.playMode == NO_PLAYLIST || gPlayProperties.playlist == nullptr) {
				gPlayProperties.playlistFinished = true;
				return;
			}
			if (gPlayProperties.currentTrackNumber + 1 < gPlayProperties.playlist->size()) {
				// Only save if there's another track, otherwise it will be saved at end of playlist anyway
				// (the helper additionally gates on saveLastPlayPosition; skipped for AUDIOBOOK_LOOP)
				AudioPlayer_SaveTrackStart(gPlayProperties.currentTrackNumber + 1);
			}
			if (gPlayProperties.sleepAfterCurrentTrack) { // Go to sleep if "sleep after track" was requested
				gPlayProperties.playlistFinished = true;
				gPlayProperties.playMode = NO_PLAYLIST;
				System_RequestSleep();
				return; // TODO-> check if this is necessary or if we need a flag here
			}
			if (!gPlayProperties.repeatCurrentTrack) { // If endless-loop requested, track-number will not be incremented
				gPlayProperties.currentTrackNumber++;
			} else {
				Log_Println(repeatTrackDueToPlaymode, LOGLEVEL_INFO);
				Led_Indicate(LedIndicatorType::Rewind);
			}
		}

		// Resolve smart-seek (SMARTFORWARD/SMARTBACKWARD): on a single long file it becomes a coalesced
		// in-file seek (rapid presses accumulate into one jump -> one resync), otherwise it falls back to
		// a normal next/previous track change. Reads the cached duration so it works from any caller context.
		if (trackCommand == SMARTFORWARD || trackCommand == SMARTBACKWARD) {
			const bool forward = (trackCommand == SMARTFORWARD);
			const uint16_t step = AudioPlayer_GetSeekStep();
			// A single-file playlist (audiobook) always smart-seeks in-file - even before the decoder
			// has reported the duration (audioFileDuration is 0 for the first ~second after a card tap
			// or resume). The coalesced seek below clamps to the end when the duration is known and
			// otherwise does a relative setTimeOffset, so seeking with an unknown duration is safe.
			// The old "audioFileDuration > step" guard turned an early forward press into a NEXTTRACK,
			// which on a one-file playlist became "last track active" -> an error beep instead of a seek.
			const bool singleLongFile = (gPlayProperties.playlist != nullptr) && (gPlayProperties.playlist->size() == 1);
			if (singleLongFile) {
				AudioPlayer_ResumeIfPaused(); // resume before seeking (mirrors the next/previous-track behavior)
				gPlayProperties.smartSeekPendingSec += forward ? (int32_t) step : -(int32_t) step;
				gPlayProperties.smartSeekRequestMs = millis();
				trackCommand = NO_ACTION; // applied later (coalesced) in the seek-handling section
				return; // exit before the switch/reconnect tail so the current file isn't restarted from 0
			} else {
				trackCommand = forward ? NEXTTRACK : PREVIOUSTRACK;
			}
		}

		if (gPlayProperties.playlistFinished && trackCommand != NO_ACTION) {
			if (gPlayProperties.playMode != BUSY) { // Prevents from staying in mode BUSY forever when error occured (e.g. directory empty that should be played)
				Log_Println(noPlaymodeChangeIfIdle, LOGLEVEL_NOTICE);
				trackCommand = NO_ACTION;
				System_IndicateError();
				return;
			}
		}
		/* Check if track-control was called
		   (stop, start, next track, prev. track, last track, first track...) */
		switch (trackCommand) {
			case STOP:
				// Persist the audiobook resume point *before* stopping (e.g. the card was pulled in
				// stop-on-removal mode) so the next play continues exactly where it left off instead of
				// from the last ~30 s periodic checkpoint or the track start. Mirrors the pause-save guard:
				// only the position-saving audiobook modes carry a resume point (saveLastPlayPosition), and
				// a running TTS announcement is skipped (its ~0 s position would clobber the book's slot).
				// getAudioCurrentTime() must be read before stopSong() resets it. A pull within the first
				// few seconds is reset to the very start by AudioPlayer_NvsRfidWriteWrapper (minResumeSec).
				if (gPlayProperties.saveLastPlayPosition && !gPlayProperties.currentSpeechActive && audio && audio->isRunning() && AudioPlayer_ResumePositionSettled()) {
					Log_Printf(LOGLEVEL_INFO, trackPausedAtPos, audio->getAudioCurrentTime(), audio->getAudioFileDuration());
					AudioPlayer_NvsRfidWriteWrapper(gPlayProperties.playRfidTag, audio->getAudioCurrentTime(), gPlayProperties.playMode, gPlayProperties.currentTrackNumber);
				}
				audio->stopSong();
				trackCommand = NO_ACTION;
				Log_Println(cmndStop, LOGLEVEL_INFO);
				gPlayProperties.pausePlay = true;
				gPlayProperties.playlistFinished = true;
				gPlayProperties.playMode = NO_PLAYLIST;
				Audio_setTitle(noPlaylist);
				AudioPlayer_ClearCover();
				Playstats_Save(); // flush listening stats now that nothing plays (periodic save runs only every 5 min)
#ifdef MQTT_ENABLE
				publishMqtt(topicPausePlay, "idle", false);
#endif
				return;

			case PAUSEPLAY:
				trackCommand = NO_ACTION;
				audio->pauseResume();
				if (gPlayProperties.pausePlay) {
					Log_Println(cmndResumeFromPause, LOGLEVEL_INFO);
#ifdef MQTT_ENABLE
					publishMqtt(topicPausePlay, "play", false);
#endif
				} else {
					Log_Println(cmndPause, LOGLEVEL_INFO);
#ifdef MQTT_ENABLE
					publishMqtt(topicPausePlay, "pause", false);
#endif
				}
				// Don't persist while a TTS-announcement (IP/time) is running: the audio-object is
				// then decoding the speech, so getAudioCurrentTime() would write the announcement's
				// position (~0s) into the audiobook's NVS-slot and resume the book from the start.
				if (gPlayProperties.saveLastPlayPosition && !gPlayProperties.pausePlay && !gPlayProperties.currentSpeechActive && AudioPlayer_ResumePositionSettled()) {
					Log_Printf(LOGLEVEL_INFO, trackPausedAtPos, audio->getAudioCurrentTime(), audio->getAudioFileDuration());
					AudioPlayer_NvsRfidWriteWrapper(gPlayProperties.playRfidTag, audio->getAudioCurrentTime(), gPlayProperties.playMode, gPlayProperties.currentTrackNumber);
				}
				if (!gPlayProperties.pausePlay) {
					Playstats_Save(); // entering pause: flush listening stats (periodic save runs only every 5 min)
				}
				gPlayProperties.pausePlay = !gPlayProperties.pausePlay;

				Web_SendWebsocketData(0, WebsocketCodeType::TrackInfo);
				return;

			case NEXTTRACK:
				trackCommand = NO_ACTION;
				AudioPlayer_ResumeIfPaused();
				if (gPlayProperties.repeatCurrentTrack) { // End loop if button was pressed
					gPlayProperties.repeatCurrentTrack = false;
#ifdef MQTT_ENABLE
					publishMqtt(topicRepeatMode, static_cast<uint32_t>(AudioPlayer_GetRepeatMode()), false);
#endif
				}
				// Allow next track if current track played in playlist isn't the last track.
				// Exception: loop-playlist is active. In this case playback restarts at the first track of the playlist.
				if ((gPlayProperties.currentTrackNumber + 1 < gPlayProperties.playlist->size()) || gPlayProperties.repeatPlaylist) {
					if ((gPlayProperties.currentTrackNumber + 1 >= gPlayProperties.playlist->size()) && gPlayProperties.repeatPlaylist) {
						gPlayProperties.currentTrackNumber = 0;
					} else {
						gPlayProperties.currentTrackNumber++;
					}
					AudioPlayer_SaveTrackStart(gPlayProperties.currentTrackNumber, true);
					Log_Println(cmndNextTrack, LOGLEVEL_INFO);
					if (!gPlayProperties.playlistFinished) {
						audio->stopSong();
					}
				} else {
					Log_Println(lastTrackAlreadyActive, LOGLEVEL_NOTICE);
					System_IndicateError();
					return;
				}
				break;

			case PREVIOUSTRACK:
				trackCommand = NO_ACTION;
				AudioPlayer_ResumeIfPaused();
				if (gPlayProperties.repeatCurrentTrack) { // End loop if button was pressed
					gPlayProperties.repeatCurrentTrack = false;
#ifdef MQTT_ENABLE
					publishMqtt(topicRepeatMode, static_cast<uint32_t>(AudioPlayer_GetRepeatMode()), false);
#endif
				}
				if (gPlayProperties.playMode == WEBSTREAM) {
					Log_Println(trackChangeWebstream, LOGLEVEL_INFO);
					System_IndicateError();
					return;
				} else if (gPlayProperties.playMode == LOCAL_M3U) {
					Log_Println(cmndPrevTrack, LOGLEVEL_INFO);
					if (gPlayProperties.currentTrackNumber > 0) {
						gPlayProperties.currentTrackNumber--;
					} else {
						System_IndicateError();
						return;
					}
				} else {
					if (gPlayProperties.currentTrackNumber > 0 || gPlayProperties.repeatPlaylist) {
						if (audio->getAudioCurrentTime() < 5) { // play previous track when current track time is small, else play current track again
							if (gPlayProperties.currentTrackNumber == 0 && gPlayProperties.repeatPlaylist) {
								gPlayProperties.currentTrackNumber = gPlayProperties.playlist->size() - 1; // Go back to last track in loop-mode when first track is played
							} else {
								gPlayProperties.currentTrackNumber--;
							}
						}

						AudioPlayer_SaveTrackStart(gPlayProperties.currentTrackNumber, true);

						Log_Println(cmndPrevTrack, LOGLEVEL_INFO);
						if (!gPlayProperties.playlistFinished) {
							audio->stopSong();
						}
					} else {
						AudioPlayer_SaveTrackStart(gPlayProperties.currentTrackNumber);
						audio->stopSong();
						Led_Indicate(LedIndicatorType::Rewind);
						String pathToTrack = gFSystem.rawPath(gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber));
						audioReturnCode = audio->connecttoFS(gFSystem, pathToTrack.c_str());
						// consider track as finished, when audio lib call was not successful
						if (!audioReturnCode) {
							System_IndicateError();
							gPlayProperties.trackFinished = true;
							return;
						}
						Log_Println(trackStart, LOGLEVEL_INFO);
						return;
					}
				}
				break;
			case FIRSTTRACK:
				trackCommand = NO_ACTION;
				AudioPlayer_ResumeIfPaused();
				gPlayProperties.currentTrackNumber = 0;
				AudioPlayer_SaveTrackStart(gPlayProperties.currentTrackNumber, true);
				Log_Println(cmndFirstTrack, LOGLEVEL_INFO);
				if (!gPlayProperties.playlistFinished) {
					audio->stopSong();
				}
				break;

			case LASTTRACK:
				trackCommand = NO_ACTION;
				AudioPlayer_ResumeIfPaused();
				if (gPlayProperties.currentTrackNumber + 1 < gPlayProperties.playlist->size()) {
					gPlayProperties.currentTrackNumber = gPlayProperties.playlist->size() - 1;
					AudioPlayer_SaveTrackStart(gPlayProperties.currentTrackNumber, true);
					Log_Println(cmndLastTrack, LOGLEVEL_INFO);
					if (!gPlayProperties.playlistFinished) {
						audio->stopSong();
					}
				} else {
					Log_Println(lastTrackAlreadyActive, LOGLEVEL_NOTICE);
					System_IndicateError();
					return;
				}
				break;

			case NEXTFOLDER: // Used for recursive playmodes
				trackCommand = NO_ACTION;
				if (gPlayProperties.pausePlay) {
					audio->pauseResume();
					gPlayProperties.pausePlay = false;
				}
				gPlayProperties.jumpToFolderTrack = SdCard_findNextOrPrevDirectoryTrack(*gPlayProperties.playlist, gPlayProperties.currentTrackNumber, SearchDirection::Forward);
				if (gPlayProperties.jumpToFolderTrack != -1) {
					gPlayProperties.currentTrackNumber = gPlayProperties.jumpToFolderTrack;
					gPlayProperties.jumpToFolderTrack = -1;
					AudioPlayer_SaveTrackStart(gPlayProperties.currentTrackNumber);
				} else {
					Log_Println(lastFolderAlreadyActive, LOGLEVEL_NOTICE);
					System_IndicateError();
					return;
				}
				break;

			case PREVIOUSFOLDER: // Used for recursive playmodes
				trackCommand = NO_ACTION;
				if (gPlayProperties.pausePlay) {
					audio->pauseResume();
					gPlayProperties.pausePlay = false;
				}

				gPlayProperties.jumpToFolderTrack = SdCard_findNextOrPrevDirectoryTrack(*gPlayProperties.playlist, gPlayProperties.currentTrackNumber, SearchDirection::Backward);
				if (gPlayProperties.jumpToFolderTrack != -1) {
					gPlayProperties.currentTrackNumber = gPlayProperties.jumpToFolderTrack;
					gPlayProperties.jumpToFolderTrack = -1;
					AudioPlayer_SaveTrackStart(gPlayProperties.currentTrackNumber);
				} else {
					System_IndicateError();
					return;
				}
				break;

			case 0:
				break;

			default:
				trackCommand = NO_ACTION;
				Log_Println(cmndDoesNotExist, LOGLEVEL_NOTICE);
				System_IndicateError();
				return;
		}

		if (gPlayProperties.playUntilTrackNumber == gPlayProperties.currentTrackNumber && gPlayProperties.playUntilTrackNumber > 0) {
			AudioPlayer_SaveTrackStart(0);
			gPlayProperties.playlistFinished = true;
			gPlayProperties.playMode = NO_PLAYLIST;
			System_RequestSleep();
			return;
		}

		if (gPlayProperties.currentTrackNumber >= gPlayProperties.playlist->size()) { // Check if last element of playlist is already reached
			Log_Println(endOfPlaylistReached, LOGLEVEL_NOTICE);
			if (!gPlayProperties.repeatPlaylist) {
				// Set back to first track
				AudioPlayer_SaveTrackStart(0);
				gPlayProperties.playlistFinished = true;
				gPlayProperties.playMode = NO_PLAYLIST;
				Audio_setTitle(noPlaylist);
				AudioPlayer_ClearCover();
#ifdef MQTT_ENABLE
				publishMqtt(topicPlaymode, static_cast<uint32_t>(gPlayProperties.playMode), false);
#endif
				gPlayProperties.currentTrackNumber = 0;
				if (gPlayProperties.sleepAfterPlaylist) {
					System_RequestSleep();
				}
				return;
			} else { // Check if sleep after current track/playlist was requested
				if (gPlayProperties.sleepAfterPlaylist || gPlayProperties.sleepAfterCurrentTrack) {
					gPlayProperties.playlistFinished = true;
					gPlayProperties.playMode = NO_PLAYLIST;
					System_RequestSleep();
					return;
				} // Repeat playlist; set current track number back to 0
				Log_Println(repeatPlaylistDueToPlaymode, LOGLEVEL_NOTICE);
				gPlayProperties.currentTrackNumber = 0;
				AudioPlayer_SaveTrackStart(gPlayProperties.currentTrackNumber);
			}
		}

		if (!strncmp("http", gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber), 4)) {
			gPlayProperties.isWebstream = true;
		} else {
			gPlayProperties.isWebstream = false;
		}
		gPlayProperties.currentRelPos = 0;
		audioReturnCode = false;
		bool resumeFadeWanted = false; // set when a rewound audiobook-resume should fade in (see AudioPlayer_Loop)

		if (gPlayProperties.playMode == WEBSTREAM || (gPlayProperties.playMode == LOCAL_M3U && gPlayProperties.isWebstream)) { // Webstream
			audioReturnCode = audio->connecttohost(gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber));
			gPlayProperties.playlistFinished = false;
		} else if (gPlayProperties.playMode != WEBSTREAM && !gPlayProperties.isWebstream) {
			// Files from SD
			if (!gFSystem.exists(gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber))) { // Check first if file/folder exists
				Log_Printf(LOGLEVEL_ERROR, dirOrFileDoesNotExist, gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber));
				gPlayProperties.trackFinished = true;
				return;
			} else {
				int32_t fileStartTime = -1;
				gResumeSeekPending = false; // cleared by default; re-armed below only for an actual mid-file resume
				gResumeFallbackSeekDone = false;
				gResumeLowTimeTicks = 0;
				gAudioBitrateKnown = false; // re-set by Audio_InfoCallback once this track's bitrate is known
				if (gPlayProperties.startAtFilePos > 0) {
					fileStartTime = gPlayProperties.startAtFilePos;
					if (fileStartTime > 65535) {
						fileStartTime = 65535; // connecttoFS()/setAudioPlayTime() takes a uint16_t (seconds); clamp so an >18h resume can't wrap
					}
					if (RESUME_FADEIN_DURATION_MS > 0) {
						// Rewind a few seconds so the cold-start glitch (file open + header
						// decode + seek-flush) lands on already-heard audio, then fade in below.
						fileStartTime = (fileStartTime > (int32_t) RESUME_FADEIN_REWIND_S) ? (fileStartTime - (int32_t) RESUME_FADEIN_REWIND_S) : 1;
						resumeFadeWanted = true;
					}
					// Arm the resume-settle guard: until the decoder reaches this file-time, suppress
					// position-saves so a card pulled during the pre-seek window can't persist a near-0
					// position and lose the book's progress (see AudioPlayer_ResumePositionSettled).
					gResumeSeekPending = true;
					gResumeTargetSec = (uint32_t) fileStartTime;
					gResumeSeekStartedMs = millis();
					Log_Printf(LOGLEVEL_NOTICE, trackStartatPos, fileStartTime);
					gPlayProperties.startAtFilePos = 0;
				}
				String pathToTrack = gFSystem.rawPath(gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber));
				// apply the per-path equalizer (directory/file rule) before starting the track
				AudioPlayer_ApplyEqualizerForPath(gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber));
				audioReturnCode
					= audio->connecttoFS(gFSystem, pathToTrack.c_str(), fileStartTime);
				// consider track as finished, when audio lib call was not successful
			}
		}

		if (!audioReturnCode) {
			System_IndicateError();
			// If a webstream (e.g. from an m3u-playlist) failed because WiFi is not (yet) connected,
			// remember the tag and retry it once WiFi is up.
			if (gPlayProperties.isWebstream && !Wlan_IsConnected()) {
				AudioPlayer_RememberRfidForWifiRetry(gPlayProperties.playRfidTag);
			}
			gPlayProperties.trackFinished = true;
			return;
		} else {
			if (gResumeSeekPending) {
				// connecttoFS() blocks for the SD open + header read; anchor the fallback-seek
				// wait (and the settle timeout) after it so the wait isn't partly consumed.
				gResumeSeekStartedMs = millis();
			}
			if (resumeFadeWanted) {
				// Start the (rewound) resume muted; AudioPlayer_Loop ramps the volume back up.
				audio->setVolume(0);
				AudioPlayer_ResumeFadeStartMs = millis();
			}
			if (gPlayProperties.currentTrackNumber) {
				Led_Indicate(LedIndicatorType::PlaylistProgress);
			}
			const char *title = gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber);
			if (gPlayProperties.isWebstream) {
				title = "Webradio";
			}
			if (gPlayProperties.playlist->size() > 1) {
				Audio_setTitle("(%u/%u): %s", gPlayProperties.currentTrackNumber + 1, gPlayProperties.playlist->size(), title);
			} else {
				Audio_setTitle("%s", title);
			}
			AudioPlayer_ClearCover();
			// after ClearCover (which zeroes it): cache the file size for web handlers, so
			// they don't have to open the currently playing file on SD again
			gPlayProperties.audioFileSize = gPlayProperties.isWebstream ? 0 : audio->getFileSize();
			Log_Printf(LOGLEVEL_NOTICE, currentlyPlaying, gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber), (gPlayProperties.currentTrackNumber + 1), gPlayProperties.playlist->size());
			gPlayProperties.playlistFinished = false;
		}
	}

	// Apply a coalesced smart-seek once the rapid presses have settled. Bundling several presses into a
	// single setTimeOffset() means one buffer-flush/resync instead of one per press (which felt sluggish,
	// and dropped presses while the decoder was re-syncing - especially on FLAC).
	if (gPlayProperties.smartSeekPendingSec != 0 && (millis() - gPlayProperties.smartSeekRequestMs >= smartSeekCoalesceMs)) {
		const int32_t offset = gPlayProperties.smartSeekPendingSec;
		gPlayProperties.smartSeekPendingSec = 0;
		const uint32_t duration = audio->getAudioFileDuration();
		bool ok;
		if (duration > 0) {
			// Clamp the target into the file: a forward jump past the end lands a few seconds before
			// the end and keeps playing, instead of overshooting and ending the file (setTimeOffset
			// would call stopSong()). Backward clamps to the start.
			int32_t target = (int32_t) audio->getAudioCurrentTime() + offset;
			const int32_t maxTarget = (duration > smartSeekEndMarginSec) ? (int32_t) (duration - smartSeekEndMarginSec) : 0;
			if (target < 0) {
				target = 0;
			} else if (target > maxTarget) {
				target = maxTarget;
			}
			if (target > 65535) {
				target = 65535; // setAudioPlayTime() takes a uint16_t (seconds); clamp so an >18h file can't wrap to a backward jump
			}
			ok = audio->setAudioPlayTime((uint16_t) target);
		} else {
			// duration unknown -> fall back to a relative offset
			ok = audio->setTimeOffset(offset);
		}
		if (ok) {
			if (offset >= 0) {
				Log_Printf(LOGLEVEL_NOTICE, secondsJumpForward, offset);
			} else {
				Log_Printf(LOGLEVEL_NOTICE, secondsJumpBackward, -offset);
			}
		} else {
			System_IndicateError();
		}
	}

	// Relative seek. Accumulated in an atomic so that firing CMD_SEEK_* once per rotary detent scrubs
	// proportionally instead of collapsing into a single jump (seekmode was a single overwrite-able enum
	// consumed once per iteration, so rapid repeats were lost).
	const int16_t seekOffset = AudioPlayer_PendingSeekSeconds.exchange(0, std::memory_order_relaxed);
	if (seekOffset != 0) {
		if (audio->setTimeOffset(seekOffset)) {
			Log_Printf(LOGLEVEL_NOTICE, (seekOffset > 0) ? secondsJumpForward : secondsJumpBackward, abs(seekOffset));
		}
	}

	// Handle seekmodes
	if (gPlayProperties.seekmode != SEEK_NORMAL) {
		if ((gPlayProperties.seekmode == SEEK_POS_PERCENT) && (gPlayProperties.currentRelPos > 0) && (gPlayProperties.currentRelPos < 100)) {
			uint32_t newFileTime = uint32_t((gPlayProperties.currentRelPos / 100.0f) * audio->getAudioFileDuration());
			if (audio->setAudioPlayTime(newFileTime)) {
				Log_Printf(LOGLEVEL_NOTICE, JumpToPosition, newFileTime, audio->getAudioFileDuration());
			} else {
				System_IndicateError();
			}
		}
		gPlayProperties.seekmode = SEEK_NORMAL;
	}

	// Handle IP-announcement
	if (gPlayProperties.tellMode == TTS_IP_ADDRESS) {
		gPlayProperties.tellMode = TTS_NONE;
		String ipText = Wlan_GetIpAddress();
		bool speechOk;
		// make IP as text (replace thousand separator with locale text)
		switch (LANGUAGE) {
			case DE:
				ipText.replace(".", "Punkt");
				speechOk = audio->connecttospeech(ipText.c_str(), "de");
				break;
			case FR:
				ipText.replace(".", "point");
				speechOk = audio->connecttospeech(ipText.c_str(), "fr");
				break;
			default:
				ipText.replace(".", "point");
				speechOk = audio->connecttospeech(ipText.c_str(), "en");
		}
		if (!speechOk) {
			System_IndicateError();
		}
	}

	// Handle time-announcement
	if (gPlayProperties.tellMode == TTS_CURRENT_TIME) {
		gPlayProperties.tellMode = TTS_NONE;
		struct tm timeinfo;
		getLocalTime(&timeinfo);
		static char timeStringBuff[64];
		bool speechOk;
#if (LANGUAGE == DE)
		snprintf(timeStringBuff, sizeof(timeStringBuff), "Es ist %02d:%02d Uhr", timeinfo.tm_hour, timeinfo.tm_min);
		speechOk = audio->connecttospeech(timeStringBuff, "de");
#else
		if (timeinfo.tm_hour > 12) {
			snprintf(timeStringBuff, sizeof(timeStringBuff), "It is %02d:%02d PM", timeinfo.tm_hour - 12, timeinfo.tm_min);
		} else {
			snprintf(timeStringBuff, sizeof(timeStringBuff), "It is %02d:%02d AM", timeinfo.tm_hour, timeinfo.tm_min);
		}
		speechOk = audio->connecttospeech(timeStringBuff, "en");
#endif
		if (!speechOk) {
			System_IndicateError();
		}
	}

	// If speech is over, go back to predefined state
	if (!gPlayProperties.currentSpeechActive && gPlayProperties.lastSpeechActive) {
		gPlayProperties.lastSpeechActive = false;
		if (gPlayProperties.playMode != NO_PLAYLIST) {
			xQueueSend(gRfidCardQueue, gPlayProperties.playRfidTag, 0); // Re-inject previous RFID-ID in order to continue playback
		}
	}

	// Handle if mono/stereo should be changed (e.g. if plugging headphones)
	if (gPlayProperties.newPlayMono != gPlayProperties.currentPlayMono) {
		gPlayProperties.currentPlayMono = gPlayProperties.newPlayMono;
		audio->forceMono(gPlayProperties.currentPlayMono);
		if (gPlayProperties.currentPlayMono) {
			Log_Println(newPlayModeMono, LOGLEVEL_NOTICE);
		} else {
			Log_Println(newPlayModeStereo, LOGLEVEL_NOTICE);
		}
		audio->setTone(gPlayProperties.gainLowPass, gPlayProperties.gainBandPass, gPlayProperties.gainHighPass);
	}

	audio->loop(); // Call audio-loop function to process incoming data

	if (gPlayProperties.playlistFinished || gPlayProperties.pausePlay) {
	} else {
		System_UpdateActivityTimer(); // Refresh if playlist is active so uC will not fall asleep due to reaching inactivity-time
	}

	if (audio->isRunning()) {
		playbackTimeoutStart = millis();
	}

	// If error occured: move to the next track in the playlist
	const bool activeMode = (gPlayProperties.playMode != NO_PLAYLIST && gPlayProperties.playMode != BUSY);
	const bool noAudio = (!audio->isRunning() && !gPlayProperties.pausePlay);
	const bool timeout = ((millis() - playbackTimeoutStart) > playbackTimeout);
	if (activeMode) {
		// we check for timeout
		if (noAudio && timeout) {
			// Audio playback timed out, move on to the next
			System_IndicateError();
			gPlayProperties.trackFinished = true;
			playbackTimeoutStart = millis();
		}
	} else {
		// we are idle, update timeout so that we do not get a spurious error when launching into a playlist
		playbackTimeoutStart = millis();
	}

	if (gPlayProperties.dontAcceptRfidTwice) {
		// Release the lock once playback is idle again, so the same tag can be applied anew. The lock is
		// armed when a tag is accepted (see Rfid_PreferenceLookupHandler()), independent of whether playback
		// actually started - otherwise a tag whose first track fails immediately (e.g. a webstream without
		// WiFi) would stay locked forever and could never be retried.
		if (gPlayProperties.playlistFinished || gPlayProperties.playMode == NO_PLAYLIST) {
			if (gResetOldRfidOnIdle) {
				Rfid_ResetOldRfid();
				gResetOldRfidOnIdle = false;
			}
		}
	}
}

// Returns current repeat-mode (mix of repeat current track and current playlist)
uint8_t AudioPlayer_GetRepeatMode(void) {
	if (gPlayProperties.repeatPlaylist && gPlayProperties.repeatCurrentTrack) {
		return TRACK_N_PLAYLIST;
	} else if (gPlayProperties.repeatPlaylist && !gPlayProperties.repeatCurrentTrack) {
		return PLAYLIST;
	} else if (!gPlayProperties.repeatPlaylist && gPlayProperties.repeatCurrentTrack) {
		return TRACK;
	} else {
		return NO_REPEAT;
	}
}

// Adds new volume-entry to volume-queue
// If volume is changed via webgui or MQTT, it's necessary to re-adjust current value of rotary-encoder.
void AudioPlayer_SetVolume(const int32_t _newVolume) {
	uint32_t _volume;
	int32_t _volumeBuf = AudioPlayer_GetCurrentVolume();

	Led_Indicate(LedIndicatorType::VolumeChange);
	// Clamp rather than reject: a fast rotary spin can ask for several steps at once, and rejecting the whole
	// change meant that near the rails (e.g. 20 -> 23 with max 21) nothing happened at all instead of pinning.
	int32_t clampedVolume = _newVolume;
	if (clampedVolume < AudioPlayer_GetMinVolume()) {
		clampedVolume = AudioPlayer_GetMinVolume();
		Log_Println(minLoudnessReached, LOGLEVEL_INFO);
	} else if (clampedVolume > AudioPlayer_GetMaxVolume()) {
		clampedVolume = AudioPlayer_GetMaxVolume();
		Log_Println(maxLoudnessReached, LOGLEVEL_INFO);
	}
	{
		_volume = clampedVolume;
		AudioPlayer_SetCurrentVolume(_volume);

		Log_Printf(LOGLEVEL_INFO, newLoudnessReceived, _volume);
		AudioPlayer_ResumeFadeStartMs = 0; // a manual volume change cancels an active resume fade-in
		audio->setVolume(_volume);
		Web_SendWebsocketData(0, WebsocketCodeType::Volume);
#ifdef MQTT_ENABLE
		publishMqtt(topicLoudness, static_cast<uint32_t>(_volume), false);
#endif
		AudioPlayer_PauseOnMinVolume(_volumeBuf, clampedVolume);
	}
}

// Adds equalizer settings low, band and high pass and readjusts the equalizer
void AudioPlayer_SetEqualizer(const int8_t gainLowPass, const int8_t gainBandPass, const int8_t gainHighPass) {
	audio->setTone(gainLowPass, gainBandPass, gainHighPass);
}

// Predefined equalizer profiles. Values are gains in dB (low / band / high) and must mirror
// the profiles offered by the web interface (see eqProfiles in management.html).
struct EqProfile {
	const char *name;
	int8_t low;
	int8_t band;
	int8_t high;
};
static const EqProfile AudioPlayer_EqProfiles[] = {
	{	  "flat",	 0, 0, 0},
	{	 "music",  3, 0, 1},
	{	 "speech", -2, 4, 3},
	{"voiceBoost", -4, 5, 4},
};

// Apply a predefined equalizer profile by name, persist it to NVS and publish the new state.
// Returns false if the profile name is unknown.
bool AudioPlayer_SetEqualizerProfile(const char *profile) {
	if (profile == nullptr) {
		return false;
	}
	for (const EqProfile &p : AudioPlayer_EqProfiles) {
		if (strcmp(profile, p.name) == 0) {
			gPrefsSettings.putChar("gainLowPass", p.low);
			gPrefsSettings.putChar("gainBandPass", p.band);
			gPrefsSettings.putChar("gainHighPass", p.high);
			gPrefsSettings.putString("eqProfile", p.name);
			gPlayProperties.gainLowPass = p.low;
			gPlayProperties.gainBandPass = p.band;
			gPlayProperties.gainHighPass = p.high;
			AudioPlayer_SetEqualizer(p.low, p.band, p.high);
			Log_Printf(LOGLEVEL_NOTICE, "Equalizer profile: %s", p.name);
#ifdef MQTT_ENABLE
			publishMqtt(topicEqualizer, p.name, false);
#endif
			return true;
		}
	}
	return false;
}

// Cycle to the next predefined equalizer profile (wraps around). If the currently stored profile
// is unknown (e.g. "custom"), start at the first profile.
void AudioPlayer_CycleEqualizerProfile(void) {
	const String current = gPrefsSettings.getString("eqProfile", "flat");
	const size_t count = sizeof(AudioPlayer_EqProfiles) / sizeof(AudioPlayer_EqProfiles[0]);
	size_t idx = 0;
	bool found = false;
	for (size_t i = 0; i < count; i++) {
		if (current == AudioPlayer_EqProfiles[i].name) {
			idx = i;
			found = true;
			break;
		}
	}
	const char *next = found ? AudioPlayer_EqProfiles[(idx + 1) % count].name : AudioPlayer_EqProfiles[0].name;
	AudioPlayer_SetEqualizerProfile(next);
}

// Returns the name of the currently stored equalizer profile.
String AudioPlayer_GetEqualizerProfile(void) {
	return gPrefsSettings.getString("eqProfile", "flat");
}

// Per-path equalizer rules: a file or directory path is mapped to a set of EQ gains.
// Rules are stored as a compact JSON array in NVS (key "eqRules") and cached here.
struct EqRule {
	String path;
	int8_t low;
	int8_t band;
	int8_t high;
};
static std::vector<EqRule> AudioPlayer_EqRules; // guarded by AudioPlayer_EqRulesMutex (declared near the top of this file)

// (Re)load the per-path equalizer rules from NVS into the in-memory cache.
void AudioPlayer_ReloadEqRules(void) {
	// Parse into a local vector first (no lock held during the NVS read / JSON deserialize), then take the
	// mutex only to swap it in, so a concurrent reader never observes the cache empty or half-built.
	std::vector<EqRule> rules;
	String json = gPrefsSettings.getString("eqRules", "[]");
	JsonDocument doc;
	if (deserializeJson(doc, json) == DeserializationError::Ok && doc.is<JsonArray>()) {
		for (JsonObject o : doc.as<JsonArray>()) {
			EqRule r;
			r.path = o["p"].as<String>();
			r.low = o["l"] | 0;
			r.band = o["b"] | 0;
			r.high = o["h"] | 0;
			if (r.path.length() > 0) {
				rules.push_back(r);
			}
		}
	}
	if (AudioPlayer_EqRulesMutex) {
		xSemaphoreTake(AudioPlayer_EqRulesMutex, portMAX_DELAY);
	}
	AudioPlayer_EqRules.swap(rules);
	if (AudioPlayer_EqRulesMutex) {
		xSemaphoreGive(AudioPlayer_EqRulesMutex);
	}
}

// Apply the equalizer for the given track path: use the longest matching per-path rule
// (exact file or directory prefix), otherwise fall back to the global equalizer from NVS.
void AudioPlayer_ApplyEqualizerForPath(const char *trackPath) {
	int8_t low = gPrefsSettings.getChar("gainLowPass", 0);
	int8_t band = gPrefsSettings.getChar("gainBandPass", 0);
	int8_t high = gPrefsSettings.getChar("gainHighPass", 0);
	if (trackPath != nullptr) {
		const String tp = trackPath;
		size_t bestLen = 0;
		// Guard the read against a concurrent AudioPlayer_ReloadEqRules() swap on the web task.
		if (AudioPlayer_EqRulesMutex) {
			xSemaphoreTake(AudioPlayer_EqRulesMutex, portMAX_DELAY);
		}
		for (const EqRule &r : AudioPlayer_EqRules) {
			const bool isMatch = (tp == r.path)
				|| (tp.length() > r.path.length() && tp.startsWith(r.path) && tp.charAt(r.path.length()) == '/');
			if (isMatch && r.path.length() > bestLen) {
				bestLen = r.path.length();
				low = r.low;
				band = r.band;
				high = r.high;
			}
		}
		if (AudioPlayer_EqRulesMutex) {
			xSemaphoreGive(AudioPlayer_EqRulesMutex);
		}
	}
	gPlayProperties.gainLowPass = low;
	gPlayProperties.gainBandPass = band;
	gPlayProperties.gainHighPass = high;
	audio->setTone(low, band, high);
}

// Pauses playback if playback is active and volume is changes from minVolume+1 to minVolume (usually 0)
void AudioPlayer_PauseOnMinVolume(const uint8_t oldVolume, const uint8_t newVolume) {
	if (gPlayProperties.pauseOnMinVolume) {
		if (gPlayProperties.playMode == BUSY || gPlayProperties.playMode == NO_PLAYLIST) {
			return;
		}

		if (!gPlayProperties.pausePlay) { // Volume changes from 1 to 0
			if (oldVolume == AudioPlayer_GetMinVolume() + 1 && newVolume == AudioPlayer_GetMinVolume()) {
				Cmd_Action(CMD_PLAYPAUSE, true); // auto-pause must work even while controls are locked
			}
		}
		if (gPlayProperties.pausePlay) { // Volume changes from 0 to 1
			if (oldVolume == AudioPlayer_GetMinVolume() && newVolume > AudioPlayer_GetMinVolume()) {
				Cmd_Action(CMD_PLAYPAUSE, true); // auto-pause must work even while controls are locked
			}
		}
	}
}

void AudioPlayer_PlayReadyMsg(void) {
	if (!gPrefsSettings.getBool("playStartupSnd", true)) {
		return; // startup sound disabled in the web settings
	}
	if (audio != nullptr) {
		String path = gPrefsSettings.getString("readyPath", "/ready.mp3");
		if (path.length() > 0 && gFSystem.exists(path)) {
			Log_Printf(LOGLEVEL_INFO, "Audio: Playing ready sound from %s", path.c_str());
			audio->connecttoFS(gFSystem, path.c_str());
		} else {
			Log_Printf(LOGLEVEL_NOTICE, "Audio: Ready sound not found at %s", path.c_str());
		}
	}
}

// Receives de-serialized RFID-data (from NVS) and dispatches playlists for the given
// playmode to the track-queue.
void AudioPlayer_SetPlaylist(const char *_itemToPlay, const uint32_t _lastPlayPos, const uint32_t _playMode, const uint16_t _trackLastPlayed) {
	// Daily listening-time limit (screentime): refuse to start a new playlist once today's limit
	// is reached. Enforced only while the clock is valid (see AudioPlayer_DailyLimitReached), so
	// a device without a trustworthy date never locks out. Checked first, before the running book
	// is paused/position-saved below, so a blocked tap leaves the current playback untouched.
	if (AudioPlayer_DailyLimitReached()) {
		Log_Println("Daily listening limit reached - not starting playback", LOGLEVEL_NOTICE);
		System_IndicateError();
		return;
	}

	// Make sure last playposition for audiobook is saved when new RFID-tag is applied. Skip this while
	// an IP/time announcement is active: the speech-abort branch in AudioPlayer_Loop() would consume the
	// PAUSEPLAY command below (so pausePlay would never be set and the wait would spin forever), and the
	// book's position was already saved when the announcement started, so there is nothing to save here.
	if (gPlayProperties.SavePlayPosRfidChange && !gPlayProperties.pausePlay && !gPlayProperties.currentSpeechActive && (gPlayProperties.playMode == AUDIOBOOK || gPlayProperties.playMode == AUDIOBOOK_LOOP || gPlayProperties.playMode == AUDIOBOOK_RECURSIVE)) {
		AudioPlayer_SetTrackControl(PAUSEPLAY);
		// Bounded (~5 s): make sure playback is paused so the position lands in NVS, but never spin the
		// loop task forever if the PAUSEPLAY command gets swallowed for any reason.
		for (uint8_t i = 0; !gPlayProperties.pausePlay && i < 50u; i++) {
			AudioPlayer_Loop();
			vTaskDelay(portTICK_PERIOD_MS * 100u);
		}
	}

	gPlayProperties.startAtFilePos = _lastPlayPos;
	gPlayProperties.currentTrackNumber = _trackLastPlayed;
	std::optional<Playlist *> musicFiles;
	String folderPath = _itemToPlay;

	if (_playMode != WEBSTREAM) {
		// Every directory playmode recurses into subdirectories: a card assigned to a folder plays
		// everything in that folder *and* its subfolders (capped by SdCard_GetMaxRecursionDepth()).
		// Single-track and m3u modes ignore the depth entirely, so passing it is harmless there.
		// The dedicated *_RECURSIVE modes (15/16/17) still exist for backwards compatibility and now
		// behave identically to their non-recursive counterparts.
		if (_playMode == RANDOM_SUBDIRECTORY_OF_DIRECTORY || _playMode == RANDOM_SUBDIRECTORY_OF_DIRECTORY_ALL_TRACKS_OF_DIR_RANDOM) {
			folderPath = SdCard_pickRandomSubdirectory(_itemToPlay);
			if (!folderPath) {
				// If error occured while extracting random subdirectory
				musicFiles = std::nullopt;
			} else {
				musicFiles = SdCard_ReturnPlaylist(folderPath.c_str(), _playMode, SdCard_GetMaxRecursionDepth(), false); // Provide random subdirectory in order to enter regular playlist-generation
			}
		} else {
			musicFiles = SdCard_ReturnPlaylist(_itemToPlay, _playMode, SdCard_GetMaxRecursionDepth(), false);
		}
	} else {
		musicFiles = AudioPlayer_ReturnPlaylistFromWebstream(_itemToPlay);
	}

	// Catch if error occured (e.g. file not found)
	if (!musicFiles) {
		Log_Println(errorOccured, LOGLEVEL_ERROR);
		System_IndicateError();
		if (gPlayProperties.playMode != NO_PLAYLIST) {
			AudioPlayer_SetTrackControl(STOP);
		}
		return;
	}

	gPlayProperties.playMode = BUSY; // Show @Neopixel, if uC is busy with creating playlist
	Playlist *list = musicFiles.value();
	if (!list->size()) {
		Log_Println(noMp3FilesInDir, LOGLEVEL_NOTICE);
		System_IndicateError();
		if (!gPlayProperties.pausePlay) {
			AudioPlayer_SetTrackControl(STOP);
			while (!gPlayProperties.pausePlay) {
				AudioPlayer_Loop();
				vTaskDelay(portTICK_PERIOD_MS * 10u);
			}
		}

		gPlayProperties.playMode = NO_PLAYLIST;
		freePlaylist(list);
		return;
	}

	// Set some default-values
	gPlayProperties.repeatCurrentTrack = false;
	gPlayProperties.repeatPlaylist = false;
	gPlayProperties.sleepAfterCurrentTrack = false;
	gPlayProperties.sleepAfterPlaylist = false;
	gPlayProperties.saveLastPlayPosition = false;
	gPlayProperties.playUntilTrackNumber = 0;

	// Store last RFID-tag to NVS
	gPrefsSettings.putString("lastRfid", gCurrentRfidTagId);

	bool error = false;
	switch (_playMode) {
		case SINGLE_TRACK: {
			Log_Println(modeSingleTrack, LOGLEVEL_NOTICE);

			break;
		}

		case SINGLE_TRACK_LOOP: {
			gPlayProperties.repeatCurrentTrack = true;
			gPlayProperties.repeatPlaylist = true;
			Log_Println(modeSingleTrackLoop, LOGLEVEL_NOTICE);
			break;
		}

		case SINGLE_TRACK_OF_DIR_RANDOM: {
			gPlayProperties.sleepAfterCurrentTrack = true;
			gPlayProperties.playUntilTrackNumber = 0;
			Led_SetNightmode(true);
			Log_Println(modeSingleTrackRandom, LOGLEVEL_NOTICE);
			AudioPlayer_RandomizePlaylist(list);
			// we have a random order, so pick the first entry and scrap the rest
			auto first = list->at(0);
			list->at(0) = nullptr; // prevent our entry from being destroyed
			freePlaylist(list); // this also scrapped our vector --> recreate it
			list = allocatePlaylist();
			list->push_back(first);
			break;
		}

		case AUDIOBOOK: { // Tracks need to be sorted!
			gPlayProperties.saveLastPlayPosition = true;
			Log_Println(modeSingleAudiobook, LOGLEVEL_NOTICE);
			AudioPlayer_SortPlaylist(list);
			break;
		}

		case AUDIOBOOK_LOOP: { // Tracks need to be sorted!
			gPlayProperties.repeatPlaylist = true;
			gPlayProperties.saveLastPlayPosition = true;
			Log_Println(modeSingleAudiobookLoop, LOGLEVEL_NOTICE);
			AudioPlayer_SortPlaylist(list);
			break;
		}

		case AUDIOBOOK_RECURSIVE: { // Tracks need to be sorted!
			gPlayProperties.saveLastPlayPosition = true;
			Log_Println(modeAudiobookRecursive, LOGLEVEL_NOTICE);
			AudioPlayer_SortPlaylist(list);
			break;
		}

		case ALL_TRACKS_OF_DIR_SORTED_RECURSIVE: {
			Log_Printf(LOGLEVEL_NOTICE, modeAllTrackAlphSortedRecursive, folderPath.c_str());
			AudioPlayer_SortPlaylist(list);
			break;
		}

		case ALL_TRACKS_OF_DIR_SORTED:
		case RANDOM_SUBDIRECTORY_OF_DIRECTORY: {
			Log_Printf(LOGLEVEL_NOTICE, modeAllTrackAlphSorted, folderPath.c_str());
			AudioPlayer_SortPlaylist(list);
			break;
		}

		case ALL_TRACKS_OF_DIR_RANDOM_RECURSIVE: {
			Log_Printf(LOGLEVEL_NOTICE, modeAllTrackRandomRecursive, folderPath.c_str());
			AudioPlayer_RandomizePlaylist(list);
			break;
		}

		case ALL_TRACKS_OF_DIR_RANDOM:
		case RANDOM_SUBDIRECTORY_OF_DIRECTORY_ALL_TRACKS_OF_DIR_RANDOM: {
			Log_Printf(LOGLEVEL_NOTICE, modeAllTrackRandom, folderPath.c_str());
			AudioPlayer_RandomizePlaylist(list);
			break;
		}

		case ALL_TRACKS_OF_DIR_SORTED_LOOP: {
			gPlayProperties.repeatPlaylist = true;
			Log_Println(modeAllTrackAlphSortedLoop, LOGLEVEL_NOTICE);
			AudioPlayer_SortPlaylist(list);
			break;
		}

		case ALL_TRACKS_OF_DIR_RANDOM_LOOP: {
			gPlayProperties.repeatPlaylist = true;
			Log_Println(modeAllTrackRandomLoop, LOGLEVEL_NOTICE);
			AudioPlayer_RandomizePlaylist(list);
			break;
		}

		case WEBSTREAM: { // This is always just one "track"
			Log_Println(modeWebstream, LOGLEVEL_NOTICE);
			if (!Wlan_IsConnected()) {
				Log_Println(webstreamNotAvailable, LOGLEVEL_ERROR);
				// Remember this tag and retry automatically once WiFi is connected (e.g. webradio-tag applied at boot)
				AudioPlayer_RememberRfidForWifiRetry(gCurrentRfidTagId);
				error = true;
			}
			break;
		}

		case LOCAL_M3U: { // Can be one or multiple SD-files or webradio-stations; or a mix of both
			Log_Println(modeWebstreamM3u, LOGLEVEL_NOTICE);
			break;
		}

		default:
			Log_Printf(LOGLEVEL_ERROR, modeInvalid, gPlayProperties.playMode);
			error = true;
	}

	if (!error) {
		gPlayProperties.playMode = _playMode;
		newPlayList = list; // publish the payload *before* the flag so the consumer never sees a stale pointer
		newPlayListAvailable = true;
		return;
	}

	// we had an error, blink and destroy playlist
	gPlayProperties.playMode = NO_PLAYLIST;
	System_IndicateError();
	freePlaylist(list);
}

/* Wraps putString for writing settings into NVS for RFID-cards.
   Returns number of characters written. */
size_t AudioPlayer_NvsRfidWriteWrapper(const char *_rfidCardId, const uint32_t _playPosition, const uint8_t _playMode, const uint16_t _trackLastPlayed) {
	if (_playMode == NO_PLAYLIST) {
		// writing back to NVS with NO_PLAYLIST seems to be a bug - Todo: Find the cause here
		Log_Printf(LOGLEVEL_ERROR, modeInvalid, _playMode);
		return 0;
	}
	Led_SetPause(true); // Workaround to prevent exceptions due to Neopixel-signalisation while NVS-write
	char firstPart[275] = {0};
	char prefBuf[275];

	// Don't create a "resume a few seconds in" point when an audiobook is only briefly sampled: if the
	// card is pulled within the first <minResumeSec> seconds of the very first track, persist position 0
	// so the next play starts cleanly from the beginning instead of a couple of seconds in. Gated to the
	// position-saving audiobook modes and track 0, so genuine mid-book progress is never discarded.
	// 0 = off. Default 20 s.
	uint32_t playPosition = _playPosition;
	if (_trackLastPlayed == 0 && _playPosition > 0 && (_playMode == AUDIOBOOK || _playMode == AUDIOBOOK_LOOP || _playMode == AUDIOBOOK_RECURSIVE)) {
		if (gMinResumeSec > 0 && _playPosition < gMinResumeSec) {
			Log_Printf(LOGLEVEL_NOTICE, "Audiobook pulled within first %u s -> resetting resume point to start", gMinResumeSec);
			playPosition = 0;
		}
	}

	// Songs shouldn't resume mid-track: when the track being saved is shorter than
	// <shortTrackSec> it is considered a song (music-album card) rather than an audiobook
	// chapter, so persist position 0 — re-applying the card restarts the current song from
	// its beginning while the track number is kept. Long chapter files keep their exact
	// resume position. Uses the cached duration of the currently playing track; when the
	// duration isn't known (yet), the exact position is kept. 0 = off. Default 300 s.
	if (playPosition > 0 && (_playMode == AUDIOBOOK || _playMode == AUDIOBOOK_LOOP || _playMode == AUDIOBOOK_RECURSIVE)) {
		if (gShortTrackSec > 0 && AudioPlayer_FileDuration > 0 && AudioPlayer_FileDuration < gShortTrackSec) {
			Log_Printf(LOGLEVEL_DEBUG, "track shorter than %u s -> resume point set to track start", gShortTrackSec);
			playPosition = 0;
		}
	}

	gPrefsRfid.getString(_rfidCardId, firstPart, sizeof(firstPart)); // read back previous value from NVS

	// Remove everything after the first part (after the first stringDelimiter)
	char *pos = strchr(firstPart + strlen(stringDelimiter), stringDelimiter[0]);
	if (pos != NULL) {
		*pos = '\0'; // Terminate the string at this position
	}

	// Build the new string with the preserved first part (which already contains the track)
	snprintf(prefBuf, sizeof(prefBuf), "%s%s%" PRIu32 "%s%d%s%" PRIu16, firstPart, stringDelimiter, playPosition, stringDelimiter, _playMode, stringDelimiter, _trackLastPlayed);

	Log_Printf(LOGLEVEL_INFO, wroteLastTrackToNvs, prefBuf, _rfidCardId, _playMode, _trackLastPlayed);
	Log_Println(prefBuf, LOGLEVEL_INFO);
	// keep the LED task paused THROUGH the flash write (that is the whole point of the
	// pause); releasing it before putString re-armed the NeoPixel during the write
	const size_t written = gPrefsRfid.putString(_rfidCardId, prefBuf);
	Led_SetPause(false);
	return written;

	// Examples for serialized RFID-actions that are stored in NVS
	// #<file/folder>#<startPlayPositionInBytes>#<playmode>#<trackNumberToStartWith>
	// Please note: There's no need to do this manually (unless you want to)
	/*gPrefsRfid.putString("215123125075", "#/mp3/Kinderlieder#0#6#0");
	gPrefsRfid.putString("169239075184", "#http://radio.koennmer.net/evosonic.mp3#0#8#0");
	gPrefsRfid.putString("244105171042", "#0#0#111#0"); // modification-card (repeat track)
	gPrefsRfid.putString("228064156042", "#0#0#110#0"); // modification-card (repeat playlist)
	gPrefsRfid.putString("212130160042", "#/mp3/Hoerspiele/Yakari/Sammlung2#0#3#0");*/
}

// Resets a tag's saved play-position and last-played track to the start, keeping the
// folder/file and play mode intact (so an audiobook can be restarted from the beginning).
void AudioPlayer_ResetRfidPos(const char *_rfidCardId, const uint8_t _playMode) {
	AudioPlayer_NvsRfidWriteWrapper(_rfidCardId, 0, _playMode, 0);
}

// Adds webstream to playlist; same like SdCard_ReturnPlaylist() but always only one entry
std::optional<Playlist *> AudioPlayer_ReturnPlaylistFromWebstream(const char *_webUrl) {
	Playlist *playlist = allocatePlaylist();
	const size_t len = strlen(_webUrl) + 1;
	char *entry = static_cast<char *>(x_malloc(len));
	if (!entry) {
		// OOM
		Log_Println(unableToAllocateMemForLinearPlaylist, LOGLEVEL_ERROR);
		freePlaylist(playlist);
		return std::nullopt;
	}
	strncpy(entry, _webUrl, len);
	entry[len - 1] = '\0';
	playlist->push_back(entry);

	return playlist;
}

// Adds new control-command to control-queue
void AudioPlayer_SetTrackControl(const uint8_t new_trackCommand) {
	// A running HTTP sync pauses playback for the duration of the SD-writing phase.
	// As soon as the user asks for playback again (press play / skip), cancel the
	// sync so the SD card is freed and playback can resume.
	if (Sync_GetStatus() == 1) {
		const bool wantsPlayback = (new_trackCommand == PLAY)
			|| (new_trackCommand == NEXTTRACK)
			|| (new_trackCommand == PREVIOUSTRACK)
			|| (new_trackCommand == SMARTFORWARD)
			|| (new_trackCommand == SMARTBACKWARD)
			|| ((new_trackCommand == PAUSEPLAY) && gPlayProperties.pausePlay); // PAUSEPLAY while paused == resume
		if (wantsPlayback) {
			Sync_Cancel();
		}
	}
	trackCommand = new_trackCommand;
}

// Knuth-Fisher-Yates-algorithm to randomize playlist
void AudioPlayer_RandomizePlaylist(Playlist *playlist) {
	if (playlist->size() < 2) {
		// we can not randomize less than 2 entries
		return;
	}

	// randomize using the "normal" random engine and shuffle
	std::default_random_engine rnd(millis());
	std::shuffle(playlist->begin(), playlist->end(), rnd);
}

// Helper to sort playlist - standard string comparison
static bool AudioPlayer_ArrSortHelper_strcmp(const char *a, const char *b) {
	return strcmp(a, b) < 0;
}

// Helper to sort playlist - natural case-sensitive
static bool AudioPlayer_ArrSortHelper_strnatcmp(const char *a, const char *b) {
	return strnatcmp(a, b) < 0;
}

// Helper to sort playlist - natural case-insensitive
static bool AudioPlayer_ArrSortHelper_strnatcasecmp(const char *a, const char *b) {
	return strnatcasecmp(a, b) < 0;
}

// Sort playlist
void AudioPlayer_SortPlaylist(Playlist *playlist) {
	std::function<bool(const char *, const char *)> cmpFunc;
	const char *mode;
	switch (AudioPlayer_PlaylistSortMode) {
		case playlistSortMode::STRCMP:
			cmpFunc = AudioPlayer_ArrSortHelper_strcmp; // standard string comparison
			mode = "standard string compare";
			break;
		case playlistSortMode::STRNATCMP:
			cmpFunc = AudioPlayer_ArrSortHelper_strnatcmp; // natural case-sensitive
			mode = "case-sensitive natural sorting";
			break;
		case playlistSortMode::STRNATCASECMP:
		default:
			cmpFunc = AudioPlayer_ArrSortHelper_strnatcasecmp; // natural case-insensitive
			mode = "case-insensitive natural sorting";
			break;
	}

	Log_Printf(LOGLEVEL_INFO, "Sorting files using %s", mode, "\n");
	std::sort(playlist->begin(), playlist->end(), cmpFunc);
	/*for (const char *str : *playlist) {
		Serial.println(str);
	}*/
}

// Clear cover send notification
void AudioPlayer_ClearCover(void) {
	gPlayProperties.coverFilePos = 0;
	gPlayProperties.audioFileSize = 0;
	AudioPlayer_StationLogoUrl = "";
	// reset per-track metadata too (called at the start of every track), so artist/album from a
	// previous track don't linger if the new one carries no ID3/Vorbis tags
	gPlayProperties.artist[0] = '\0';
	gPlayProperties.album[0] = '\0';
	// websocket and mqtt notify cover image has changed
	Web_SendWebsocketData(0, WebsocketCodeType::CoverImg);
#ifdef MQTT_ENABLE
	publishMqtt(topicCoverChanged, "", false);
	publishMqtt(topicTrackArtist, "", false); // reset now-playing metadata for the next track
	publishMqtt(topicTrackAlbum, "", false);
#endif
}

// id3 tag: save cover image
void audio_id3image(File &file, const size_t pos, const size_t size) {
	// save cover image position and size for later use
	gPlayProperties.coverFilePos = pos;
	gPlayProperties.coverFileSize = size;
	// websocket and mqtt notify cover image has changed
	Web_SendWebsocketData(0, WebsocketCodeType::CoverImg);
#ifdef MQTT_ENABLE
	publishMqtt(topicCoverChanged, "", false);
#endif
}

// encoded blockpicture cover image segments (all ogg, vorbis, opus files, some flac files)
void audio_oggimage(File &file, std::vector<uint32_t> v) {
	// save decoded cover in /.cache/file.path()
	String decodedCover = "/.cache";
	decodedCover.concat(file.path());
	if (gFSystem.exists(decodedCover)) {
		Log_Printf(LOGLEVEL_DEBUG, "Cover already cached in %s", decodedCover.c_str());
	} else {
		String tmpDecodedCover = decodedCover.substring(0, decodedCover.lastIndexOf('/') + 1); // to prevent coverFile corruption write into temporary file; fixed name since no parallel usage of audioI2S
		tmpDecodedCover.concat(".tmp");
		File coverFile = gFSystem.open(tmpDecodedCover, FILE_WRITE, true); // open file with create=true to make sure parent directories are created
		if (!coverFile) {
			return;
		}

		// write fLaC marker in order to use flac Routine, since decoded cover has METADATA_BLOCK_PICTURE like flac
		constexpr uint8_t flacMarker[] = "fLaC";
		coverFile.write(flacMarker, std::char_traits<uint8_t>::length(flacMarker));

		const size_t chunkSize = 2048; // must be base64 compatible, i.e. a multiple of 4
		uint8_t *encodedChunk = (uint8_t *) x_malloc(chunkSize);
		if (!encodedChunk) {
			// OOM: bail out cleanly instead of dereferencing a null pointer (crash)
			Log_Println(unableToAllocateMem, LOGLEVEL_ERROR);
			coverFile.close();
			gFSystem.remove(tmpDecodedCover);
			return;
		}
		size_t decodedLength;
		size_t currentRemainder = 0;
		size_t currentPosition = file.position(); // save current position in audio file otherwise playback will result in an error

		for (size_t i = 0; i < v.size(); i += 2) {
			// calculate the number of chunks needed to read the segment
			size_t numChunks = (v[i + 1] + currentRemainder) / chunkSize;
			size_t remainder = currentRemainder;

			// read and decode the segment chunk by chunk, write decodedChunk into encodedChunk to save memory
			file.seek(v[i]);
			for (size_t chunk = 0; chunk < numChunks; chunk++) {
				file.readBytes(reinterpret_cast<char *>(&encodedChunk[remainder]), chunkSize - remainder);
				decodedLength = b64decode(encodedChunk, encodedChunk, chunkSize);
				coverFile.write(encodedChunk, decodedLength);
				remainder = 0;
			}

			// calculate new remainder, read it, and if it is the end of file, decode it
			currentRemainder = (v[i + 1] + currentRemainder) % chunkSize;
			if (currentRemainder) {
				file.readBytes(reinterpret_cast<char *>(&encodedChunk[remainder]), currentRemainder - remainder);
				if (i == v.size() - 2) {
					decodedLength = b64decode(encodedChunk, encodedChunk, currentRemainder);
					coverFile.write(encodedChunk, decodedLength);
				}
			}
		}
		free(encodedChunk);
		coverFile.close();
		file.seek(currentPosition);
		gFSystem.rename(tmpDecodedCover, decodedCover);
		Log_Printf(LOGLEVEL_DEBUG, "Cover decoded and cached in %s", decodedCover.c_str());
	}
	gPlayProperties.coverFilePos = 4; // flacMarker gives 4 Bytes before METADATA_BLOCK_PICTURE (audioI2S points to METADATA_BLOCK_PICTURE since 6241daa)
	// websocket and mqtt notify cover image has changed
	Web_SendWebsocketData(0, WebsocketCodeType::CoverImg);
#ifdef MQTT_ENABLE
	publishMqtt(topicCoverChanged, "", false);
#endif
}

// record audiodata or send via BT
void audio_process_i2s(int32_t *outBuff, int16_t validSamples, bool *continueI2S) {
	if ((System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE) && Bluetooth_Device_Connected()) {
		// do downsamling to 16bit and send via BT
		int16_t *outBuff16 = reinterpret_cast<int16_t *>(outBuff);
		for (int16_t i = 0; i < validSamples * 2; i++) {
			outBuff16[i] = outBuff16[i * 2 + 1];
		}

		Bluetooth_Source_SendAudioData(outBuff16, validSamples);
		*continueI2S = false;
		return;
	}

	*continueI2S = true;
}
