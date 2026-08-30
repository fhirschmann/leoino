// clang-format off
#include "settings.h"

#ifdef OLED_ENABLE

#include "AudioPlayer.h"
#include "Battery.h"
#include "Display.h"
#include "Led.h"
#include "Log.h"
#include "SdCard.h"
#include "System.h"
#include "Web.h"
#include "Webdav.h"
#include "Wlan.h"
#include "gitrevision.h"
#include "values.h"

#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>

extern TwoWire i2cBusTwo;

// -------- runtime configuration (web-settings, persisted in NVS) --------
// Startup-animation selector for the idle/attract screen.
enum class StartupAnim : uint8_t { None = 0, Boot = 1, Login = 2, Full = 3 };

enum class QuickMenuView : uint8_t { Closed, List, Status, Equalizer, FirmwareUpdate, Confirm };

struct QuickMenuItem {
    const char *id;
    const char *label;
    QuickMenuView infoView;
    uint16_t command;
    bool confirm;
};

static constexpr QuickMenuItem kQuickMenuItems[] = {
    {"status", "STATUS", QuickMenuView::Status, CMD_NOTHING, false},
    {"equalizer", "EQUALIZER", QuickMenuView::Equalizer, CMD_TOGGLE_EQUALIZER, false},
    {"nightmode", "NIGHT MODE", QuickMenuView::Closed, CMD_DIMM_LEDS_NIGHTMODE, false},
    {"webdav", "WEBDAV", QuickMenuView::Closed, CMD_TOGGLE_WEBDAV_SERVER, false},
    {"fwupdate", "FIRMWARE UPDATE", QuickMenuView::Closed, CMD_FIRMWARE_UPDATE, true},
    {"shutdown", "SHUTDOWN", QuickMenuView::Closed, CMD_SLEEPMODE, true},
};
static constexpr uint8_t kQuickMenuDefinitionCount = sizeof(kQuickMenuItems) / sizeof(kQuickMenuItems[0]);

struct QuickMenuEqProfile {
    const char *id;
    const char *label;
};

static constexpr QuickMenuEqProfile kQuickMenuEqProfiles[] = {
    {"flat", "FLAT"},
    {"music", "MUSIC"},
    {"speech", "SPEECH"},
    {"voiceBoost", "VOICE BOOST"},
};
static constexpr uint8_t kQuickMenuEqProfileCount = sizeof(kQuickMenuEqProfiles) / sizeof(kQuickMenuEqProfiles[0]);

static constexpr char kDefaultIdleLine1[] = "LEO INDUSTRIES";
static constexpr char kDefaultIdleLine2[] = "AUDIO TERMINAL AT-1";

static bool        s_cfgEnabled    = true;                       // master on/off (oledEnable)
static StartupAnim s_cfgStartAnim  = StartupAnim::Full;          // oledStartAnim
static bool        s_cfgAnimColdOnly = false;                    // oledAnimCold – only run the startup anim on a real power-on
static bool        s_cfgShowBattery = true;                      // oledShowBatt – battery % on playing screen
static bool        s_cfgShowArtist  = true;                      // oledShowArtist – prepend the ID3 artist to the playing title
static bool        s_cfgShowTime   = true;                       // oledShowTime – elapsed/total on playing screen
static bool        s_cfgShowWifi   = true;                       // oledShowWifi – WIFI marker on playing screen
static bool        s_cfgShowVolume = true;                       // oledShowVol – full-screen volume overlay
static bool        s_cfgFlip       = false;                      // oledFlip – rotate the panel by 180°
static char        s_cfgIdleLine1[32] = "";                      // oledIdleL1 – idle header line 1
static char        s_cfgIdleLine2[32] = "";                      // oledIdleL2 – idle header line 2
static uint16_t    s_cfgIdleTimeout = 0;                         // oledIdleTimeout – blank the panel after N s idle (0 = off)
static uint8_t     s_cfgContrast    = 255;                       // oledContrast – panel brightness/contrast 0..255
static bool        s_cfgShowClock   = false;                     // oledShowClock – show the RTC time on the idle screen
static bool        s_cfgClock24h    = true;                      // oledClock24h – 24h vs 12h clock format
static bool        s_cfgBurnIn      = false;                     // oledBurnIn – pixel-shift the idle content (anti burn-in)
static bool        s_cfgInvert      = false;                     // oledInvert – invert the whole panel
static char        s_cfgLoginUser[16] = "leo";                  // oledLoginUser – username typed in the login splash
static char        s_cfgBootText[16]  = "Booting";              // oledBootText – word shown with the boot dots
static uint8_t     s_cfgLoginPwLen  = 6;                         // oledLoginPwLen – number of password asterisks
static uint8_t     s_cfgAnimSpeed   = 1;                         // oledAnimSpeed – 0 slow, 1 normal, 2 fast
static bool        s_cfgTrackNum    = false;                     // oledTrackNum – show "N/M" track position on the playing screen
static uint8_t     s_cfgTimeMode    = 0;                         // oledTimeMode – 0 elapsed/total, 1 remaining, 2 elapsed
static bool        s_cfgStatusInv   = false;                     // oledStatusInv – draw the playing status-bar inverted
static bool        s_cfgIdleBatt    = false;                     // oledIdleBatt – show battery % on the idle screen
static uint32_t    s_cfgQuickMenuTimeoutMs = OLED_MENU_TIMEOUT_DEFAULT_SECONDS * 1000u;
static bool        s_cfgQuickMenuRemember = false;
static uint8_t     s_cfgQuickMenuOrder[kQuickMenuDefinitionCount] = {};
static uint8_t     s_cfgQuickMenuItemCount = 0;

static void Display_LoadQuickMenuConfig(void);

// Boot/login animation phase timings at normal speed. The runtime copies below are these scaled by
// s_cfgAnimSpeed in Display_LoadConfig.
static constexpr uint32_t kBootDurationMs  = 3000;  // boot screen
static constexpr uint32_t kLoginDurationMs = 3500;  // login animation
static constexpr uint32_t kBootLine1Ms  = 0;        // boot line 1 appears immediately
static constexpr uint32_t kBootLine2Ms  = 750;      // boot line 2 appears here
static constexpr uint32_t kBootDotsMs   = 1000;     // dots start cycling from here
static constexpr uint32_t kBootDotCycle = 600;      // ms per dot step
static constexpr uint32_t kUserStart    = 500;      // login: username starts typing (relative to boot end)
static constexpr uint32_t kUserStep     = 220;      // ms per username char
static constexpr uint32_t kPassStart    = 1400;     // login: password starts typing
static constexpr uint32_t kPassStep     = 220;      // ms per password char

// Runtime (speed-scaled) copies of the timings above.
static uint32_t s_bootDurMs, s_loginDurMs, s_bootLine2Ms, s_bootDotsMs, s_bootDotCycleMs;
static uint32_t s_userStartMs, s_userStepMs, s_passStartMs, s_passStepMs;

// Pull the OLED settings out of NVS into the cached statics above.
static void Display_LoadConfig(void) {
    s_cfgEnabled     = gPrefsSettings.getBool("oledEnable", true);
    uint8_t anim     = gPrefsSettings.getUChar("oledStartAnim", static_cast<uint8_t>(StartupAnim::Full));
    if (anim > static_cast<uint8_t>(StartupAnim::Full)) anim = static_cast<uint8_t>(StartupAnim::Full);
    s_cfgStartAnim   = static_cast<StartupAnim>(anim);
    s_cfgAnimColdOnly = gPrefsSettings.getBool("oledAnimCold", false);
    s_cfgShowBattery = gPrefsSettings.getBool("oledShowBatt", true);
    s_cfgShowArtist  = gPrefsSettings.getBool("oledShowArtist", true);
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

    s_cfgIdleTimeout = gPrefsSettings.getUShort("oledIdleTimeout", 0);
    s_cfgContrast    = gPrefsSettings.getUChar("oledContrast", 255);
    s_cfgShowClock   = gPrefsSettings.getBool("oledShowClock", false);
    s_cfgClock24h    = gPrefsSettings.getBool("oledClock24h", true);
    s_cfgBurnIn      = gPrefsSettings.getBool("oledBurnIn", false);
    s_cfgInvert      = gPrefsSettings.getBool("oledInvert", false);
    String lu        = gPrefsSettings.getString("oledLoginUser", "leo");
    strncpy(s_cfgLoginUser, lu.c_str(), sizeof(s_cfgLoginUser) - 1);
    s_cfgLoginUser[sizeof(s_cfgLoginUser) - 1] = '\0';
    String bt        = gPrefsSettings.getString("oledBootText", "Booting");
    strncpy(s_cfgBootText, bt.c_str(), sizeof(s_cfgBootText) - 1);
    s_cfgBootText[sizeof(s_cfgBootText) - 1] = '\0';
    s_cfgLoginPwLen  = gPrefsSettings.getUChar("oledLoginPwLen", 6);
    if (s_cfgLoginPwLen < 1) s_cfgLoginPwLen = 1;
    if (s_cfgLoginPwLen > 12) s_cfgLoginPwLen = 12;
    s_cfgAnimSpeed   = gPrefsSettings.getUChar("oledAnimSpeed", 1);
    if (s_cfgAnimSpeed > 2) s_cfgAnimSpeed = 1;
    s_cfgTrackNum    = gPrefsSettings.getBool("oledTrackNum", false);
    s_cfgTimeMode    = gPrefsSettings.getUChar("oledTimeMode", 0);
    if (s_cfgTimeMode > 2) s_cfgTimeMode = 0;
    s_cfgStatusInv   = gPrefsSettings.getBool("oledStatusInv", false);
    s_cfgIdleBatt    = gPrefsSettings.getBool("oledIdleBatt", false);
    uint8_t menuTimeout = gPrefsSettings.getUChar("oledMenuTout", OLED_MENU_TIMEOUT_DEFAULT_SECONDS);
    if (menuTimeout < 1) menuTimeout = 1;
    if (menuTimeout > 30) menuTimeout = 30;
    s_cfgQuickMenuTimeoutMs = static_cast<uint32_t>(menuTimeout) * 1000u;
    s_cfgQuickMenuRemember = gPrefsSettings.getBool("oledMenuRem", false);
    Display_LoadQuickMenuConfig();

    // Scale the boot/login animation timings by the chosen speed (0 slow ×3/2, 1 normal ×1, 2 fast ×3/5).
    const uint32_t sn = (s_cfgAnimSpeed == 0) ? 3u : (s_cfgAnimSpeed == 2) ? 3u : 1u;
    const uint32_t sd = (s_cfgAnimSpeed == 0) ? 2u : (s_cfgAnimSpeed == 2) ? 5u : 1u;
    s_bootDurMs      = kBootDurationMs * sn / sd;
    s_loginDurMs     = kLoginDurationMs * sn / sd;
    s_bootLine2Ms    = kBootLine2Ms * sn / sd;
    s_bootDotsMs     = kBootDotsMs * sn / sd;
    s_bootDotCycleMs = kBootDotCycle * sn / sd;
    s_userStartMs    = kUserStart * sn / sd;
    s_userStepMs     = kUserStep * sn / sd;
    s_passStartMs    = kPassStart * sn / sd;
    s_passStepMs     = kPassStep * sn / sd;
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

// -------- OLED quick menu --------
// One press of a button mapped to CMD_OLED_MENU opens the list; the encoder moves the selection and
// the same command confirms it. Informational/result pages return to the list on the next press;
// firmware progress stays visible while Cmd.cpp dispatches the OTA. Any interaction restarts the timeout.
static QuickMenuView s_quickMenuView = QuickMenuView::Closed;
static uint8_t s_quickMenuSelection = 0;
static uint32_t s_quickMenuTouchedAt = 0;
static uint16_t s_quickMenuPendingCommand = CMD_NOTHING;
static uint8_t s_quickMenuLastOtaStatus = 0;
static uint8_t s_quickMenuEqSelection = 0;

static void Display_SelectCurrentEqProfile(void) {
    const String current = AudioPlayer_GetEqualizerProfile();
    s_quickMenuEqSelection = 0;
    for (uint8_t i = 0; i < kQuickMenuEqProfileCount; i++) {
        if (current.equals(kQuickMenuEqProfiles[i].id)) {
            s_quickMenuEqSelection = i;
            return;
        }
    }
}

static void Display_LoadQuickMenuConfig(void) {
    const String configured = gPrefsSettings.getString("oledMenuItems", OLED_MENU_ITEMS_DEFAULT);
    bool seen[kQuickMenuDefinitionCount] = {};
    s_cfgQuickMenuItemCount = 0;

    int start = 0;
    while (start < configured.length()) {
        int end = configured.indexOf(',', start);
        if (end < 0) end = configured.length();
        String token = configured.substring(start, end);
        token.trim();
        bool enabled = true;
        if (token.startsWith("-")) {
            enabled = false;
            token.remove(0, 1);
        }
        for (uint8_t i = 0; i < kQuickMenuDefinitionCount; i++) {
            if (!seen[i] && token.equals(kQuickMenuItems[i].id)) {
                seen[i] = true;
                if (enabled) s_cfgQuickMenuOrder[s_cfgQuickMenuItemCount++] = i;
                break;
            }
        }
        start = end + 1;
    }

    // A malformed API request must not create an unusable empty menu. The web UI prevents this too,
    // but the firmware remains authoritative for direct REST/WebSocket clients.
    if (s_cfgQuickMenuItemCount == 0) {
        s_cfgQuickMenuOrder[0] = 0; // STATUS
        s_cfgQuickMenuItemCount = 1;
    }
    if (s_quickMenuSelection >= s_cfgQuickMenuItemCount) s_quickMenuSelection = 0;
}

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
// Right edge for right-aligned idle text (clock, battery). A few px short of 128 because the
// SH1106 can clip the last column(s) and the burn-in shift would otherwise push text off the edge.
static constexpr int      kIdleRightEdge = 124;
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
static bool     s_warmReinit       = false; // set by Display_ReloadConfig so Display_Init skips the cold-boot settle delay
static bool     s_startupAnimShown = false; // the startup animation has already run to completion once


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

static void Display_DrawTitle(const char *rawTitle, const char *rawArtist, uint8_t y1, uint8_t y2, uint8_t y3) {
    char stripped[256];
    stripPathAndExt(rawTitle, stripped, sizeof(stripped));
    char titleOnly[256];
    utf8ToLatin1(stripped, titleOnly, sizeof(titleOnly));

    // Optionally prepend the ID3/Vorbis artist as "Artist - Title". Done AFTER stripping so a
    // path-style title can't swallow the artist; the existing " - " splitter then naturally puts
    // the artist on its own line. The change-detection + scroll key below uses the combined string.
    char title[384];
    if (rawArtist && rawArtist[0] != '\0') {
        char artist[160];
        utf8ToLatin1(rawArtist, artist, sizeof(artist));
        if (titleOnly[0] != '\0') {
            strlcpy(title, artist, sizeof(title));
            strlcat(title, " - ", sizeof(title));
            strlcat(title, titleOnly, sizeof(title));
        } else {
            snprintf(title, sizeof(title), "%s", artist);
        }
    } else {
        snprintf(title, sizeof(title), "%s", titleOnly);
    }
    size_t len = strlen(title);

    if (strncmp(title, s_lastRawTitle, sizeof(s_lastRawTitle)) != 0) {
        strncpy(s_lastRawTitle, title, sizeof(s_lastRawTitle) - 1);
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
    s_u8g2.setContrast(s_cfgContrast); // panel brightness/contrast
    s_u8g2.sendF("c", s_cfgInvert ? 0x0A7 : 0x0A6); // SH1106: A7 = inverse, A6 = normal
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
    // Skip the ~25 ms I2C transfer when the framebuffer is identical to the last frame the
    // panel acknowledged — most cycles the screen is static (title, icons; the clock only
    // ticks once per second). This frees a big slice of loopTask time every 100 ms cycle.
    // A frame is still pushed (a) after any send error, so a NACKed frame gets resent and
    // the panel keeps its self-correcting property, and (b) at the latest every 30 skipped
    // cycles (~3 s), so a frame corrupted on the wire while the screen is static heals too.
    static uint32_t s_lastFrameHash = 0;
    static uint8_t s_skippedFrames = 0;
    const uint8_t *buf = s_u8g2.getBufferPtr();
    const size_t bufLen = 8u * s_u8g2.getBufferTileHeight() * s_u8g2.getBufferTileWidth();
    uint32_t hash = 2166136261u; // FNV-1a
    for (size_t i = 0; i < bufLen; i++) {
        hash = (hash ^ buf[i]) * 16777619u;
    }
    if (hash == s_lastFrameHash && s_consecSendErrors == 0 && s_skippedFrames < 30) {
        s_skippedFrames++;
        return;
    }
    s_lastFrameHash = hash;
    s_skippedFrames = 0;

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
    // The 200 ms power-rail settle in Display_HwInit is only needed on a real cold boot. A runtime
    // re-init (Display_ReloadConfig, on the async_tcp task when OLED settings are saved) sets
    // s_warmReinit so we skip it and don't stall the web server. read-and-clear so the next call
    // defaults back to the cold-boot path.
    bool coldBoot = !s_warmReinit;
    s_warmReinit = false;
    if (Display_HwInit(coldBoot)) {
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
        s_warmReinit = true; // skip the cold-boot settle delay: this is a runtime re-init, rail is stable
        Display_Init(); // re-reads the config, but that is harmless
        return;
    }
    I2cBusTwo_Lock();
    s_u8g2.setFlipMode(s_cfgFlip ? 1 : 0);
    s_u8g2.setContrast(s_cfgContrast); // apply a changed brightness live
    s_u8g2.sendF("c", s_cfgInvert ? 0x0A7 : 0x0A6); // apply a changed invert-mode live
    I2cBusTwo_Unlock();
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

bool Display_MenuIsActive(void) {
    if (s_quickMenuView == QuickMenuView::Closed) return false;
    if (s_quickMenuView == QuickMenuView::FirmwareUpdate) {
        const uint8_t otaStatus = Web_GetGithubOtaStatus();
        if (otaStatus != s_quickMenuLastOtaStatus) {
            s_quickMenuLastOtaStatus = otaStatus;
            s_quickMenuTouchedAt = millis();
        }
        // Keep progress visible for the whole download/check. A terminal result then remains on
        // screen for the configured menu timeout before returning to the normal display.
        if (otaStatus == 1) return true;
    }
    if (millis() - s_quickMenuTouchedAt >= s_cfgQuickMenuTimeoutMs) {
        s_quickMenuView = QuickMenuView::Closed;
        s_quickMenuPendingCommand = CMD_NOTHING;
        return false;
    }
    return true;
}

bool Display_MenuPress(uint16_t *selectedCommand) {
    if (selectedCommand != nullptr) *selectedCommand = CMD_NOTHING;
    if (!s_cfgEnabled || !s_displayOk) return false;

    const uint32_t now = millis();
    if (!Display_MenuIsActive()) {
        s_quickMenuView = QuickMenuView::List;
        if (!s_cfgQuickMenuRemember || s_quickMenuSelection >= s_cfgQuickMenuItemCount) {
            s_quickMenuSelection = 0;
        }
        s_quickMenuTouchedAt = now;
        return true;
    }

    s_quickMenuTouchedAt = now;
    if (s_quickMenuView == QuickMenuView::Confirm) {
        const uint16_t pendingCommand = s_quickMenuPendingCommand;
        s_quickMenuView = pendingCommand == CMD_FIRMWARE_UPDATE ? QuickMenuView::FirmwareUpdate : QuickMenuView::Closed;
        s_quickMenuLastOtaStatus = Web_GetGithubOtaStatus();
        if (selectedCommand != nullptr) *selectedCommand = pendingCommand;
        s_quickMenuPendingCommand = CMD_NOTHING;
        return true;
    }
    if (s_quickMenuView == QuickMenuView::Equalizer) {
        AudioPlayer_SetEqualizerProfile(kQuickMenuEqProfiles[s_quickMenuEqSelection].id);
        s_quickMenuView = QuickMenuView::List;
        return true;
    }
    if (s_quickMenuView != QuickMenuView::List) {
        s_quickMenuView = QuickMenuView::List;
        return true;
    }

    const QuickMenuItem &item = kQuickMenuItems[s_cfgQuickMenuOrder[s_quickMenuSelection]];
    if (item.infoView != QuickMenuView::Closed) {
        if (item.infoView == QuickMenuView::Equalizer) Display_SelectCurrentEqProfile();
        s_quickMenuView = item.infoView;
        return true;
    }

    const int8_t firmwareUpToDate = Web_GetFirmwareUpToDate();
    const uint8_t otaStatus = Web_GetGithubOtaStatus();
    if (item.command == CMD_FIRMWARE_UPDATE &&
        (otaStatus == 1 || firmwareUpToDate == 1 || (firmwareUpToDate < 0 && otaStatus == 2))) {
        s_quickMenuLastOtaStatus = otaStatus;
        s_quickMenuView = QuickMenuView::FirmwareUpdate;
        return true;
    }

    if (item.confirm) {
        s_quickMenuPendingCommand = item.command;
        s_quickMenuView = QuickMenuView::Confirm;
        return true;
    }

    s_quickMenuView = QuickMenuView::Closed;
    if (selectedCommand != nullptr) *selectedCommand = item.command;
    return true;
}

void Display_MenuRotate(int32_t detents) {
    if (!Display_MenuIsActive() || detents == 0) return;

    if (s_quickMenuView == QuickMenuView::Equalizer) {
        const int32_t count = static_cast<int32_t>(kQuickMenuEqProfileCount);
        int32_t next = (static_cast<int32_t>(s_quickMenuEqSelection) + detents) % count;
        if (next < 0) next += count;
        s_quickMenuEqSelection = static_cast<uint8_t>(next);
        s_quickMenuTouchedAt = millis();
        return;
    }

    if (s_quickMenuView != QuickMenuView::List) {
        // Turning from an info/confirmation/result page returns to the list; the same detent continues
        // from there, so browsing never needs an extra button press. An already-started OTA continues.
        s_quickMenuView = QuickMenuView::List;
        s_quickMenuPendingCommand = CMD_NOTHING;
    }
    const int32_t count = static_cast<int32_t>(s_cfgQuickMenuItemCount);
    int32_t next = (static_cast<int32_t>(s_quickMenuSelection) + detents) % count;
    if (next < 0) next += count;
    s_quickMenuSelection = static_cast<uint8_t>(next);
    s_quickMenuTouchedAt = millis();
}

// Format the current wall-clock time (RTC-backed system clock) into buf. Returns false when the
// clock hasn't been set yet, so the caller can simply skip drawing it.
static bool Display_FormatClock(char *buf, size_t n) {
    time_t t = time(nullptr);
    if (t < 1700000000) return false; // ~2023-11: clock not set (no RTC/NTP time yet)
    struct tm lt;
    localtime_r(&t, &lt);
    if (s_cfgClock24h) {
        snprintf(buf, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
    } else {
        int h = lt.tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(buf, n, "%d:%02d%c", h, lt.tm_min, lt.tm_hour < 12 ? 'a' : 'p');
    }
    return true;
}

static const char *Display_QuickMenuPlayState(void) {
    if (System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) return "BT SPEAKER";
    if (System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE) return "BT HEADSET";
    if (gPlayProperties.playMode == BUSY) return "LOADING";
    if (gPlayProperties.playMode == NO_PLAYLIST) return "IDLE";
    return gPlayProperties.pausePlay ? "PAUSED" : "PLAYING";
}

static void Display_FormatQuickMenuLabel(uint8_t definitionIndex, char *buf, size_t n) {
    const QuickMenuItem &item = kQuickMenuItems[definitionIndex];
    if (item.infoView == QuickMenuView::Status) {
        snprintf(buf, n, "STATUS: %s", Display_QuickMenuPlayState());
    } else if (item.command == CMD_TOGGLE_EQUALIZER) {
        String eq = AudioPlayer_GetEqualizerProfile();
        eq.toUpperCase();
        snprintf(buf, n, "EQ: %s", eq.c_str());
    } else if (item.command == CMD_DIMM_LEDS_NIGHTMODE) {
#ifdef NEOPIXEL_ENABLE
        snprintf(buf, n, "NIGHT MODE: %s", Led_GetNightmode() ? "ON" : "OFF");
#else
        snprintf(buf, n, "NIGHT MODE: N/A");
#endif
    } else if (item.command == CMD_TOGGLE_WEBDAV_SERVER) {
        snprintf(buf, n, "WEBDAV: %s", Webdav_IsServerRunning() ? "ON" : "OFF");
    } else if (item.command == CMD_FIRMWARE_UPDATE) {
        const int8_t upToDate = Web_GetFirmwareUpToDate();
        const uint8_t otaStatus = Web_GetGithubOtaStatus();
        const bool confirmedCurrent = upToDate == 1 || (upToDate < 0 && otaStatus == 2);
        if (otaStatus == 1) {
            snprintf(buf, n, "FW: UPDATING %u%%", Web_GetGithubOtaProgress());
        } else {
            snprintf(buf, n, "%s", confirmedCurrent ? "FW: UP TO DATE" :
                                upToDate == 0 ? "FW: UPDATE AVAILABLE" : "FIRMWARE UPDATE");
        }
    } else {
        snprintf(buf, n, "%s", item.label);
    }
}

static void Display_DrawQuickMenuTimeout(uint32_t now) {
    const uint32_t elapsed = now - s_quickMenuTouchedAt;
    const uint32_t usedWidth = elapsed >= s_cfgQuickMenuTimeoutMs ? 128u : elapsed * 128u / s_cfgQuickMenuTimeoutMs;
    const uint8_t remainingWidth = static_cast<uint8_t>(128u - usedWidth);
    if (remainingWidth > 0) s_u8g2.drawHLine(0, 63, remainingWidth);
}

static void Display_DrawQuickMenuList(uint32_t now) {
    s_u8g2.setFont(u8g2_font_5x7_tf);
    char header[28];
    snprintf(header, sizeof(header), "QUICK MENU       %u/%u",
             static_cast<unsigned>(s_quickMenuSelection + 1u), static_cast<unsigned>(s_cfgQuickMenuItemCount));
    s_u8g2.drawStr(0, 7, header);
    s_u8g2.drawHLine(0, 9, 128);

    // Four entries fit below the header. Keep one neighbouring entry visible while possible and
    // pin the window at the beginning/end; this also avoids a nearly empty final page.
    const uint8_t visible = s_cfgQuickMenuItemCount < 4u ? s_cfgQuickMenuItemCount : 4u;
    uint8_t first = 0;
    if (s_cfgQuickMenuItemCount > visible) {
        if (s_quickMenuSelection > 1u) first = s_quickMenuSelection - 1u;
        const uint8_t maxFirst = s_cfgQuickMenuItemCount - visible;
        if (first > maxFirst) first = maxFirst;
    }
    for (uint8_t row = 0; row < visible; row++) {
        const uint8_t selectionIndex = first + row;
        const uint8_t y = 20u + row * 12u;
        const bool selected = selectionIndex == s_quickMenuSelection;
        if (selected) {
            s_u8g2.drawBox(0, y - 9u, 128, 11);
            s_u8g2.setDrawColor(0);
        }
        char label[28];
        Display_FormatQuickMenuLabel(s_cfgQuickMenuOrder[selectionIndex], label, sizeof(label));
        s_u8g2.drawStr(3, y, label);
        if (selected) {
            s_u8g2.drawStr(119, y, ">");
            s_u8g2.setDrawColor(1);
        }
    }

    Display_DrawQuickMenuTimeout(now);
}

static void Display_DrawQuickMenuEqualizer(uint32_t now) {
    s_u8g2.setFont(u8g2_font_5x7_tf);
    const String current = AudioPlayer_GetEqualizerProfile();
    const char *currentLabel = "CUSTOM";
    for (uint8_t i = 0; i < kQuickMenuEqProfileCount; i++) {
        if (current.equals(kQuickMenuEqProfiles[i].id)) {
            currentLabel = kQuickMenuEqProfiles[i].label;
            break;
        }
    }

    char header[28];
    snprintf(header, sizeof(header), "EQUALIZER [%s]", currentLabel);
    s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(header)) / 2), 7, header);
    s_u8g2.drawHLine(0, 9, 128);

    for (uint8_t row = 0; row < kQuickMenuEqProfileCount; row++) {
        const uint8_t y = 20u + row * 12u;
        const bool selected = row == s_quickMenuEqSelection;
        const bool applied = current.equals(kQuickMenuEqProfiles[row].id);
        if (selected) {
            s_u8g2.drawBox(0, y - 9u, 128, 11);
            s_u8g2.setDrawColor(0);
        }
        s_u8g2.drawStr(5, y, kQuickMenuEqProfiles[row].label);
        if (applied) s_u8g2.drawStr(108, y, "*");
        if (selected) {
            s_u8g2.drawStr(119, y, ">");
            s_u8g2.setDrawColor(1);
        }
    }

    Display_DrawQuickMenuTimeout(now);
}

static void Display_DrawQuickMenuStatus(void) {
    s_u8g2.setFont(u8g2_font_5x7_tf);
    const char *header = "SYSTEM STATUS";
    s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(header)) / 2), 7, header);
    s_u8g2.drawHLine(0, 9, 128);

    char audio[32];
    char ip[32];
    char battery[32];
    char firmware[32];
    char uptime[32];
    char sd[32];
    snprintf(audio, sizeof(audio), "AUDIO:%s V:%u/%u", Display_QuickMenuPlayState(),
             AudioPlayer_GetCurrentVolume(), AudioPlayer_GetMaxVolume());
    const String ipAddress = Wlan_GetIpAddress();
    snprintf(ip, sizeof(ip), "IP:%s", ipAddress.length() > 0 ? ipAddress.c_str() : "NO WIFI");
#ifdef BATTERY_MEASURE_ENABLE
    const int percent = static_cast<int>(Battery_EstimateLevel() * 100.0f);
    const char *batteryState = Battery_IsCritical() ? "CRIT" : Battery_IsLow() ? "LOW" : "OK";
    snprintf(battery, sizeof(battery), "BAT:%d%% %.2fV %s", percent, Battery_GetVoltage(), batteryState);
#else
    snprintf(battery, sizeof(battery), "BAT:N/A");
#endif
    snprintf(firmware, sizeof(firmware), "FW:%s", softwareRevisionShort);
    const uint32_t uptimeMinutes = millis() / 60000u;
    snprintf(uptime, sizeof(uptime), "UP:%lud %02lu:%02lu H:%luK",
             static_cast<unsigned long>(uptimeMinutes / 1440u),
             static_cast<unsigned long>((uptimeMinutes / 60u) % 24u),
             static_cast<unsigned long>(uptimeMinutes % 60u),
             static_cast<unsigned long>(ESP.getFreeHeap() / 1024u));
    const SdCardHealth &sdHealth = SdCard_GetHealth();
    snprintf(sd, sizeof(sd), "SD:%s %luKHZ", sdHealth.mounted ? "OK" : "ERROR",
             static_cast<unsigned long>(sdHealth.frequencyKhz));

    s_u8g2.drawStr(1, 17, audio);
    s_u8g2.drawStr(1, 26, ip);
    s_u8g2.drawStr(1, 35, battery);
    s_u8g2.drawStr(1, 44, firmware);
    s_u8g2.drawStr(1, 53, uptime);
    s_u8g2.drawStr(1, 62, sd);
}

static void Display_DrawQuickMenuFirmwareUpdate(uint32_t now) {
    s_u8g2.setFont(u8g2_font_6x13_tf);
    const char *header = "FIRMWARE";
    s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(header)) / 2), 13, header);
    s_u8g2.drawHLine(0, 16, 128);

    const uint8_t otaStatus = Web_GetGithubOtaStatus();
    if (otaStatus == 2 || Web_GetFirmwareUpToDate() == 1) {
        const char *result = "UP TO DATE";
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(result)) / 2), 36, result);
        s_u8g2.setFont(u8g2_font_5x7_tf);
        char firmware[32];
        snprintf(firmware, sizeof(firmware), "FW: %s", softwareRevisionShort);
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(firmware)) / 2), 52, firmware);
        const char *detail = "NO UPDATE NEEDED";
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(detail)) / 2), 61, detail);
        Display_DrawQuickMenuTimeout(now);
        return;
    }

    if (otaStatus == 1) {
        const uint8_t progress = Web_GetGithubOtaProgress();
        const char *state = progress > 0 ? "DOWNLOADING" : "CHECKING...";
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(state)) / 2), 34, state);
        s_u8g2.drawFrame(9, 41, 110, 10);
        if (progress > 0) s_u8g2.drawBox(11, 43, static_cast<uint8_t>(progress * 106u / 100u), 6);
        s_u8g2.setFont(u8g2_font_5x7_tf);
        char percent[8];
        snprintf(percent, sizeof(percent), "%u%%", progress);
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(percent)) / 2), 61, percent);
        return;
    }

    const char *result = otaStatus == 3 ? "UPDATE FAILED" : "BUSY - TRY AGAIN";
    s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(result)) / 2), 37, result);
    s_u8g2.setFont(u8g2_font_5x7_tf);
    char message[26];
    Web_GetGithubOtaMessage(message, sizeof(message));
    if (message[0] != '\0') s_u8g2.drawStr(2, 54, message);
    Display_DrawQuickMenuTimeout(now);
}

static void Display_DrawQuickMenuConfirm(uint32_t now) {
    s_u8g2.setFont(u8g2_font_6x13_tf);
    const char *header = s_quickMenuPendingCommand == CMD_FIRMWARE_UPDATE ? "UPDATE FIRMWARE?" : "SHUT DOWN?";
    s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(header)) / 2), 15, header);
    s_u8g2.drawFrame(8, 23, 112, 22);
    const char *confirm = "PRESS AGAIN";
    s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(confirm)) / 2), 39, confirm);
    s_u8g2.setFont(u8g2_font_5x7_tf);
    const char *cancel = "TURN TO CANCEL";
    s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(cancel)) / 2), 57, cancel);
    Display_DrawQuickMenuTimeout(now);
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
    bool shutdownScreen = System_IsShutdownPending();
    bool quickMenuScreen = Display_MenuIsActive();

    // Auto-off: once the panel has been blanked (see the idle branch below) bring it back the moment
    // anything happens — playback resumes or the volume overlay shows.
    static bool s_panelBlanked = false;
    if (s_panelBlanked && (!idle || volScreen || shutdownScreen || quickMenuScreen)) {
        I2cBusTwo_Lock();
        s_u8g2.setPowerSave(0);
        I2cBusTwo_Unlock();
        s_panelBlanked = false;
    }

    s_u8g2.clearBuffer();

    // ---- SHUTDOWN COUNTDOWN (takes priority over playback/volume/idle) ----
    // Teardown has not started yet, so every frame remains safe to draw and a physical button can
    // return directly to the previous screen without having to re-initialise any subsystem.
    if (shutdownScreen) {
        s_u8g2.setFont(u8g2_font_6x13_tf);
        const char *label = "SHUTTING DOWN...";
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(label)) / 2), 16, label);

        uint32_t const remainingSeconds = (System_GetShutdownRemainingMs() + 999u) / 1000u;
        char countdown[12];
        snprintf(countdown, sizeof(countdown), "%lus", static_cast<unsigned long>(remainingSeconds));
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(countdown)) / 2), 40, countdown);

        s_u8g2.setFont(u8g2_font_5x7_tf);
        const char *cancelHint = "PRESS ANY BUTTON";
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(cancelHint)) / 2), 60, cancelHint);
        Display_Send();
        return;
    }

    // ---- QUICK MENU (takes priority over volume/playback/idle) ----
    if (quickMenuScreen) {
        if (s_quickMenuView == QuickMenuView::List) {
            Display_DrawQuickMenuList(now);
        } else if (s_quickMenuView == QuickMenuView::Equalizer) {
            Display_DrawQuickMenuEqualizer(now);
        } else if (s_quickMenuView == QuickMenuView::Confirm) {
            Display_DrawQuickMenuConfirm(now);
        } else if (s_quickMenuView == QuickMenuView::FirmwareUpdate) {
            Display_DrawQuickMenuFirmwareUpdate(now);
        } else {
            Display_DrawQuickMenuStatus();
        }
        Display_Send();
        return;
    }

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

        // Auto-off: after the configured idle time, power the panel down (burn-in + power). It comes
        // back via the un-blank check above once playback resumes or the volume changes.
        if (s_cfgIdleTimeout > 0 && idleMs >= static_cast<uint32_t>(s_cfgIdleTimeout) * 1000u) {
            if (!s_panelBlanked) {
                I2cBusTwo_Lock();
                s_u8g2.clearBuffer();
                s_u8g2.sendBuffer();
                s_u8g2.setPowerSave(1);
                I2cBusTwo_Unlock();
                s_panelBlanked = true;
            }
            return;
        }

        // The startup/attract animation is selectable: it can show the boot screen, the
        // login splash, both (default) or nothing. Compute each phase's duration so the
        // disabled phases collapse to zero and we fall straight through to the idle screen.
        //
        // "Cold-start only" (oledAnimCold) restricts the animation to a genuine power-on via the
        // physical switch: it then plays exactly once and is skipped on every wake-from-deep-sleep
        // and on the attract re-runs that follow each playback. Off (default) keeps the original
        // behaviour where the animation plays on every idle entry.
        const bool animAllowed = !s_cfgAnimColdOnly || (!s_wokeFromSleep && !s_startupAnimShown);
        const uint32_t bootDur  = (animAllowed && (s_cfgStartAnim == StartupAnim::Boot || s_cfgStartAnim == StartupAnim::Full)) ? s_bootDurMs : 0u;
        const uint32_t loginDur = (animAllowed && (s_cfgStartAnim == StartupAnim::Login || s_cfgStartAnim == StartupAnim::Full)) ? s_loginDurMs : 0u;

        s_u8g2.setFont(u8g2_font_6x13_tf);

        if (idleMs < bootDur) {
            // ---- BOOT SCREEN ----
            if (idleMs >= kBootLine1Ms) s_u8g2.drawStr(0, 13, s_cfgIdleLine1);
            if (idleMs >= s_bootLine2Ms) s_u8g2.drawStr(0, 26, s_cfgIdleLine2);
            if (idleMs >= s_bootDotsMs) {
                uint32_t step = ((idleMs - s_bootDotsMs) / s_bootDotCycleMs) % 3u;
                char dotBuf[24];
                snprintf(dotBuf, sizeof(dotBuf), "%s%s", s_cfgBootText, step == 0 ? "." : step == 1 ? ".." : "...");
                s_u8g2.drawStr(0, 45, dotBuf);
            }
        } else if (idleMs < bootDur + loginDur) {
            // ---- LOGIN SPLASH ----
            uint32_t loginMs = idleMs - bootDur;
            bool cursorOn = (now / 400u) % 2u == 0u;

            // Header
            s_u8g2.drawStr(0, 13, "AT-1 LOGIN");

            // Username line (row 3) – types out the configurable username one char at a time
            s_u8g2.drawStr(0, 35, "Username: ");
            const char *uFull = s_cfgLoginUser;
            uint8_t uLen = static_cast<uint8_t>(strlen(uFull));
            uint8_t uChars = 0;
            if (loginMs >= s_userStartMs) {
                uChars = static_cast<uint8_t>(
                    min((loginMs - s_userStartMs) / s_userStepMs + 1u, static_cast<uint32_t>(uLen)));
            }
            char uBuf[18];
            memcpy(uBuf, uFull, uChars);
            bool uDone = (uChars >= uLen);
            uBuf[uChars] = (uDone ? '\0' : (cursorOn ? '_' : ' '));
            uBuf[uChars + (uDone ? 0 : 1)] = '\0';
            s_u8g2.drawStr(static_cast<int>(s_u8g2.getStrWidth("Username: ")), 35, uBuf);

            // Password line (row 4) – label appears a step before the asterisks start filling
            if (loginMs + s_passStepMs >= s_passStartMs) {
                s_u8g2.drawStr(0, 52, "Password: ");
                uint8_t pChars = 0;
                if (loginMs >= s_passStartMs) {
                    pChars = static_cast<uint8_t>(
                        min((loginMs - s_passStartMs) / s_passStepMs + 1u, static_cast<uint32_t>(s_cfgLoginPwLen)));
                }
                bool pDone = (pChars >= s_cfgLoginPwLen);
                char pBuf[16];
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

            // Anti burn-in: nudge the whole idle screen by a few pixels on a slow cycle so no pixel
            // stays lit in the same spot forever.
            int sx = 0, sy = 0;
            if (s_cfgBurnIn) {
                static const int8_t ox[4] = {0, 1, 2, 1};
                static const int8_t oy[4] = {0, 1, 0, -1};
                uint8_t ph = (now / 30000u) % 4u; // shift every 30 s
                sx = ox[ph];
                sy = oy[ph];
            }

            s_u8g2.drawStr(0 + sx, 13 + sy, s_cfgIdleLine1);
            s_u8g2.drawStr(0 + sx, 26 + sy, s_cfgIdleLine2);
            String ip = Wlan_GetIpAddress();
            s_u8g2.drawStr(0 + sx, 39 + sy, ip.length() > 0 ? ip.c_str() : "NO WIFI");
#ifdef BATTERY_MEASURE_ENABLE
            // Battery % – right-aligned on the IP row.
            if (s_cfgIdleBatt) {
                char batBuf[8];
                snprintf(batBuf, sizeof(batBuf), "%d%%", static_cast<int>(Battery_EstimateLevel() * 100.0f));
                s_u8g2.drawStr(kIdleRightEdge - static_cast<int>(s_u8g2.getStrWidth(batBuf)) + sx, 39 + sy, batBuf);
            }
#endif
            bool cursorOn = (now / 500u) % 2u == 0u;
            s_u8g2.drawStr(0 + sx, 56 + sy, cursorOn ? "READY_" : "READY ");

            // Clock (RTC) – right-aligned on the footer row, next to READY. Keep a few px of right
            // margin: the SH1106 can clip the last column(s), and the burn-in shift (+sx) would
            // otherwise push the last digit off the edge.
            if (s_cfgShowClock) {
                char clk[12];
                if (Display_FormatClock(clk, sizeof(clk))) {
                    s_u8g2.drawStr(kIdleRightEdge - static_cast<int>(s_u8g2.getStrWidth(clk)) + sx, 56 + sy, clk);
                }
            }
        }

        Display_Send();
        return;
    }
    s_lastPlayMode = gPlayProperties.playMode;

    // ---- PLAYING SCREEN ----
    // Title: larger font (6x13, still 6px wide so the 21-char wrapping stays valid)
    s_u8g2.setFont(u8g2_font_6x13_tf);
    Display_DrawTitle(gPlayProperties.title, s_cfgShowArtist ? gPlayProperties.artist : "", 12, 26, 40);

    // Status bar (small font_5x7): XX% left | time centred | "N/M" or "WIFI" right.
    // Optionally rendered as an inverted (filled) strip for a highlighted look.
    s_u8g2.setFont(u8g2_font_5x7_tf);
    if (s_cfgStatusInv) {
        s_u8g2.drawBox(0, 52, 128, 12); // highlight strip behind the status row (y 52..63)
        s_u8g2.setDrawColor(0); // draw the status text in black on top of the strip
    }

    // Battery – left
#ifdef BATTERY_MEASURE_ENABLE
    if (s_cfgShowBattery) {
        char batBuf[8];
        snprintf(batBuf, sizeof(batBuf), "%d%%",
                 static_cast<int>(Battery_EstimateLevel() * 100.0f));
        s_u8g2.drawStr(0, 60, batBuf);
    }
#endif

    // Time – centred, per the selected mode (0 elapsed/total, 1 remaining, 2 elapsed)
    if (s_cfgShowTime) {
        uint32_t elapsed  = AudioPlayer_GetCurrentTime();
        uint32_t duration = AudioPlayer_GetFileDuration();
        char timeBuf[24];
        if (gPlayProperties.isWebstream || duration == 0) {
            snprintf(timeBuf, sizeof(timeBuf), "%lu:%02lu",
                     static_cast<unsigned long>(elapsed / 60), static_cast<unsigned long>(elapsed % 60));
        } else if (s_cfgTimeMode == 1) {
            uint32_t rem = (duration > elapsed) ? (duration - elapsed) : 0u;
            snprintf(timeBuf, sizeof(timeBuf), "-%lu:%02lu",
                     static_cast<unsigned long>(rem / 60), static_cast<unsigned long>(rem % 60));
        } else if (s_cfgTimeMode == 2) {
            snprintf(timeBuf, sizeof(timeBuf), "%lu:%02lu",
                     static_cast<unsigned long>(elapsed / 60), static_cast<unsigned long>(elapsed % 60));
        } else {
            snprintf(timeBuf, sizeof(timeBuf), "%lu:%02lu/%lu:%02lu",
                     static_cast<unsigned long>(elapsed / 60), static_cast<unsigned long>(elapsed % 60),
                     static_cast<unsigned long>(duration / 60), static_cast<unsigned long>(duration % 60));
        }
        s_u8g2.drawStr(static_cast<int>((128 - s_u8g2.getStrWidth(timeBuf)) / 2), 60, timeBuf);
    }

    // Right slot: track position "N/M" (if enabled) takes priority over the WiFi marker.
    bool drewRight = false;
    if (s_cfgTrackNum) {
        size_t playlistSize = 0;
        uint16_t currentTrackNumber = 0;
        AudioPlayer_LockPlaylist();
        if (gPlayProperties.playlist != nullptr) {
            playlistSize = gPlayProperties.playlist->size();
        }
        currentTrackNumber = gPlayProperties.currentTrackNumber;
        AudioPlayer_UnlockPlaylist();
        if (playlistSize > 1 && currentTrackNumber < playlistSize) {
            char tnBuf[24];
            snprintf(tnBuf, sizeof(tnBuf), "%u/%u",
                     static_cast<unsigned>(currentTrackNumber + 1),
                     static_cast<unsigned>(playlistSize));
            s_u8g2.drawStr(static_cast<int>(128 - s_u8g2.getStrWidth(tnBuf)), 60, tnBuf);
            drewRight = true;
        }
    }
    if (!drewRight && s_cfgShowWifi && Wlan_IsConnected()) {
        const char *wifiStr = "WIFI";
        s_u8g2.drawStr(static_cast<int>(128 - s_u8g2.getStrWidth(wifiStr)), 60, wifiStr);
    }

    if (s_cfgStatusInv) s_u8g2.setDrawColor(1); // restore the normal draw colour

    Display_Send();
}

#endif // OLED_ENABLE
