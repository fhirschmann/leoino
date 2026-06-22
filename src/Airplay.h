#pragma once

// AirPlay 1 (RAOP) audio receiver.
//
// Active only while the device is in OPMODE_AIRPLAY (see values.h). In that mode ESPuino advertises
// itself as an AirPlay speaker via mDNS (_raop._tcp) and plays an incoming AirPlay stream from an
// iPhone/iPad/Mac through the very same I2S DAC path the Bluetooth A2DP sink uses. Like the BT modes
// it is mutually exclusive with normal SD/webradio playback (a single DAC), so switching into/out of
// it reboots the device. The lifecycle mirrors Bluetooth_*.
//
// Implemented on top of the vendored `esp-raop-receiver` component (see components/esp-raop-receiver).

void Airplay_Init(void); // boot init: claims I2S when in AirPlay mode
void Airplay_Cyclic(void); // pumped from loop(): starts the RAOP receiver once WiFi/mDNS are up
void Airplay_Exit(void); // abandon on shutdown (reboot performs the real teardown)

bool Airplay_IsActive(void); // true when the operation mode is OPMODE_AIRPLAY
bool Airplay_IsRunning(void); // true while an AirPlay client is connected/streaming

void Airplay_Enable(void); // switch into AirPlay mode (reboots if the mode actually changes)
void Airplay_Disable(void); // switch back to normal mode (reboots if the mode actually changes)

const char *Airplay_GetDeviceName(void); // advertised name (resolved at Init from NVS/hostname)
const char *Airplay_GetNowPlaying(void); // "Artist - Title" of the current stream, or "" if none
