#pragma once

typedef struct {
	bool lastState		 : 1;
	bool currentState	 : 1;
	bool isPressed		 : 1;
	bool isReleased		 : 1;
	bool usedAsModifier	 : 1; // A rotary gesture was performed while this button was held: swallow its normal short/long action
	bool holdRepeatFired : 1; // A repeatable long action fired: suppress the normal release action
	unsigned long lastPressedTimestamp;
	unsigned long lastReleasedTimestamp;
	unsigned long firstPressedTimestamp;
	unsigned long lastRepeatTimestamp;
} t_button;

extern uint8_t gShutdownButton;
extern bool gButtonInitComplete;

constexpr uint8_t BUTTON_NONE = 0xFFu;

// "Hold a button, turn the rotary encoder" gestures. Button_GetHeldModifier() returns the index of a
// button that is physically down right now AND has a rotary action configured, or BUTTON_NONE.
uint8_t Button_GetHeldModifier(void);
uint8_t Button_GetRotaryAction(uint8_t buttonIndex, bool clockwise);
uint8_t Button_GetRotaryActionDefault(uint8_t buttonIndex, bool clockwise);
void Button_MarkModifierUsed(uint8_t buttonIndex);

void Button_Init(void);
void Button_StartSampler(void); // start after shared I2C/peripherals are initialized
void Button_StopSampler(void); // stop before shutdown tears down the shared I2C bus
void Button_Cyclic(void);
