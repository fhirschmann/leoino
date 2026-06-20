#pragma once

void Display_Init(void);
void Display_Cyclic(void);
void Display_Exit(void);
void Display_ReloadConfig(void); // re-read the OLED settings from NVS and apply them live
void Display_Toggle(void); // flip the runtime enable flag (used by CMD_TOGGLE_OLED), persists to NVS
bool Display_IsEnabled(void); // current runtime enable state (reflects the NVS flag)
