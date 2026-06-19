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
	WebdavStatus
} WebsocketCodeType;

void Web_Cyclic(void);
void Web_Exit(void);
void Web_SendWebsocketData(uint32_t client, WebsocketCodeType code);
void Web_TriggerGithubOta(void);
const char *Web_GetGithubOtaStatusText(void);
// GitHub OTA / passive version-check state (implemented in WebOta.cpp).
uint8_t Web_GetGithubOtaStatus(void);
uint8_t Web_GetGithubOtaProgress(void);
void Web_GetGithubOtaMessage(char *dst, size_t dstLen); // thread-safe copy of the OTA status message
void Web_CheckForUpdate(void);
int8_t Web_GetFirmwareUpToDate(void); // -1 unknown, 0 update available, 1 up to date
void Web_GetLatestBuild(char *dst, size_t dstLen);
