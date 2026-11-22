#pragma once

#include <stdint.h>

void RotaryEncoder_Init(void);
void RotaryEncoder_Cyclic(void);
void RotaryEncoder_InjectDetents(int16_t detents);
