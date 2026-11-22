#include <Arduino.h>
#include "settings.h"

#include "Ftp.h"

#include "Log.h"
#include "MemX.h"
#include "SdCard.h"
#include "System.h"
#include "Wlan.h"

#include <WiFi.h>

#ifdef FTP_ENABLE
	#include "ESP-FTP-Server-Lib.h"
#endif

// FTP
String Ftp_User = "esp32"; // FTP-user (default; can be changed later via GUI)
String Ftp_Password = "esp32"; // FTP-password (default; can be changed later via GUI)

// FTP
#ifdef FTP_ENABLE
FTPServer *ftpSrv; // Heap-alloction takes place later (when needed)
bool ftpEnableLastStatus = false;
bool ftpEnableCurrentStatus = false;
static bool ftpAutostart = false; // start automatically on boot (persisted setting "ftpEnable")
// Deferred-teardown flags: ftpSrv may only be deleted by the loop task (Ftp_Cyclic),
// never by the async_tcp web/websocket task, which could delete it mid-handle() -> UAF.
// The web-task entry points set these; Ftp_Cyclic() acts on them between handle() calls.
static volatile bool ftpStopPending = false; // request: stop the server
static volatile bool ftpRestartPending = false; // request: re-create the server (apply new credentials)
#endif

void ftpManager(void);

void Ftp_Init(void) {
	String info;
	// Get FTP-user from NVS
	String nvsFtpUser = gPrefsSettings.getString("ftpuser", "-1");
	if (!nvsFtpUser.compareTo("-1")) {
		gPrefsSettings.putString("ftpuser", (String) Ftp_User);
		Log_Println(wroteFtpUserToNvs, LOGLEVEL_ERROR);
	} else {
		Ftp_User = nvsFtpUser;
		Log_Printf(LOGLEVEL_INFO, restoredFtpUserFromNvs, nvsFtpUser.c_str());
	}

	// Get FTP-password from NVS
	String nvsFtpPassword = gPrefsSettings.getString("ftppassword", "-1");
	if (!nvsFtpPassword.compareTo("-1")) {
		gPrefsSettings.putString("ftppassword", (String) Ftp_Password);
		Log_Println(wroteFtpPwdToNvs, LOGLEVEL_ERROR);
	} else {
		Ftp_Password = nvsFtpPassword;
		Log_Println(restoredFtpPwdFromNvs, LOGLEVEL_INFO);
	}

#ifdef FTP_ENABLE
	ftpAutostart = gPrefsSettings.getBool("ftpEnable", false);
#endif
}

// Requests a deferred credentials reload on the loop task. Called from async_tcp
// when the shared device password changes via the web UI so we never mutate Ftp_User /
// Ftp_Password String objects across cores while loopTask is inside ftpSrv->handle().
void Ftp_ReloadCredentials(void) {
#ifdef FTP_ENABLE
	ftpRestartPending = true;
#endif
}

void Ftp_Exit(void) {
#ifdef FTP_ENABLE
	if (ftpEnableCurrentStatus) {
		delete ftpSrv;
		ftpSrv = NULL;
		ftpEnableCurrentStatus = false;
		ftpEnableLastStatus = false;
	}
#endif
}

void Ftp_Cyclic(void) {
#ifdef FTP_ENABLE
	// One-shot auto-start: when the persisted setting asks for it, fire the server up the first time
	// WiFi is available after boot, then never touch it again (so a manual stop isn't overridden).
	static bool autostartHandled = false;
	if (ftpAutostart && !autostartHandled && Wlan_IsConnected()) {
		autostartHandled = true;
		Ftp_EnableServer();
	}

	if (ftpRestartPending) {
		Ftp_User = gPrefsSettings.getString("ftpuser", Ftp_User);
		Ftp_Password = gPrefsSettings.getString("ftppassword", Ftp_Password);
	}

	// Consume deferred teardown requests raised by the web/websocket task. We are on the
	// loop task here and outside ftpSrv->handle() (called further down), so deleting the
	// server object is safe -- no transfer can be in flight on this task at this point.
	if ((ftpStopPending || ftpRestartPending) && (ftpSrv != NULL)) {
		delete ftpSrv; // also closes the listening socket and all client connections
		ftpSrv = NULL;
		ftpEnableCurrentStatus = false;
		Log_Println(ftpServerStopped, LOGLEVEL_NOTICE);
	}
	if (ftpStopPending) {
		// A stop clears the "requested" flag too; a restart leaves ftpEnableLastStatus set so
		// ftpManager() re-creates the server below with the updated credentials.
		ftpEnableLastStatus = false;
	}
	ftpStopPending = false;
	ftpRestartPending = false;

	ftpManager();

	if (Wlan_IsConnected()) {
		if (ftpEnableLastStatus && ftpEnableCurrentStatus && ftpSrv != NULL) {
			ftpSrv->handle();
		}
	}

	if (ftpEnableLastStatus && ftpEnableCurrentStatus && ftpSrv != NULL) {
		if (ftpSrv->hasActiveSession(60000UL)) {
			System_UpdateActivityTimer(); // Keep device awake during active FTP transfers/commands, not forever on idle sockets
		}
	}
#endif
}

void Ftp_EnableServer(void) {
#ifdef FTP_ENABLE
	if (Wlan_IsConnected() && !ftpEnableLastStatus && !ftpEnableCurrentStatus) {
		ftpEnableLastStatus = true;
#else
	if (Wlan_IsConnected()) {
#endif

		System_IndicateOk();
	} else {
		Log_Println(unableToStartFtpServer, LOGLEVEL_ERROR);
		System_IndicateError();
	}
}

void Ftp_DisableServer(void) {
#ifdef FTP_ENABLE
	if (ftpEnableLastStatus || ftpEnableCurrentStatus) {
		// Called from the websocket handler on the async_tcp task: don't delete ftpSrv
		// here (the loop task may be inside ftpSrv->handle() -> UAF). Flip the "requested"
		// flag so Ftp_IsServerRunning()/the status broadcast report "stopped" immediately,
		// and hand the actual delete off to Ftp_Cyclic() on the loop task.
		ftpEnableLastStatus = false;
		ftpStopPending = true;
		System_IndicateOk();
	} else {
		System_IndicateError();
	}
#endif
}

bool Ftp_IsServerRunning(void) {
#ifdef FTP_ENABLE
	return ftpEnableLastStatus;
#else
	return false;
#endif
}

// Creates FTP-instance only when requested
void ftpManager(void) {
#ifdef FTP_ENABLE
	if (ftpEnableLastStatus && !ftpEnableCurrentStatus) {
		Log_Printf(LOGLEVEL_DEBUG, freeHeapWithoutFtp, ESP.getFreeHeap());
		ftpEnableCurrentStatus = true;
		ftpSrv = new FTPServer();
		ftpSrv->addUser(Ftp_User, Ftp_Password);
		ftpSrv->addFilesystem("SD-Card", &gFSystem);
		ftpSrv->begin();
		Log_Printf(LOGLEVEL_DEBUG, freeHeapWithFtp, ESP.getFreeHeap());
		Log_Println(ftpServerStarted, LOGLEVEL_NOTICE);
	}
#endif
}
