#include <Arduino.h>
#include "settings.h"

#include "Button.h"

#include "Cmd.h"
#include "Log.h"
#include "Port.h"
#include "System.h"

#include <atomic>
#include <freertos/task.h>

bool gButtonInitComplete = false;

// Only enable those buttons that are not disabled (99 or >115)
// 0 -> 39: GPIOs
// 100 -> 115: Port-expander
#if (NEXT_BUTTON >= 0 && NEXT_BUTTON <= MAX_GPIO)
	#define BUTTON_0_ENABLE
#elif (NEXT_BUTTON >= 100 && NEXT_BUTTON <= 115)
	#define EXPANDER_0_ENABLE
#endif
#if (PREVIOUS_BUTTON >= 0 && PREVIOUS_BUTTON <= MAX_GPIO)
	#define BUTTON_1_ENABLE
#elif (PREVIOUS_BUTTON >= 100 && PREVIOUS_BUTTON <= 115)
	#define EXPANDER_1_ENABLE
#endif
#if (PAUSEPLAY_BUTTON >= 0 && PAUSEPLAY_BUTTON <= MAX_GPIO)
	#define BUTTON_2_ENABLE
#elif (PAUSEPLAY_BUTTON >= 100 && PAUSEPLAY_BUTTON <= 115)
	#define EXPANDER_2_ENABLE
#endif
#if (ROTARYENCODER_BUTTON >= 0 && ROTARYENCODER_BUTTON <= MAX_GPIO)
	#define BUTTON_3_ENABLE
#elif (ROTARYENCODER_BUTTON >= 100 && ROTARYENCODER_BUTTON <= 115)
	#define EXPANDER_3_ENABLE
#endif
#if (BUTTON_4 >= 0 && BUTTON_4 <= MAX_GPIO)
	#define BUTTON_4_ENABLE
#elif (BUTTON_4 >= 100 && BUTTON_4 <= 115)
	#define EXPANDER_4_ENABLE
#endif
#if (BUTTON_5 >= 0 && BUTTON_5 <= MAX_GPIO)
	#define BUTTON_5_ENABLE
#elif (BUTTON_5 >= 100 && BUTTON_5 <= 115)
	#define EXPANDER_5_ENABLE
#endif
#if (BUTTON_6 >= 0 && BUTTON_6 <= MAX_GPIO)
	#define BUTTON_6_ENABLE
#elif (BUTTON_6 >= 100 && BUTTON_6 <= 115)
	#define EXPANDER_6_ENABLE
#endif

// Allocate gButtons in PSRAM if available
EXT_RAM_BSS_ATTR t_button gButtons[8]; // next + prev + pplay + rotEnc + button4 + button5 + button6 + dummy-button
uint8_t gShutdownButton = 99; // Helper used for Neopixel: stores button-number of shutdown-button
uint16_t gLongPressTime = 0;
static constexpr uint16_t smartHoldRepeatIntervalMs = 200; // match the IR remote's repeat cadence

static std::atomic<SemaphoreHandle_t> Button_TimerSemaphore;

// The audio player's track-open path is synchronous and can hold Arduino's main loop for a few
// hundred milliseconds. Sampling buttons from that loop loses a complete quick press/release made
// during the open. A tiny task therefore records debounced physical edges independently; the main
// loop still owns all command dispatch and consumes at most one action per pass.
struct ButtonEdge {
	uint32_t timestamp;
	uint8_t index;
	bool pressed;
};

static constexpr uint8_t buttonEdgeBufferSize = 64; // 31 complete clicks while the main loop is blocked
static constexpr uint8_t buttonEdgeBufferMask = buttonEdgeBufferSize - 1;
static_assert((buttonEdgeBufferSize & buttonEdgeBufferMask) == 0, "button edge buffer must be a power of two");
static ButtonEdge Button_EdgeBuffer[buttonEdgeBufferSize];
static std::atomic<uint8_t> Button_EdgeWriteIndex {0};
static std::atomic<uint8_t> Button_EdgeReadIndex {0};
static std::atomic<uint32_t> Button_DroppedEdges {0};
static std::atomic<TaskHandle_t> Button_SamplerTaskHandle {nullptr};
static std::atomic<bool> Button_AsyncSamplerRunning {false};
static std::atomic<bool> Button_StopSamplerRequested {false};
static bool Button_SamplerInitialStates[7] = {true, true, true, true, true, true, true};

hw_timer_t *Button_Timer = NULL;
static void IRAM_ATTR onTimer();
static bool Button_DoButtonActions(unsigned long currentTimestamp);

void Button_Init() {
	memset(gButtons, 0, sizeof(gButtons));
#if (WAKEUP_BUTTON >= 0 && WAKEUP_BUTTON <= MAX_GPIO)
	if (ESP_ERR_INVALID_ARG == esp_sleep_enable_ext0_wakeup((gpio_num_t) WAKEUP_BUTTON, 0)) {
		Log_Printf(LOGLEVEL_ERROR, wrongWakeUpGpio, WAKEUP_BUTTON);
	}
#endif

#ifdef NEOPIXEL_ENABLE // Try to find button that is used for shutdown via longpress-action (only necessary for Neopixel)
	#if (defined(BUTTON_0_ENABLE) || defined(EXPANDER_0_ENABLE)) && (BUTTON_0_LONG == CMD_SLEEPMODE)
	gShutdownButton = 0;
	#elif (defined(BUTTON_1_ENABLE) || defined(EXPANDER_1_ENABLE)) && (BUTTON_1_LONG == CMD_SLEEPMODE)
	gShutdownButton = 1;
	#elif (defined(BUTTON_2_ENABLE) || defined(EXPANDER_2_ENABLE)) && (BUTTON_2_LONG == CMD_SLEEPMODE)
	gShutdownButton = 2;
	#elif (defined(BUTTON_3_ENABLE) || defined(EXPANDER_3_ENABLE)) && (BUTTON_3_LONG == CMD_SLEEPMODE)
	gShutdownButton = 3;
	#elif (defined(BUTTON_4_ENABLE) || defined(EXPANDER_4_ENABLE)) && (BUTTON_4_LONG == CMD_SLEEPMODE)
	gShutdownButton = 4;
	#elif (defined(BUTTON_5_ENABLE) || defined(EXPANDER_5_ENABLE)) && (BUTTON_5_LONG == CMD_SLEEPMODE)
	gShutdownButton = 5;
	#elif (defined(BUTTON_6_ENABLE) || defined(EXPANDER_6_ENABLE)) && (BUTTON_6_LONG == CMD_SLEEPMODE)
	gShutdownButton = 6;
	#endif
#endif

// Activate internal pullups for all enabled buttons connected to GPIOs
#ifdef BUTTON_0_ENABLE
	if (BUTTON_0_ACTIVE_STATE) {
		pinMode(NEXT_BUTTON, INPUT);
	} else {
		pinMode(NEXT_BUTTON, INPUT_PULLUP);
	}
#endif
#ifdef BUTTON_1_ENABLE
	if (BUTTON_1_ACTIVE_STATE) {
		pinMode(PREVIOUS_BUTTON, INPUT);
	} else {
		pinMode(PREVIOUS_BUTTON, INPUT_PULLUP);
	}
#endif
#ifdef BUTTON_2_ENABLE
	if (BUTTON_2_ACTIVE_STATE) {
		pinMode(PAUSEPLAY_BUTTON, INPUT);
	} else {
		pinMode(PAUSEPLAY_BUTTON, INPUT_PULLUP);
	}
#endif
#ifdef BUTTON_3_ENABLE
	if (BUTTON_3_ACTIVE_STATE) {
		pinMode(ROTARYENCODER_BUTTON, INPUT);
	} else {
		pinMode(ROTARYENCODER_BUTTON, INPUT_PULLUP);
	}
#endif
#ifdef BUTTON_4_ENABLE
	if (BUTTON_4_ACTIVE_STATE) {
		pinMode(BUTTON_4, INPUT);
	} else {
		pinMode(BUTTON_4, INPUT_PULLUP);
	}
#endif
#ifdef BUTTON_5_ENABLE
	if (BUTTON_5_ACTIVE_STATE) {
		pinMode(BUTTON_5, INPUT);
	} else {
		pinMode(BUTTON_5, INPUT_PULLUP);
	}
#endif
#ifdef BUTTON_6_ENABLE
	if (BUTTON_6_ACTIVE_STATE) {
		pinMode(BUTTON_6, INPUT);
	} else {
		pinMode(BUTTON_6, INPUT_PULLUP);
	}
#endif

	// Create 1000Hz-HW-Timer (currently only used for buttons)
	Button_TimerSemaphore = xSemaphoreCreateBinary();
#if (defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3))
	Button_Timer = timerBegin(1000000); // Prescaler: CPU-clock in MHz
	timerAttachInterrupt(Button_Timer, &onTimer);
	timerAlarm(Button_Timer, 10000, true, 0); // 100 Hz
#else
	Button_Timer = timerBegin(0, 240, true); // Prescaler: CPU-clock in MHz
	timerAttachInterrupt(Button_Timer, &onTimer, true);
	timerAlarmWrite(Button_Timer, 10000, true); // 100 Hz
	timerAlarmEnable(Button_Timer);
#endif
}

// Read current state of all enabled buttons. true means released, false means physically down.
static void Button_ReadPhysicalStates(bool (&states)[7]) {
	for (bool &state : states) {
		state = true;
	}
#if defined(BUTTON_0_ENABLE) || defined(EXPANDER_0_ENABLE)
	states[0] = Port_Read(NEXT_BUTTON) ^ BUTTON_0_ACTIVE_STATE;
#endif
#if defined(BUTTON_1_ENABLE) || defined(EXPANDER_1_ENABLE)
	states[1] = Port_Read(PREVIOUS_BUTTON) ^ BUTTON_1_ACTIVE_STATE;
#endif
#if defined(BUTTON_2_ENABLE) || defined(EXPANDER_2_ENABLE)
	states[2] = Port_Read(PAUSEPLAY_BUTTON) ^ BUTTON_2_ACTIVE_STATE;
#endif
#if defined(BUTTON_3_ENABLE) || defined(EXPANDER_3_ENABLE)
	states[3] = Port_Read(ROTARYENCODER_BUTTON) ^ BUTTON_3_ACTIVE_STATE;
#endif
#if defined(BUTTON_4_ENABLE) || defined(EXPANDER_4_ENABLE)
	states[4] = Port_Read(BUTTON_4) ^ BUTTON_4_ACTIVE_STATE;
#endif
#if defined(BUTTON_5_ENABLE) || defined(EXPANDER_5_ENABLE)
	states[5] = Port_Read(BUTTON_5) ^ BUTTON_5_ACTIVE_STATE;
#endif
#if defined(BUTTON_6_ENABLE) || defined(EXPANDER_6_ENABLE)
	states[6] = Port_Read(BUTTON_6) ^ BUTTON_6_ACTIVE_STATE;
#endif
}

static void Button_ReadAllStates(void) {
	bool states[7];
	Button_ReadPhysicalStates(states);
	for (uint8_t i = 0; i < 7; i++) {
		gButtons[i].currentState = states[i];
	}
}

static const char *buttonNames[] = {
	"NEXT",
	"PREVIOUS",
	"PAUSEPLAY",
	"ROTARYENCODER",
	"BUTTON_4",
	"BUTTON_5",
	"BUTTON_6",
	"DUMMY"}; // index 7 matches the dummy slot in gButtons[8]

static bool Button_PushEdge(const ButtonEdge &edge) {
	const uint8_t writeIndex = Button_EdgeWriteIndex.load(std::memory_order_relaxed);
	const uint8_t nextWriteIndex = (writeIndex + 1) & buttonEdgeBufferMask;
	if (nextWriteIndex == Button_EdgeReadIndex.load(std::memory_order_acquire)) {
		Button_DroppedEdges.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	Button_EdgeBuffer[writeIndex] = edge;
	Button_EdgeWriteIndex.store(nextWriteIndex, std::memory_order_release);
	return true;
}

static bool Button_PopEdge(ButtonEdge &edge) {
	const uint8_t readIndex = Button_EdgeReadIndex.load(std::memory_order_relaxed);
	if (readIndex == Button_EdgeWriteIndex.load(std::memory_order_acquire)) {
		return false;
	}
	edge = Button_EdgeBuffer[readIndex];
	Button_EdgeReadIndex.store((readIndex + 1) & buttonEdgeBufferMask, std::memory_order_release);
	return true;
}

static void Button_SamplerTask(void *) {
	bool stableStates[7];
	bool candidateStates[7];
	uint32_t candidateSince[7];
	const uint32_t startedAt = millis();
	for (uint8_t i = 0; i < 7; i++) {
		stableStates[i] = Button_SamplerInitialStates[i];
		candidateStates[i] = Button_SamplerInitialStates[i];
		candidateSince[i] = startedAt;
	}

	while (!Button_StopSamplerRequested.load(std::memory_order_acquire)) {
		if (xSemaphoreTake(Button_TimerSemaphore.load(std::memory_order_acquire), portMAX_DELAY) != pdTRUE) {
			continue;
		}
		if (Button_StopSamplerRequested.load(std::memory_order_acquire)) {
			break;
		}

#ifdef PORT_EXPANDER_ENABLE
		Port_Cyclic();
#endif
		bool physicalStates[7];
		Button_ReadPhysicalStates(physicalStates);
		const uint32_t now = millis();

		for (uint8_t i = 0; i < 7; i++) {
			if (physicalStates[i] != candidateStates[i]) {
				candidateStates[i] = physicalStates[i];
				candidateSince[i] = now;
				continue;
			}
			if ((candidateStates[i] != stableStates[i]) && (now - candidateSince[i] >= buttonDebounceInterval)) {
				stableStates[i] = candidateStates[i];
				Button_PushEdge({now, i, !stableStates[i]});
			}
		}
	}

	Button_AsyncSamplerRunning.store(false, std::memory_order_release);
	Button_SamplerTaskHandle.store(nullptr, std::memory_order_release);
	vTaskDelete(nullptr);
}

void Button_StartSampler(void) {
	if (Button_AsyncSamplerRunning.load(std::memory_order_acquire)) {
		return;
	}

#ifdef PORT_EXPANDER_ENABLE
	Port_Cyclic();
#endif
	Button_ReadAllStates();
	for (uint8_t i = 0; i < 7; i++) {
		gButtons[i].lastState = gButtons[i].currentState;
		Button_SamplerInitialStates[i] = gButtons[i].currentState;
	}
	gButtonInitComplete = true;
	Button_EdgeReadIndex.store(0, std::memory_order_relaxed);
	Button_EdgeWriteIndex.store(0, std::memory_order_relaxed);
	Button_StopSamplerRequested.store(false, std::memory_order_release);

	TaskHandle_t taskHandle = nullptr;
	if (xTaskCreatePinnedToCore(Button_SamplerTask, "buttonSampler", 3072, nullptr, 2, &taskHandle, 0) == pdPASS) {
		Button_SamplerTaskHandle.store(taskHandle, std::memory_order_release);
		Button_AsyncSamplerRunning.store(true, std::memory_order_release);
	} else {
		Log_Println("Unable to start asynchronous button sampler; using main-loop fallback", LOGLEVEL_ERROR);
	}
}

void Button_StopSampler(void) {
	if (!Button_AsyncSamplerRunning.load(std::memory_order_acquire)) {
		return;
	}
	Button_StopSamplerRequested.store(true, std::memory_order_release);
	xSemaphoreGive(Button_TimerSemaphore.load(std::memory_order_acquire));
	const uint32_t waitStarted = millis();
	while (Button_SamplerTaskHandle.load(std::memory_order_acquire) != nullptr && millis() - waitStarted < 500u) {
		vTaskDelay(1);
	}
	if (Button_SamplerTaskHandle.load(std::memory_order_acquire) != nullptr) {
		Log_Println("Button sampler did not stop before I2C shutdown", LOGLEVEL_ERROR);
	}
}

// Update press/release state for a single button with debouncing
static void Button_UpdateState(uint8_t i, t_button &btn, unsigned long currentTimestamp) {
	bool const stateChanged = btn.currentState != btn.lastState;
	bool const debounceElapsed = currentTimestamp - btn.lastPressedTimestamp > buttonDebounceInterval;

	if (stateChanged && debounceElapsed) {
		bool const buttonPressed = !btn.currentState;
		if (buttonPressed) {
			Log_Printf(LOGLEVEL_INFO, "Button %d (%s) pressed", i, buttonNames[i]);
			// A press during the shutdown countdown is an emergency cancel, not a second command. Consume
			// the edge so NEXT/PREV/etc. does not also alter playback as the normal screen returns.
			btn.isPressed = !System_CancelSleep();
			btn.lastPressedTimestamp = currentTimestamp;
			btn.lastRepeatTimestamp = currentTimestamp;
			btn.holdRepeatFired = false;
			if (!btn.firstPressedTimestamp) {
				btn.firstPressedTimestamp = currentTimestamp;
			}
		} else {
			btn.isReleased = true;
			btn.lastReleasedTimestamp = currentTimestamp;
			btn.firstPressedTimestamp = 0;
		}
		btn.lastState = btn.currentState;
	}
}

static void Button_ApplyEdge(const ButtonEdge &edge) {
	t_button &btn = gButtons[edge.index];
	btn.currentState = !edge.pressed;
	btn.lastState = btn.currentState;
	if (edge.pressed) {
		Log_Printf(LOGLEVEL_INFO, "Button %d (%s) pressed", edge.index, buttonNames[edge.index]);
		// A press during the shutdown countdown is an emergency cancel, not a second command.
		btn.isPressed = !System_CancelSleep();
		btn.isReleased = false;
		btn.lastPressedTimestamp = edge.timestamp;
		btn.lastRepeatTimestamp = edge.timestamp;
		btn.holdRepeatFired = false;
		if (!btn.firstPressedTimestamp) {
			btn.firstPressedTimestamp = edge.timestamp;
		}
	} else {
		btn.isReleased = true;
		btn.lastReleasedTimestamp = edge.timestamp;
		btn.firstPressedTimestamp = 0;
	}
}

// If timer-semaphore is set, read buttons (unless controls are locked)
void Button_Cyclic() {
	if (Button_AsyncSamplerRunning.load(std::memory_order_acquire)) {
		const uint32_t dropped = Button_DroppedEdges.exchange(0, std::memory_order_acq_rel);
		if (dropped) {
			Log_Printf(LOGLEVEL_ERROR, "Button edge buffer overflow: %u event(s) lost", dropped);
		}

		bool actionDispatched = false;
		ButtonEdge edge;
		while (!actionDispatched && Button_PopEdge(edge)) {
			Button_ApplyEdge(edge);
			actionDispatched = Button_DoButtonActions(edge.timestamp);
		}
		if (!actionDispatched) {
			Button_DoButtonActions(millis()); // long-press/repeat while the button remains held
		}
		return;
	}

	if (xSemaphoreTake(Button_TimerSemaphore, 0) != pdTRUE) {
		return;
	}

	unsigned long currentTimestamp = millis();

#ifdef PORT_EXPANDER_ENABLE
	Port_Cyclic();
#endif

	Button_ReadAllStates();

	for (uint8_t i = 0; i < sizeof(gButtons) / sizeof(gButtons[0]); i++) {
		Button_UpdateState(i, gButtons[i], currentTimestamp);
	}

	gButtonInitComplete = true;
	Button_DoButtonActions(currentTimestamp);
}

// Multi-button combination configuration: {btn1, btn2, prefsKey, defaultCmd}
static const struct {
	uint8_t btn1;
	uint8_t btn2;
	const char *prefsKey;
	uint8_t defaultCmd;
} multiButtonCombos[] = {
	{0, 1, "btnMulti01", BUTTON_MULTI_01},
	{0, 2, "btnMulti02", BUTTON_MULTI_02},
	{0, 3, "btnMulti03", BUTTON_MULTI_03},
	{0, 4, "btnMulti04", BUTTON_MULTI_04},
	{0, 5, "btnMulti05", BUTTON_MULTI_05},
	{0, 6, "btnMulti06", BUTTON_MULTI_06},
	{1, 2, "btnMulti12", BUTTON_MULTI_12},
	{1, 3, "btnMulti13", BUTTON_MULTI_13},
	{1, 4, "btnMulti14", BUTTON_MULTI_14},
	{1, 5, "btnMulti15", BUTTON_MULTI_15},
	{1, 6, "btnMulti16", BUTTON_MULTI_16},
	{2, 3, "btnMulti23", BUTTON_MULTI_23},
	{2, 4, "btnMulti24", BUTTON_MULTI_24},
	{2, 5, "btnMulti25", BUTTON_MULTI_25},
	{2, 6, "btnMulti26", BUTTON_MULTI_26},
	{3, 4, "btnMulti34", BUTTON_MULTI_34},
	{3, 5, "btnMulti35", BUTTON_MULTI_35},
	{3, 6, "btnMulti36", BUTTON_MULTI_36},
	{4, 5, "btnMulti45", BUTTON_MULTI_45},
	{4, 6, "btnMulti46", BUTTON_MULTI_46},
	{5, 6, "btnMulti56", BUTTON_MULTI_56},
};

// Check for multi-button combinations and execute corresponding action
static bool Button_HandleMultiButtonPress(void) {
	for (const auto &combo : multiButtonCombos) {
		if (gButtons[combo.btn1].usedAsModifier || gButtons[combo.btn2].usedAsModifier) {
			continue; // held as a rotary modifier, not as half of a combo
		}
		if (gButtons[combo.btn1].isPressed && gButtons[combo.btn2].isPressed) {
			gButtons[combo.btn1].isPressed = false;
			gButtons[combo.btn2].isPressed = false;
			Cmd_Action(gPrefsSettings.getUChar(combo.prefsKey, combo.defaultCmd));
			return true;
		}
	}
	return false;
}

// Button command configuration: {prefsKeyShort, prefsKeyLong, defaultShort, defaultLong}
static const struct {
	const char *prefsKeyShort;
	const char *prefsKeyLong;
	uint8_t defaultShort;
	uint8_t defaultLong;
} buttonCmdConfig[] = {
	{"btnShort0", "btnLong0", BUTTON_0_SHORT, BUTTON_0_LONG},
	{"btnShort1", "btnLong1", BUTTON_1_SHORT, BUTTON_1_LONG},
	{"btnShort2", "btnLong2", BUTTON_2_SHORT, BUTTON_2_LONG},
	{"btnShort3", "btnLong3", BUTTON_3_SHORT, BUTTON_3_LONG},
	{"btnShort4", "btnLong4", BUTTON_4_SHORT, BUTTON_4_LONG},
	{"btnShort5", "btnLong5", BUTTON_5_SHORT, BUTTON_5_LONG},
	{"btnShort6", "btnLong6", BUTTON_6_SHORT, BUTTON_6_LONG},
};

// Handle a single button's short/long press action. true means a command was dispatched; the caller
// then leaves later buffered clicks for the next main-loop pass so trackCommand cannot be overwritten.
static bool Button_HandleSinglePress(uint8_t i, unsigned long currentTimestamp) {
	// The button was used to modify a rotary gesture, so it must not also fire its own action. Its short
	// press would otherwise fire on release, and its long press at intervalToLongPress while still held --
	// which for a button whose long action is CMD_SLEEPMODE means the box falls asleep mid-gesture.
	if (gButtons[i].usedAsModifier) {
		if (gButtons[i].lastReleasedTimestamp > gButtons[i].lastPressedTimestamp) {
			gButtons[i].isPressed = false;
			gButtons[i].usedAsModifier = false;
		}
		return false;
	}

	uint8_t Cmd_Short = gPrefsSettings.getUChar(buttonCmdConfig[i].prefsKeyShort, buttonCmdConfig[i].defaultShort);
	uint8_t Cmd_Long = gPrefsSettings.getUChar(buttonCmdConfig[i].prefsKeyLong, buttonCmdConfig[i].defaultLong);
	unsigned long const pressDuration = currentTimestamp - gButtons[i].lastPressedTimestamp;
	bool const wasReleased = gButtons[i].lastReleasedTimestamp > gButtons[i].lastPressedTimestamp;

	// A button that can act as a rotary modifier must not fire its long action at intervalToLongPress while
	// it is still held: holding it is also how you start a gesture, and the user has not necessarily begun
	// turning the encoder yet. (Holding NEXT for 700ms to seek would otherwise first fire BUTTON_0_LONG --
	// CMD_LASTTRACK by default -- and jump straight to the end of the book.) Defer it to release, exactly as
	// CMD_SLEEPMODE already is, so that a turn in the meantime can cancel it via usedAsModifier. A plain
	// long-press with no turn still works, it just resolves when the button comes back up.
	bool const isRotaryModifier = (Button_GetRotaryAction(i, true) != CMD_NOTHING) || (Button_GetRotaryAction(i, false) != CMD_NOTHING);

	// Handle button release (short or long press completed)
	if (wasReleased) {
		// A held Smart forward/backward has already emitted one or more commands. Do not add another
		// short/long action on release, otherwise releasing after scrolling would skip one extra track.
		if (gButtons[i].holdRepeatFired) {
			gButtons[i].isPressed = false;
			gButtons[i].holdRepeatFired = false;
			return false;
		}

		unsigned long const releaseDuration = gButtons[i].lastReleasedTimestamp - gButtons[i].lastPressedTimestamp;
		bool const wasShortPress = releaseDuration < intervalToLongPress;

		bool actionDispatched = false;
		if (wasShortPress) {
			Cmd_Action(Cmd_Short);
			actionDispatched = true;
		} else {
			// Sleep and rotary-modifier actions always resolve on release. Other long actions normally fire
			// while held and clear isPressed first; reaching this branch means the main loop was blocked for
			// the complete hold, so use the buffered duration and emit the long action now.
			Cmd_Action(Cmd_Long);
			actionDispatched = true;
		}

		gButtons[i].isPressed = false;
		return actionDispatched;
	}

	// Smart forward/backward may be assigned as the long action to make a physical NEXT/PREV button
	// scroll continuously while held, just like repeat frames from the IR remote. Rotary gestures still
	// win as soon as the encoder moves (usedAsModifier is handled at the top of this function).
	if ((Cmd_Long == CMD_SMART_FORWARDS || Cmd_Long == CMD_SMART_BACKWARDS) && pressDuration >= intervalToLongPress) {
		if (!gButtons[i].holdRepeatFired || (currentTimestamp - gButtons[i].lastRepeatTimestamp >= smartHoldRepeatIntervalMs)) {
			Cmd_Action(Cmd_Long);
			gButtons[i].holdRepeatFired = true;
			gButtons[i].lastRepeatTimestamp = currentTimestamp;
			return true;
		}
		return false;
	}

	if (isRotaryModifier) {
		return false; // Still held: wait for release before deciding whether this was a long press or a gesture
	}

	// Handle volume buttons with repeat functionality
	if (Cmd_Long == CMD_VOLUMEUP || Cmd_Long == CMD_VOLUMEDOWN) {
		if (pressDuration <= intervalToLongPress) {
			return false;
		}
		uint16_t remainder = pressDuration % intervalToLongPress;
		if (remainder < gLongPressTime) {
			Cmd_Action(Cmd_Long);
			gLongPressTime = remainder;
			return true;
		}
		gLongPressTime = remainder;
		return false;
	}

	// Handle other long-press actions (except sleep mode which triggers on release)
	if (Cmd_Long != CMD_SLEEPMODE && pressDuration > intervalToLongPress) {
		gButtons[i].isPressed = false;
		Cmd_Action(Cmd_Long);
		return true;
	}
	return false;
}

// settings-override.h, when present, replaces settings.h wholesale -- so an override written before this
// feature existed defines none of the BUTTON_n_ROTARY_* macros. Default them to CMD_NOTHING (= button is
// not a modifier) so those configs keep building and simply have no gestures until they opt in.
#ifndef BUTTON_0_ROTARY_CW
	#define BUTTON_0_ROTARY_CW CMD_NOTHING
#endif
#ifndef BUTTON_0_ROTARY_CCW
	#define BUTTON_0_ROTARY_CCW CMD_NOTHING
#endif
#ifndef BUTTON_1_ROTARY_CW
	#define BUTTON_1_ROTARY_CW CMD_NOTHING
#endif
#ifndef BUTTON_1_ROTARY_CCW
	#define BUTTON_1_ROTARY_CCW CMD_NOTHING
#endif
#ifndef BUTTON_2_ROTARY_CW
	#define BUTTON_2_ROTARY_CW CMD_NOTHING
#endif
#ifndef BUTTON_2_ROTARY_CCW
	#define BUTTON_2_ROTARY_CCW CMD_NOTHING
#endif
#ifndef BUTTON_3_ROTARY_CW
	#define BUTTON_3_ROTARY_CW CMD_NOTHING
#endif
#ifndef BUTTON_3_ROTARY_CCW
	#define BUTTON_3_ROTARY_CCW CMD_NOTHING
#endif
#ifndef BUTTON_4_ROTARY_CW
	#define BUTTON_4_ROTARY_CW CMD_NOTHING
#endif
#ifndef BUTTON_4_ROTARY_CCW
	#define BUTTON_4_ROTARY_CCW CMD_NOTHING
#endif
#ifndef BUTTON_5_ROTARY_CW
	#define BUTTON_5_ROTARY_CW CMD_NOTHING
#endif
#ifndef BUTTON_5_ROTARY_CCW
	#define BUTTON_5_ROTARY_CCW CMD_NOTHING
#endif

// "Hold this button, turn the encoder" -- one action per rotation direction, per button.
// Mirrors buttonCmdConfig: compile-time default in settings.h, overridable at runtime via NVS.
static const struct {
	const char *prefsKeyCw;
	const char *prefsKeyCcw;
	uint8_t defaultCw;
	uint8_t defaultCcw;
} rotaryCmdConfig[] = {
	{"btnRotCw0", "btnRotCcw0", BUTTON_0_ROTARY_CW, BUTTON_0_ROTARY_CCW},
	{"btnRotCw1", "btnRotCcw1", BUTTON_1_ROTARY_CW, BUTTON_1_ROTARY_CCW},
	{"btnRotCw2", "btnRotCcw2", BUTTON_2_ROTARY_CW, BUTTON_2_ROTARY_CCW},
	{"btnRotCw3", "btnRotCcw3", BUTTON_3_ROTARY_CW, BUTTON_3_ROTARY_CCW},
	{"btnRotCw4", "btnRotCcw4", BUTTON_4_ROTARY_CW, BUTTON_4_ROTARY_CCW},
	{"btnRotCw5", "btnRotCcw5", BUTTON_5_ROTARY_CW, BUTTON_5_ROTARY_CCW},
};

uint8_t Button_GetRotaryAction(uint8_t buttonIndex, bool clockwise) {
	if (buttonIndex >= (sizeof(rotaryCmdConfig) / sizeof(rotaryCmdConfig[0]))) {
		return CMD_NOTHING;
	}
	const auto &cfg = rotaryCmdConfig[buttonIndex];
	return clockwise
		? gPrefsSettings.getUChar(cfg.prefsKeyCw, cfg.defaultCw)
		: gPrefsSettings.getUChar(cfg.prefsKeyCcw, cfg.defaultCcw);
}

// Compile-time default (settings.h / the override), ignoring any NVS override. Used by the
// web-UI's "reset to factory settings".
uint8_t Button_GetRotaryActionDefault(uint8_t buttonIndex, bool clockwise) {
	if (buttonIndex >= (sizeof(rotaryCmdConfig) / sizeof(rotaryCmdConfig[0]))) {
		return CMD_NOTHING;
	}
	return clockwise ? rotaryCmdConfig[buttonIndex].defaultCw : rotaryCmdConfig[buttonIndex].defaultCcw;
}

// currentState (not isPressed) is the live level: false means physically down right now. isPressed is a
// consumable latch that the long-press handler clears while the finger is still on the button, so it is
// useless as a "still held" signal.
uint8_t Button_GetHeldModifier(void) {
	if (!gButtonInitComplete) {
		return BUTTON_NONE; // gButtons[] is still garbage before the first scan
	}
	for (uint8_t i = 0; i < (sizeof(rotaryCmdConfig) / sizeof(rotaryCmdConfig[0])); i++) {
		if (gButtons[i].currentState) {
			continue; // not pressed
		}
		if (Button_GetRotaryAction(i, true) != CMD_NOTHING || Button_GetRotaryAction(i, false) != CMD_NOTHING) {
			return i;
		}
	}
	return BUTTON_NONE;
}

void Button_MarkModifierUsed(uint8_t buttonIndex) {
	if (buttonIndex < (sizeof(gButtons) / sizeof(gButtons[0]))) {
		gButtons[buttonIndex].usedAsModifier = true;
	}
}

// Do corresponding actions for all buttons. Dispatch no more than one command per call: the audio
// control path intentionally has one command slot, so a later buffered click must wait for the next
// loop pass rather than overwrite the click that is about to be consumed.
static bool Button_DoButtonActions(unsigned long currentTimestamp) {
	if (Button_HandleMultiButtonPress()) {
		return true;
	}

	for (uint8_t i = 0; i < sizeof(buttonCmdConfig) / sizeof(buttonCmdConfig[0]); i++) {
		if (gButtons[i].isPressed && Button_HandleSinglePress(i, currentTimestamp)) {
			return true;
		}
	}
	return false;
}

void IRAM_ATTR onTimer() {
	BaseType_t higherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(Button_TimerSemaphore.load(std::memory_order_relaxed), &higherPriorityTaskWoken);
	if (higherPriorityTaskWoken == pdTRUE) {
		portYIELD_FROM_ISR();
	}
}
