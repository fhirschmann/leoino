#pragma once

typedef enum class WebsocketCode {
	Ok = 0,
	Error,
	Dropout,
	CurrentRfid,
	Pong,
	TrackInfo,
	CoverImg,
	Volume,
	Settings,
	Ssid,
	TrackProgress,
	OperationMode,
	NotAllowedInCurrentMode,
	BluetoothScanInProgress,
	BluetoothScanComplete,
	FtpStatus,
	WebdavStatus,
	IrLearn,
	WebPlayBlockedByTag, // web file-browser playback refused because an RFID tag is applied
	// Request was a read-only data fetch (ssids/settings/trackinfo/coverimg/volume/...) that
	// already sent its own specific response - the caller should not also forward Ok/an ack
	// for it, unlike a genuine settings-save or control action.
	Silent
} WebsocketCodeType;

void Web_Cyclic(void);
void Web_Exit(void);
void Web_SendWebsocketData(uint32_t client, WebsocketCodeType code);
void Web_NotifyIrCode(uint16_t code); // broadcast a freshly received IR code to the web UI (learn mode)
void Web_TriggerGithubOta(void);
// OTA-after-reboot flow (see WebOta.cpp): true while a rebooted-into OTA attempt owns this
// boot -- HomeKit_Cyclic() then holds HomeSpan back so the download has enough internal heap.
bool Web_OtaBootPending(void);
void Web_OtaCyclic(bool webserverUp);
const char *Web_GetGithubOtaStatusText(void);
// GitHub OTA / passive version-check state (implemented in WebOta.cpp).
uint8_t Web_GetGithubOtaStatus(void);
uint8_t Web_GetGithubOtaProgress(void);
void Web_GetGithubOtaMessage(char *dst, size_t dstLen); // thread-safe copy of the OTA status message
void Web_CheckForUpdate(void);
int8_t Web_GetFirmwareUpToDate(void); // -1 unknown, 0 update available, 1 up to date
void Web_GetLatestBuild(char *dst, size_t dstLen);
