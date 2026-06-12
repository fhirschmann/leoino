#pragma once

// Support for a monochrome 128x64 OLED (SH1106 / SSD1306) connected to the
// external I2C bus (ext_IIC_*). Enable by defining DISPLAY_ENABLE in your
// platform settings-*.h. The display can additionally be switched on/off at
// runtime via the web interface (General settings), persisted in NVS.

void Display_Init(void);
void Display_Cyclic(void);
void Display_Exit(void);

// Runtime enable/disable (persisted in NVS); no-op on builds without DISPLAY_ENABLE
void Display_SetEnabled(bool enabled);
bool Display_IsEnabled(void);
