#pragma once

#include <Arduino.h>

// Apple HomeKit accessory support (control + Siri) via HomeSpan.
// Gated behind HOMEKIT_ENABLE in settings.h. The HomeSpan poll task is pinned
// to core 0 (audio decode lives on core 1), so it never starves the I2S
// pipeline. See HomeKit.cpp for the threading model.

void HomeKit_Init(void);

// Call from the main loop (core 1): applies HomeKit-triggered intents and
// mirrors ESPuino's state back to the Home app. No-op unless HOMEKIT_ENABLE.
void HomeKit_Cyclic(void);

// --- Web interface helpers (served by the HomeKit settings section) ----------
bool HomeKit_IsEnabled(void); // runtime on/off (persisted); always false when compiled out
void HomeKit_SetEnabled(bool enabled); // persist the on/off switch; takes effect on next reboot
bool HomeKit_IsPaired(void); // at least one admin controller paired
const char *HomeKit_GetSetupCode(void); // formatted "466-37-726"
String HomeKit_GetSetupPayload(void); // "X-HM://..." pairing URI
String HomeKit_GetQrSvg(void); // standalone SVG of the pairing QR code
void HomeKit_ResetPairing(void); // remove all paired controllers (no reboot)
void HomeKit_RequestRegenerate(void); // generate a fresh random pairing code
String HomeKit_GetDeviceName(void); // bridge/device name (follows the hostname)
String HomeKit_GetTvName(void); // TV / remote name (follows the hostname)
