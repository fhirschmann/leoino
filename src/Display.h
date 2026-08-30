#pragma once

#include <stdint.h>

inline constexpr char OLED_MENU_ITEMS_DEFAULT[] = "status,ip,battery,sysinfo,equalizer,nightmode,webdav,fwupdate,shutdown";
inline constexpr uint8_t OLED_MENU_TIMEOUT_DEFAULT_SECONDS = 5;

void Display_Init(void);
void Display_Cyclic(void);
void Display_Exit(void);
void Display_ReloadConfig(void); // re-read the OLED settings from NVS and apply them live
void Display_Toggle(void); // flip the runtime enable flag (used by CMD_TOGGLE_OLED), persists to NVS
bool Display_IsEnabled(void); // current runtime enable state (reflects the NVS flag)
bool Display_MenuIsActive(void); // true while the OLED quick menu or one of its info pages is visible
bool Display_MenuPress(uint16_t *selectedCommand); // open/confirm/back; returns an executable command via selectedCommand
void Display_MenuRotate(int32_t detents); // move the quick-menu selection without changing volume
