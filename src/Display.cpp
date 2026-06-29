// clang-format off
#include "settings.h"

#ifdef OLED_ENABLE

#include "AudioPlayer.h"
#include "Battery.h"
#include "Display.h"
#include "Log.h"
#include "System.h"
#include "Wlan.h"
#include "values.h"

#include <U8g2lib.h>
#include <Wire.h>

extern TwoWire i2cBusTwo;

// -------- runtime configuration (web-settings, persisted in NVS) --------
// Startup-animation selector for the idle/attract screen.
enum class StartupAnim : uint8_t { None = 0, Boot = 1, Login = 2, Full = 3 };

static constexpr char kDefaultIdleLine1[] = "LEO INDUSTRIES";
static constexpr char kDefaultIdleLine2[] = "AUDIO TERMINAL AT-1";

static bool        s_cfgEnabled    = true;                       // master on/off (oledEnable)
static StartupAnim s_cfgStartAnim  = StartupAnim::Full;          // oledStartAnim
static bool        s_cfgAnimColdOnly = false;                    // oledAnimCold – only run the startup anim on a real power-on
static bool        s_cfgShowBattery = true;                      // oledShowBatt – battery % on playing screen
static bool        s_cfgShowTime   = true;                       // oledShowTime – elapsed/total on playing screen
static bool        s_cfgShowWifi   = true;                       // oledShowWifi – WIFI marker on playing screen
static bool        s_cfgShowVolume = true;                       // oledShowVol – full-screen volume overlay
static bool        s_cfgFlip       = false;                      // oledFlip – rotate the panel by 180°
static char        s_cfgIdleLine1[32] = "";                      // oledIdleL1 – idle header line 1
static char        s_cfgIdleLine2[32] = "";                      // oledIdleL2 – idle header line 2

// Pull the OLED settings out of NVS into the cached statics above.
static void Display_LoadConfig(void) {
    s_cfgEnabled     = gPrefsSettings.getBool("oledEnable", true);
    uint8_t anim     = gPrefsSettings.getUChar("oledStartAnim", static_cast<uint8_t>(StartupAnim::Full));
    if (anim > static_cast<uint8_t>(StartupAnim::Full)) anim = static_cast<uint8_t>(StartupAnim::Full);
    s_cfgStartAnim   = static_cast<StartupAnim>(anim);
    s_cfgAnimColdOnly = gPrefsSettings.getBool("oledAnimCold", false);
    s_cfgShowBattery = gPrefsSettings.getBool("oledShowBatt", true);
    s_cfgShowTime    = gPrefsSettings.getBool("oledShowTime", true);
    s_cfgShowWifi    = gPrefsSettings.getBool("oledShowWifi", true);
    s_cfgShowVolume  = gPrefsSettings.getBool("oledShowVol", true);
    s_cfgFlip        = gPrefsSettings.getBool("oledFlip", false);
    String l1        = gPrefsSettings.getString("oledIdleL1", kDefaultIdleLine1);
    String l2        = gPrefsSettings.getString("oledIdleL2", kDefaultIdleLine2);
    strncpy(s_cfgIdleLine1, l1.c_str(), sizeof(s_cfgIdleLine1) - 1);
    s_cfgIdleLine1[sizeof(s_cfgIdleLine1) - 1] = '\0';
    strncpy(s_cfgIdleLine2, l2.c_str(), sizeof(s_cfgIdleLine2) - 1);
    s_cfgIdleLine2[sizeof(s_cfgIdleLine2) - 1] = '\0';
}

// Set by the byte callback when an I2C transfer NACKs/fails (e.g. a bus glitch or a transfer that
// got interrupted). Display_Cyclic re-initialises the panel when this trips, so a one-off desync
// self-heals instead of leaving the OLED black until the next reboot.
static bool s_i2cSendError = false;

static uint8_t Display_I2cByteCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
        case U8X8_MSG_BYTE_INIT:
            i2cBusTwo.setClock(400000UL);
            break;
        case U8X8_MSG_BYTE_SEND:
            i2cBusTwo.write(static_cast<const uint8_t *>(arg_ptr), arg_int);
            break;
        case U8X8_MSG_BYTE_START_TRANSFER:
            i2cBusTwo.beginTransmission(u8x8_GetI2CAddress(u8x8) >> 1);
            break;
        case U8X8_MSG_BYTE_END_TRANSFER:
            if (i2cBusTwo.endTransmission() != 0) {
                s_i2cSendError = true;
            }
            break;
        default:
            return 0;
    }
    return 1;
}

static U8G2_SH1106_128X64_NONAME_F_HW_I2C s_u8g2(U8G2_R0, U8X8_PIN_NONE);
static bool s_displayOk = false;

// Re-init recovery. The OLED has no reset line (U8X8_PIN_NONE), so the only way to bring a panel that
// lost power (hot-unplug/replug), boot-probed too early, or got confused back to life is to re-run
// initDisplay() over I2C. We do that, but SAFELY so we never repeat the wedge:
//   - only after SUSTAINED frame failures (a one-off NACK on a working panel must never trigger it),
//   - rate-limited (never a per-frame storm),
//   - initDisplay() only runs once the address probe ACKs (a missing panel is just a cheap probe).
static uint16_t s_consecSendErrors = 0; // consecutive frame NACKs; reset by any good frame
static constexpr uint16_t kSendErrorsBeforeLost = 20; // ~2 s of solid NACKs => treat the panel as lost
static constexpr uint32_t kReinitIntervalMs = 3000; // at most one (re)init attempt per 3 s

// -------- title scroll state --------
static char s_lastRawTitle[256] = "";

enum class ScrollPhase : uint8_t { PAUSE_START, SCROLLING, PAUSE_END };
static ScrollPhase s_scrollPhase      = ScrollPhase::PAUSE_START;
static uint32_t    s_scrollPhaseStart = 0;
static int16_t     s_scrollPos        = 0;
static uint32_t    s_lastScrollStep   = 0;

// font_6x13: 6 px wide → 21 chars fit in 126 px ≤ 128 px
static constexpr uint8_t  kLineChars     = 21;
static constexpr uint16_t kScrollPauseMs = 2000;
static constexpr uint16_t kScrollStepMs  = 280;

// -------- volume-bar state --------
static uint8_t  s_lastVol      = 0xFF;  // 0xFF = not yet sampled
static uint32_t s_volChangedAt = 0;
static constexpr uint32_t kVolBarDurationMs = 2500;

// -------- login splash state --------
static uint32_t s_idleSince    = 0;        // millis() when we first entered idle
static uint8_t  s_lastPlayMode = 0xFF;     // detect idle transition
static bool     s_wokeFromSleep    = false; // this boot is a wake from an intentional deep-sleep (NVS intent-flag)
static bool     s_coldStartLatched = false; // read+consume the wake-from-sleep flag exactly once per boot
static bool     s_startupAnimShown = false; // the startup animation has already run to completion once
static constexpr uint32_t kBootDurationMs  = 3000;  // boot screen
static constexpr uint32_t kLoginDurationMs = 3500;  // login animation
// Boot screen: first two lines appear, then "Booting." / ".." / "..."
static constexpr uint32_t kBootLine1Ms  = 0;
static constexpr uint32_t kBootLine2Ms  = 750;
static constexpr uint32_t kBootDotsMs   = 1000;  // dots start cycling from here
static constexpr uint32_t kBootDotCycle = 600;   // ms per dot step
// Login animation offsets are relative to kBootDurationMs
static constexpr uint32_t kUserStart  = 500;
static constexpr uint32_t kUserStep   = 220;
static constexpr uint32_t kPassStart  = 1400;
static constexpr uint32_t kPassStep   = 220;


// Convert a UTF-8 string to ISO-8859-1 (Latin-1) in-place equivalent.
// Characters outside Latin-1 (U+0100 and above) are replaced with '?'.
// All German umlauts (ä ö ü Ä Ö Ü ß) are within Latin-1 and round-trip losslessly.
static void utf8ToLatin1(const char *src, char *dst, size_t dstSize) {
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di < dstSize - 1; ) {
        uint8_t c = static_cast<uint8_t>(src[si]);
        if (c < 0x80) {                         // ASCII – copy directly
            dst[di++] = static_cast<char>(c);
            si++;
        } else if (c == 0xC2 &&
                   static_cast<uint8_t>(src[si+1]) >= 0x80 &&
                   static_cast<uint8_t>(src[si+1]) <= 0xBF) {
            // U+0080..U+00BF: second byte is the Latin-1 code point
            dst[di++] = src[si + 1];
            si += 2;
        } else if (c == 0xC3 &&
                   static_cast<uint8_t>(src[si+1]) >= 0x80 &&
                   static_cast<uint8_t>(src[si+1]) <= 0xBF) {
            // U+00C0..U+00FF: second byte + 0x40 is the Latin-1 code point
            dst[di++] = static_cast<char>(static_cast<uint8_t>(src[si + 1]) + 0x40u);
            si += 2;
        } else {
            // Beyond Latin-1 or malformed – skip the whole sequence
            dst[di++] = '?';
            si++;
            while (static_cast<uint8_t>(src[si]) >= 0x80 &&
                   static_cast<uint8_t>(src[si]) <  0xC0) {
                si++;
            }
        }
    }
    dst[di] = '\0';
}

// Strip directory prefix and file extension from a path-style title.
static void stripPathAndExt(const char *src, char *dst, size_t dstSize) {
    const char *base = strrchr(src, '/');
    base = (base != nullptr) ? base + 1 : src;
    strncpy(dst, base, dstSize - 1);
    dst[dstSize - 1] = '\0';
    char *dot = strrchr(dst, '.');
    if (dot != nullptr && strlen(dot) >= 2 && strlen(dot) <= 5) {
        *dot = '\0';
    }
}

// Find the first " - " in src. Returns its index or SIZE_MAX if not found.
static size_t findDash(const char *src, size_t len) {
    for (size_t i = 0; i + 2 < len; i++) {
        if (src[i] == ' ' && src[i+1] == '-' && src[i+2] == ' ') {
            return i;
        }
    }
    return SIZE_MAX;
}

// Copy up to n chars from src into dst (always null-terminates).
static void copyLine(char *dst, const char *src, size_t n) {
    strncpy(dst, src, n);
    dst[n] = '\0';
}

// Try to split title on " - " boundaries across up to 3 lines.
// Searches the entire string — the dash can be anywhere.
// Returns number of lines filled (0 = no dash found, caller should hard-wrap).
static uint8_t splitOnDashes(const char *title, size_t len,
                              char *line1, char *line2, char *line3) {
    line1[0] = line2[0] = line3[0] = '\0';

    size_t d1 = findDash(title, len);
    if (d1 == SIZE_MAX) return 0; // no " - " at all

    size_t p1Len    = d1;
    const char *p2  = title + d1 + 3;
    size_t      p2Len = len - d1 - 3;

    if (p1Len <= kLineChars && p2Len <= kLineChars) {
        // Both parts fit cleanly: 2 lines
        copyLine(line1, title, p1Len);
        copyLine(line2, p2, p2Len);
        return 2;
    }

    if (p1Len <= kLineChars && p2Len <= kLineChars * 2u) {
        // Part1 fits on line1, part2 needs lines 2+3
        copyLine(line1, title, p1Len);
        // Try a second dash within part2
        size_t d2 = findDash(p2, p2Len);
        if (d2 != SIZE_MAX && d2 <= kLineChars && (p2Len - d2 - 3) <= kLineChars) {
            copyLine(line2, p2, d2);
            copyLine(line3, p2 + d2 + 3, p2Len - d2 - 3);
        } else {
            copyLine(line2, p2, kLineChars);
            copyLine(line3, p2 + kLineChars, kLineChars);
        }
        return 3;
    }

    if (p1Len <= kLineChars * 2u && p2Len <= kLineChars) {
        // Part1 needs lines 1+2, part2 goes on line3
        copyLine(line1, title, kLineChars);
        copyLine(line2, title + kLineChars, p1Len - kLineChars);
        copyLine(line3, p2, p2Len);
        return 3;
    }

    // Parts don't fit cleanly — give up, caller will hard-wrap
    return 0;
}

// Three display lines × 21 chars = 63-char static window.
static constexpr uint8_t kWindowChars3 = kLineChars * 3u;

// Draw the track title across three lines with scrolling for very long titles.
static void drawCentred(const char *s, uint8_t y) {
    int x = static_cast<int>((128 - s_u8g2.getStrWidth(s)) / 2);
    if (x < 0) x = 0;
    s_u8g2.drawStr(x, y, s);
}

static void Display_DrawTitle(const char *rawTitle, uint8_t y1, uint8_t y2, uint8_t y3) {
    char stripped[256];
    stripPathAndExt(rawTitle, stripped, sizeof(stripped));
    char title[256];
    utf8ToLatin1(stripped, title, sizeof(title));
    size_t len = strlen(title);

    if (strncmp(rawTitle, s_lastRawTitle, sizeof(s_lastRawTitle)) != 0) {
        strncpy(s_lastRawTitle, rawTitle, sizeof(s_lastRawTitle) - 1);
        s_lastRawTitle[sizeof(s_lastRawTitle) - 1] = '\0';
        s_scrollPos        = 0;
        s_scrollPhase      = ScrollPhase::PAUSE_START;
        s_scrollPhaseStart = millis();
    }

    if (len == 0) return;

    if (len <= kWindowChars3) {
        char line1[kLineChars + 1];
        char line2[kLineChars + 1];
        char line3[kLineChars + 1];

        uint8_t n = splitOnDashes(title, len, line1, line2, line3);
        if (n >= 1) {
            drawCentred(line1, y1);
            if (n >= 2 && line2[0]) drawCentred(line2, y2);
            if (n >= 3 && line3[0]) drawCentred(line3, y3);
            return;
        }

        // No dash split possible — hard-wrap across up to 3 lines
        strncpy(line1, title, kLineChars); line1[kLineChars] = '\0';
        drawCentred(line1, y1);
        if (len > kLineChars) {
            strncpy(line2, title + kLineChars, kLineChars); line2[kLineChars] = '\0';
            drawCentred(line2, y2);
        }
        if (len > kLineChars * 2u) {
            strncpy(line3, title + kLineChars * 2u, kLineChars); line3[kLineChars] = '\0';
            drawCentred(line3, y3);
        }
        return;
    }

    // Title too long even for 3 lines — scroll a 3-line window
    uint32_t now    = millis();
    int16_t  maxPos = static_cast<int16_t>(len - kWindowChars3);

    switch (s_scrollPhase) {
        case ScrollPhase::PAUSE_START:
            s_scrollPos = 0;
            if (now - s_scrollPhaseStart >= kScrollPauseMs) {
                s_scrollPhase    = ScrollPhase::SCROLLING;
                s_lastScrollStep = now;
            }
            break;
        case ScrollPhase::SCROLLING:
            if (now - s_lastScrollStep >= kScrollStepMs) {
                s_lastScrollStep = now;
                s_scrollPos++;
                if (s_scrollPos >= maxPos) {
                    s_scrollPos        = maxPos;
                    s_scrollPhase      = ScrollPhase::PAUSE_END;
                    s_scrollPhaseStart = now;
                }
            }
            break;
        case ScrollPhase::PAUSE_END:
            s_scrollPos = maxPos;
            if (now - s_scrollPhaseStart >= kScrollPauseMs) {
                s_scrollPos        = 0;
                s_scrollPhase      = ScrollPhase::PAUSE_START;
                s_scrollPhaseStart = now;
            }
            break;
    }

    char line1[kLineChars + 1];
    char line2[kLineChars + 1];
    char line3[kLineChars + 1];
    strncpy(line1, title + s_scrollPos,                  kLineChars); line1[kLineChars] = '\0';
    strncpy(line2, title + s_scrollPos + kLineChars,     kLineChars); line2[kLineChars] = '\0';
    strncpy(line3, title + s_scrollPos + kLineChars * 2, kLineChars); line3[kLineChars] = '\0';
    s_u8g2.drawStr(0, y1, line1);
    s_u8g2.drawStr(0, y2, line2);
    s_u8g2.drawStr(0, y3, line3);
}

// -----------------------------------------------------------------------

void Display_Exit(void) {
    if (!s_displayOk) return;
    I2cBusTwo_Lock();
    s_u8g2.clearBuffer();
    s_u8g2.sendBuffer();
    s_u8g2.setPowerSave(1); // display off, low power — avoids SDA glitch as power rail drops
    I2cBusTwo_Unlock();
    s_displayOk = false;
}

// Probe the panel and bring it up. All I2C is done under the bus lock so it can't interleave with
// the RC522-I2C reader task / port-expander. settleDelay adds the cold-boot power-rail settle wait;
// the cyclic retry path passes false (the rail is long stable and a NACK probe is cheap).
static bool Display_HwInit(bool settleDelay) {
    if (settleDelay) {
        // Allow the peripheral power rail time to stabilise after a power cycle.
        // 20 ms is too short when the rail starts from zero (cold boot / deepsleep).
        delay(200);
    }
    I2cBusTwo_Lock();
    i2cBusTwo.beginTransmission(oledI2cAddress);
    const bool present = (i2cBusTwo.endTransmission() == 0);
    if (!present) {
        I2cBusTwo_Unlock();
        s_displayOk = false;
        return false;
    }
    s_u8g2.getU8x8()->byte_cb = Display_I2cByteCb;
    s_u8g2.setI2CAddress(oledI2cAddress << 1);
    s_u8g2.initDisplay();
    s_u8g2.setFlipMode(s_cfgFlip ? 1 : 0); // 180° rotation when the panel is mounted upside-down
    s_u8g2.setPowerSave(0);
    s_u8g2.clearBuffer();
    s_i2cSendError = false;
    s_u8g2.sendBuffer();
    I2cBusTwo_Unlock();
    s_displayOk = !s_i2cSendError;
    if (s_displayOk) {
        s_consecSendErrors = 0; // clean slate once the panel is up
    }
    return s_displayOk;
}

// Push the current framebuffer to the panel under the bus lock. A transient NACK on a single frame
// is harmless: the full framebuffer is resent on the very next cycle (~100 ms), so the pixels
// self-correct. We deliberately do NOT tear down / re-init on a send error here. Doing that turned
// an occasional glitch into an unbounded initDisplay() storm that wedged the SH1106 command
// interpreter until it was physically power-cycled (it has no reset line, so an ESP reboot can't
// clear it). Re-init only ever happens for the boot-probe case, bounded, in Display_Cyclic.
static void Display_Send(void) {
    I2cBusTwo_Lock();
    s_i2cSendError = false;
    s_u8g2.sendBuffer();
    I2cBusTwo_Unlock();
    // A one-off NACK is harmless — the full framebuffer is resent next cycle, so the pixels
    // self-correct; do NOT re-init here. Only count SUSTAINED failures (panel unplugged / lost power)
    // so Display_Cyclic can re-init once it comes back. A good frame clears the counter, so a working
    // (even slightly flaky) panel is never re-initialised — that per-NACK re-init was the wedge storm.
    if (s_i2cSendError) {
        if (s_consecSendErrors < 0xFFFF) {
            s_consecSendErrors++;
        }
    } else {
        s_consecSendErrors = 0;
    }
}

void Display_Init(void) {
    Display_LoadConfig();
    if (!s_coldStartLatched) {
        // The complete board cuts ESP32 power on deep-sleep, so esp_sleep_get_wakeup_cause() can't
        // tell a real power-on apart from a sleep-wake (both look like a cold boot). Instead,
        // System_DeepSleepManager() sets an NVS flag right before sleeping; we read and clear it
        // here. Flag present => this boot is a wake from an intentional deep-sleep; absent => a
        // genuine power-on via the physical switch.
        s_wokeFromSleep = gPrefsSettings.getBool("wokeFromSleep", false);
        if (s_wokeFromSleep) {
            gPrefsSettings.putBool("wokeFromSleep", false); // consume it so the next real power-on animates
        }
        s_coldStartLatched = true;
    }
    if (!s_cfgEnabled) {
        Log_Println("OLED: disabled via web settings", LOGLEVEL_NOTICE);
        return;
    }
    s_consecSendErrors = 0;
    if (Display_HwInit(true)) {
        Log_Println("OLED: display initialised", LOGLEVEL_INFO);
    } else {
        Log_Println("OLED: display not found on I2C bus (will retry)", LOGLEVEL_ERROR);
    }
}

// Re-read the OLED settings from NVS and apply them without a reboot. Handles the
// enable→disable (power the panel off) and disable→enable (bring it back up) transitions,
// and applies a changed flip-mode live.
void Display_ReloadConfig(void) {
    Display_LoadConfig();
    if (!s_cfgEnabled) {
        if (s_displayOk) Display_Exit();
        return;
    }
    if (!s_displayOk) {
        Display_Init(); // re-reads the config, but that is harmless
        return;
    }
    s_u8g2.setFlipMode(s_cfgFlip ? 1 : 0);
}

// Toggle the master enable flag, persist it and apply immediately (CMD_TOGGLE_OLED).
void Display_Toggle(void) {
    bool nowEnabled = !gPrefsSettings.getBool("oledEnable", true);
    gPrefsSettings.putBool("oledEnable", nowEnabled);
    Display_ReloadConfig();
    Log_Printf(LOGLEVEL_NOTICE, "OLED: %s via command", nowEnabled ? "enabled" : "disabled");
}

bool Display_IsEnabled(void) {
    return s_cfgEnabled;
}

// -----------------------------------------------------------------------
//  IDLE SCREEN (128 × 64)
//    y=13   "LEO INDUSTRIES"            font_6x13
//    y=26   "AUDIO TERMINAL AT-1"       font_6x13
//    y=39   IP address (or "NO WIFI")   font_6x13
//    y=56   "READY_"  (_ blinks 500 ms) font_6x13
//
//  PLAYING SCREEN
//    y=10   Title line 1  (font_6x10, 21 chars)
//    y=20   Title line 2  (font_6x10, 21 chars or blank)
//    y=30   Title line 3  (font_6x10, 21 chars or blank)
//    y=57   B:XX% [left]  0:00/3:45 [centre]  W:ok [right]  (font_5x7)
//
//  VOLUME SCREEN (shown for 2.5 s after any volume change)
//    y=13   "VOLUME" centred            font_6x13
//    y=15   separator
//    y=18–38 bar (21 px tall, x=4..123)
//    y=40   separator
//    y=57   volume number centred       font_6x13
// -----------------------------------------------------------------------


void Display_Cyclic(void) {
    uint32_t now = millis();

    // Recover from a failed init (a transient I2C NACK at boot used to leave the panel black until
    // the next reboot) or from a runtime desync flagged by Display_Send: re-probe + re-init
    // periodically (without the cold-boot settle delay — a NACK probe is cheap).
    // (Re)bring-up path: the panel isn't up — boot probe missed, or it was lost/replugged (see the
    // sustained-failure check below). Retry initDisplay, RATE-LIMITED so it can never become a storm.
    // HwInit only sends init commands once the address probe ACKs, so on a missing panel this is just
    // a cheap probe every few seconds; on a freshly replugged (power-cycled, clean) panel it comes
    // straight back. It never runs against a working panel, so it can't re-wedge the SH1106.
    if (!s_displayOk) {
        static uint32_t s_lastInitTry = 0;
        if (s_cfgEnabled && (now - s_lastInitTry >= kReinitIntervalMs)) {
            s_lastInitTry = now;
            if (Display_HwInit(false)) {
                Log_Println("OLED: (re)initialised (boot miss or panel reconnected)", LOGLEVEL_NOTICE);
            }
        }
        return;
    }

    // Panel was up but has been solidly NACKing for ~2 s (unplugged / lost power): drop it to the
    // bring-up path above so it re-inits once it responds again. The threshold means a one-off frame
    // glitch on a working panel never trips this (that distinction is what avoids the re-init storm).
    if (s_consecSendErrors >= kSendErrorsBeforeLost) {
        Log_Println("OLED: panel stopped responding — will re-init when it returns", LOGLEVEL_NOTICE);
        s_displayOk = false;
        s_consecSendErrors = 0;
        return;
    }

    static uint32_t s_lastUpdate = 0;
    if (now - s_lastUpdate < 100u) return;
    s_lastUpdate = now;

    uint8_t vol = AudioPlayer_GetCurrentVolume();
    if (s_lastVol == 0xFF) {
        s_lastVol = vol;                    // first sample – no change event
    } else if (vol != s_lastVol) {
        s_lastVol      = vol;
        s_volChangedAt = now;
    }
    bool volScreen = s_cfgShowVolume && (s_volChangedAt > 0) && (now - s_volChangedAt < kVolBarDurationMs);
    bool idle      = (gPlayProperties.playMode == NO_PLAYLIST);

    s_u8g2.clearBuffer();

    // ---- VOLUME SCREEN (takes over entire display) ----
    if (volScreen) {
        s_u8g2.setFont(u8g2_font_6x13_tf);
        const char *lbl = "VOLUME";
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(lbl)) / 2), 13, lbl);

        // Tall progress bar
        constexpr uint8_t barX = 4;
        constexpr uint8_t barY = 18;
        constexpr uint8_t barW = 120;
        constexpr uint8_t barH = 21;
        s_u8g2.drawFrame(barX, barY, barW, barH);
        uint8_t maxVol = AudioPlayer_GetMaxVolume();
        if (maxVol > 0 && vol > 0) {
            uint8_t filled = static_cast<uint8_t>(
                static_cast<uint16_t>(vol) * (barW - 2) / maxVol);
            s_u8g2.drawBox(barX + 1, barY + 1, filled, barH - 2);
        }

        // Volume number centred below
        char numBuf[6];
        snprintf(numBuf, sizeof(numBuf), "%d", vol);
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(numBuf)) / 2), 57, numBuf);

        Display_Send();
        return;
    }

    // ---- IDLE SCREEN ----
    if (idle) {
        // Track when idle started
        if (s_lastPlayMode != NO_PLAYLIST) {
            s_lastPlayMode = NO_PLAYLIST;
            s_idleSince    = now;
        }
        uint32_t idleMs = now - s_idleSince;

        // The startup/attract animation is selectable: it can show the boot screen, the
        // login splash, both (default) or nothing. Compute each phase's duration so the
        // disabled phases collapse to zero and we fall straight through to the idle screen.
        //
        // "Cold-start only" (oledAnimCold) restricts the animation to a genuine power-on via the
        // physical switch: it then plays exactly once and is skipped on every wake-from-deep-sleep
        // and on the attract re-runs that follow each playback. Off (default) keeps the original
        // behaviour where the animation plays on every idle entry.
        const bool animAllowed = !s_cfgAnimColdOnly || (!s_wokeFromSleep && !s_startupAnimShown);
        const uint32_t bootDur  = (animAllowed && (s_cfgStartAnim == StartupAnim::Boot || s_cfgStartAnim == StartupAnim::Full)) ? kBootDurationMs : 0u;
        const uint32_t loginDur = (animAllowed && (s_cfgStartAnim == StartupAnim::Login || s_cfgStartAnim == StartupAnim::Full)) ? kLoginDurationMs : 0u;

        s_u8g2.setFont(u8g2_font_6x13_tf);

        if (idleMs < bootDur) {
            // ---- BOOT SCREEN ----
            if (idleMs >= kBootLine1Ms) s_u8g2.drawStr(0, 13, s_cfgIdleLine1);
            if (idleMs >= kBootLine2Ms) s_u8g2.drawStr(0, 26, s_cfgIdleLine2);
            if (idleMs >= kBootDotsMs) {
                uint32_t step = ((idleMs - kBootDotsMs) / kBootDotCycle) % 3u;
                const char *dotStrs[] = {"Booting.", "Booting..", "Booting..."};
                s_u8g2.drawStr(0, 45, dotStrs[step]);
            }
        } else if (idleMs < bootDur + loginDur) {
            // ---- LOGIN SPLASH ----
            uint32_t loginMs = idleMs - bootDur;
            bool cursorOn = (now / 400u) % 2u == 0u;

            // Header
            s_u8g2.drawStr(0, 13, "AT-1 LOGIN");

            // Username line (row 3)
            s_u8g2.drawStr(0, 35, "Username: ");
            uint8_t uChars = 0;
            if (loginMs >= kUserStart) {
                uChars = static_cast<uint8_t>(
                    min((loginMs - kUserStart) / kUserStep + 1u, static_cast<uint32_t>(3)));
            }
            const char *uFull = "leo";
            char uBuf[5];
            memcpy(uBuf, uFull, uChars);
            bool uDone = (uChars >= 3);
            uBuf[uChars] = (uDone ? '\0' : (cursorOn ? '_' : ' '));
            uBuf[uChars + (uDone ? 0 : 1)] = '\0';
            s_u8g2.drawStr(static_cast<int>(s_u8g2.getStrWidth("Username: ")), 35, uBuf);

            // Password line (row 4)
            if (loginMs >= kPassStart - kPassStep) {
                s_u8g2.drawStr(0, 52, "Password: ");
                uint8_t pChars = 0;
                if (loginMs >= kPassStart) {
                    pChars = static_cast<uint8_t>(
                        min((loginMs - kPassStart) / kPassStep + 1u, static_cast<uint32_t>(6)));
                }
                bool pDone = (pChars >= 6);
                char pBuf[8];
                memset(pBuf, '*', pChars);
                pBuf[pChars] = (pDone ? '\0' : (cursorOn ? '_' : ' '));
                pBuf[pChars + (pDone ? 0 : 1)] = '\0';
                s_u8g2.drawStr(static_cast<int>(s_u8g2.getStrWidth("Password: ")), 52, pBuf);
            }
        } else {
            // ---- NORMAL IDLE ----
            // The startup animation (if any) has played out; latch that so cold-start-only mode
            // never replays it on a later attract cycle.
            s_startupAnimShown = true;
            s_u8g2.drawStr(0, 13, s_cfgIdleLine1);
            s_u8g2.drawStr(0, 26, s_cfgIdleLine2);
            String ip = Wlan_GetIpAddress();
            s_u8g2.drawStr(0, 39, ip.length() > 0 ? ip.c_str() : "NO WIFI");
            bool cursorOn = (now / 500u) % 2u == 0u;
            s_u8g2.drawStr(0, 56, cursorOn ? "READY_" : "READY ");
        }

        Display_Send();
        return;
    }
    s_lastPlayMode = gPlayProperties.playMode;

    // ---- PLAYING SCREEN ----
    // Title: larger font (6x13, still 6px wide so the 21-char wrapping stays valid)
    s_u8g2.setFont(u8g2_font_6x13_tf);
    Display_DrawTitle(gPlayProperties.title, 12, 26, 40);

    // Status bar (small font_5x7): XX% left | elapsed/total centred | "WIFI" right (blank if no wifi)
    s_u8g2.setFont(u8g2_font_5x7_tf);

    // Battery – left
#ifdef BATTERY_MEASURE_ENABLE
    if (s_cfgShowBattery) {
        char batBuf[8];
        snprintf(batBuf, sizeof(batBuf), "%d%%",
                 static_cast<int>(Battery_EstimateLevel() * 100.0f));
        s_u8g2.drawStr(0, 60, batBuf);
    }
#endif

    // Elapsed / total – centred
    if (s_cfgShowTime) {
        uint32_t elapsed  = AudioPlayer_GetCurrentTime();
        uint32_t duration = AudioPlayer_GetFileDuration();
        char timeBuf[16];
        if (gPlayProperties.isWebstream || duration == 0) {
            snprintf(timeBuf, sizeof(timeBuf), "%d:%02d", elapsed / 60, elapsed % 60);
        } else {
            snprintf(timeBuf, sizeof(timeBuf), "%d:%02d/%d:%02d",
                     elapsed / 60, elapsed % 60,
                     duration / 60, duration % 60);
        }
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(timeBuf)) / 2), 60, timeBuf);
    }

    // WiFi – right ("wifi" when connected, nothing when not)
    if (s_cfgShowWifi && Wlan_IsConnected()) {
        const char *wifiStr = "WIFI";
        s_u8g2.drawStr(static_cast<int>(128 - s_u8g2.getStrWidth(wifiStr)), 60, wifiStr);
    }

    Display_Send();
}

#endif // OLED_ENABLE
