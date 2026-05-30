/*
 * app_flipclock.cpp — Dot-matrix departure-board clock
 *
 * Portrait 368×448 (or landscape 448×368). Every character — digits and
 * letters — is drawn as a 5×7 LED dot matrix: dim "off" pixels for the empty
 * grid, amber "on" pixels for the lit glyph, mimicking the airport
 * departure-board look (Solari Mareus / split-flap successor era).
 *
 * NTP sync at startup via WiFi (reads SSID/PASSWORD from setup.txt).
 *
 * Controls:
 *   BOOT short — rotate display (portrait ↔ landscape)
 *   PWR  short — cycle brightness
 */

#include "app_flipclock.h"
#include "app_common.h"
#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <FS.h>
#include <time.h>
#include <pgmspace.h>
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include <Adafruit_XCA9554.h>
#include "font/glcdfont.h"     // 5×7 bitmap font (PROGMEM `font[]`)

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;

// ── Config ───────────────────────────────────────────────────────────────────
static char     cfg_ssid[3][64] = {};
static char     cfg_pass[3][64] = {};
static char     cfg_tz[64]      = "CET-1CEST,M3.5.0,M10.5.0/3";
static bool     cfg_color_set   = false;    // true if CLOCK_COLOR overrides defaults
static uint16_t cfg_color       = 0xFFFF;   // override color, RGB565

// ── State ────────────────────────────────────────────────────────────────────
static Arduino_Canvas *canvas = nullptr;
static Arduino_SH8601 *s_gfx = nullptr;
static int  s_prevMin  = -1;
static int  s_prevHour = -1;
static int  s_prevDay  = -1;
static uint32_t s_lastCheck = 0;
static bool     s_bootWas   = false;
static uint8_t  s_rot       = 0;
static uint8_t  s_brightIdx = 0;
static const uint8_t BRIGHT_LEVELS[] = { 255, 180, 100, 40, 10 };

// ── Departure-board palette (sampled from reference) ─────────────────────────
#define COL_BG       0x0000   // pure black
#define DOT_OFF      0x18C3   // dark grey #1A1A1A — unlit LED
#define DOT_YELLOW   0xFE60   // saturated yellow #FBCC04 — lit
#define DOT_WHITE    0xFFFF   // pure white                   — lit
#define COL_WHITE    0xFFFF
#define COL_GREY     0x7BEF

// ── Weekday / month names (full, all upper case) ─────────────────────────────
static const char *WDAY[] = {
    "SUNDAY","MONDAY","TUESDAY","WEDNESDAY","THURSDAY","FRIDAY","SATURDAY"
};
static const char *MON[] = {
    "JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
    "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"
};

// ── Hex color parser → RGB565 ────────────────────────────────────────────────
static uint16_t parseHexColor(const char *s) {
    if (!s || !*s) return 0xFFFF;
    if (*s == '#') s++;
    uint32_t v = 0;
    int digits = 0;
    while (*s && digits < 6) {
        char c = *s++;
        int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + d;
        digits++;
    }
    if (digits < 6) return 0xFFFF;
    uint8_t r = (v >> 16) & 0xFF;
    uint8_t g = (v >> 8) & 0xFF;
    uint8_t b = v & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// ── Config parser ────────────────────────────────────────────────────────────
static bool extractVal(const char *line, const char *key, char *out, size_t cap) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    size_t kl = strlen(key);
    if (strncmp(p, key, kl) != 0) return false;
    char after = p[kl];
    if (isalnum((unsigned char)after) || after == '_') return false;
    p += kl;
    while (*p == ' ' || *p == '=') p++;
    if (*p == '"') p++;
    size_t n = 0;
    while (*p && *p != '"' && *p != '\n' && *p != '\r' && n < cap - 1)
        out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
}

static bool readConfig() {
    File f = SD_MMC.open("/setup/setup.txt");
    if (!f) return false;
    char line[160];
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        extractVal(line, "SSID",      cfg_ssid[0], 64);
        extractVal(line, "PASSWORD",  cfg_pass[0], 64);
        extractVal(line, "SSID2",     cfg_ssid[1], 64);
        extractVal(line, "PASSWORD2", cfg_pass[1], 64);
        extractVal(line, "SSID3",     cfg_ssid[2], 64);
        extractVal(line, "PASSWORD3", cfg_pass[2], 64);
        extractVal(line, "TIMEZONE",  cfg_tz, 64);
        char utcBuf[16] = {};
        if (extractVal(line, "UTC_OFFSET", utcBuf, 16) && cfg_tz[0] == '\0') {
            int h = atoi(utcBuf);
            snprintf(cfg_tz, sizeof(cfg_tz), "UTC%+d", -h);
        }
        char colorBuf[16] = {};
        if (extractVal(line, "CLOCK_COLOR", colorBuf, 16)) {
            cfg_color = parseHexColor(colorBuf);
            cfg_color_set = true;
        }
    }
    f.close();
    return cfg_ssid[0][0] != '\0';
}

// ── Status splash ────────────────────────────────────────────────────────────
static void showStatus(const char *l1, const char *l2 = nullptr) {
    canvas->setFont();
    canvas->fillScreen(COL_BG);
    canvas->setTextColor(COL_WHITE); canvas->setTextSize(2);
    canvas->setCursor(16, LCD_HEIGHT / 2 - 16); canvas->print(l1);
    if (l2) {
        canvas->setTextColor(COL_GREY);
        canvas->setCursor(16, LCD_HEIGHT / 2 + 12); canvas->print(l2);
    }
    canvas->flush();
}

// ── WiFi + NTP sync ──────────────────────────────────────────────────────────
static bool syncTime() {
    showStatus("Connecting WiFi...", cfg_ssid[0][0] ? cfg_ssid[0] : "");
    WifiCred list[3] = {
        { cfg_ssid[0], cfg_pass[0] },
        { cfg_ssid[1], cfg_pass[1] },
        { cfg_ssid[2], cfg_pass[2] },
    };
    if (wifi_try_connect(list, 3) < 0) return false;

    showStatus("Syncing NTP...");
    configTzTime(cfg_tz, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", cfg_tz, 1);
    tzset();
    delay(2000);

    struct tm ti = {};
    bool ok = false;
    for (int t = 0; t < 15; t++) {
        if (getLocalTime(&ti, 1000) && ti.tm_year > 100) { ok = true; break; }
        delay(500);
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    USBSerial.printf("[clock] NTP sync %s: %d-%02d-%02d %02d:%02d\n",
        ok ? "OK" : "FAIL", ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday,
        ti.tm_hour, ti.tm_min);
    return ok;
}

// ── Cell layout (per text row) ───────────────────────────────────────────────
// Each letter occupies a 5-wide × 7-tall dot cell, with a 1-dot gap column
// between adjacent cells. The off-dot grid spans the full screen width;
// glyph-lit dot positions are overpainted in the on-color (same radius —
// only the color differs from the unlit dots).
#define CELL_COLS  5
#define CELL_ROWS  7
#define CELL_GAP   1

// Pick the largest pitch where N letter cells (each 5+1 cols, last cell no gap)
// still fit within `maxW`.
static int16_t pitchForCells(int n, int16_t maxW, int16_t maxPitch, int16_t minPitch) {
    int16_t cellSpan = n * (CELL_COLS + CELL_GAP) - CELL_GAP;   // dot columns
    if (cellSpan <= 0) cellSpan = 1;
    for (int16_t p = maxPitch; p >= minPitch; p--) {
        if (cellSpan * p <= maxW) return p;
    }
    return minPitch;
}

// ── Draw one text row: cells are 5×7 dot islands separated by empty gap ──────
// The grey grid is rendered as a sequence of 5×7 cells (gap columns left
// black) spanning the full row width. The text is right-aligned: the lit
// glyphs land in the rightmost N cells.
//
// y          : top y of the row (inside the canvas)
// pitch      : dot spacing in pixels
// rOn, rOff  : dot radii for on/off
// onCol/offCol: colors
// W          : screen width to span
// Returns: row height in pixels (CELL_ROWS * pitch).
static int16_t drawDotRow(int16_t y, int16_t pitch,
                          int16_t rOn, int16_t rOff,
                          uint16_t onCol, uint16_t offCol,
                          const char *text, int16_t W) {
    int16_t halfP = pitch / 2;

    // How many full 5-col cells (plus their gap) fit across W.
    // cells * (CELL_COLS + CELL_GAP) - CELL_GAP <= dotCols
    int16_t dotCols    = W / pitch;
    int16_t totalCells = (dotCols + CELL_GAP) / (CELL_COLS + CELL_GAP);
    if (totalCells < 1) totalCells = 1;
    int16_t cellSpan   = totalCells * (CELL_COLS + CELL_GAP) - CELL_GAP;
    int16_t marginPx   = (W - cellSpan * pitch) / 2;
    int16_t baseX      = marginPx + halfP;   // x of first cell's first col

    // 1) Off-dot grid — only inside cells, gap columns left black.
    if (rOff >= 1) {
        for (int cell = 0; cell < totalCells; cell++) {
            int16_t cellX = baseX + cell * (CELL_COLS + CELL_GAP) * pitch;
            for (int row = 0; row < CELL_ROWS; row++) {
                int16_t dy = y + row * pitch + halfP;
                for (int col = 0; col < CELL_COLS; col++) {
                    int16_t dx = cellX + col * pitch;
                    canvas->fillCircle(dx, dy, rOff, offCol);
                }
            }
        }
    }

    // 2) Light up character glyphs in the leftmost N cells (left-aligned).
    int n = (int)strlen(text);
    if (n > totalCells) n = totalCells;
    int16_t firstTextCell = 0;
    for (int i = 0; i < n; i++) {
        unsigned char uc = (unsigned char)text[i];
        if (uc < 32 || uc > 126) uc = ' ';
        const uint8_t *g = font + uc * 5;
        int16_t cellX = baseX + (firstTextCell + i) * (CELL_COLS + CELL_GAP) * pitch;
        for (int col = 0; col < 5; col++) {
            uint8_t b = pgm_read_byte(g + col);
            for (int row = 0; row < 7; row++) {
                if ((b >> row) & 1) {
                    int16_t dx = cellX + col * pitch;
                    int16_t dy = y + row * pitch + halfP;
                    canvas->fillCircle(dx, dy, rOn, onCol);
                }
            }
        }
    }
    return CELL_ROWS * pitch;
}

// Same dot radius for lit and unlit — only color differs (~30 % of pitch).
static inline int16_t dotR(int16_t pitch) {
    int16_t r = (pitch * 3) / 10;
    return r < 1 ? 1 : r;
}

// ── Full clock face ──────────────────────────────────────────────────────────
static void drawClock(struct tm &ti) {
    canvas->setRotation(s_rot);
    int16_t W = s_rot ? 448 : 368;
    int16_t H = s_rot ? 368 : 448;

    canvas->fillScreen(COL_BG);

    // Build row strings ──────────────────────────────────────────────────────
    char tStr[8];
    snprintf(tStr, sizeof(tStr), "%02d:%02d", ti.tm_hour, ti.tm_min);     // 5 chars
    const char *wkStr = WDAY[ti.tm_wday];                                  // up to 9 chars
    char dStr[20];
    snprintf(dStr, sizeof(dStr), "%02d %s", ti.tm_mday, MON[ti.tm_mon]);   // up to 12 chars
    char wbuf[8];
    strftime(wbuf, sizeof(wbuf), "%V", &ti);
    int week = atoi(wbuf);
    if (week < 1 || week > 53) week = 1;
    char yearStr[8];
    snprintf(yearStr, sizeof(yearStr), "%d", ti.tm_year + 1900);           // 4 chars
    char weekStr[12];
    snprintf(weekStr, sizeof(weekStr), "WEEK %02d", week);                  // 7 chars

    // Single pitch for ALL rows — picked so the longest row fits the screen.
    int longest = (int)strlen(tStr);
    if ((int)strlen(wkStr)   > longest) longest = (int)strlen(wkStr);
    if ((int)strlen(dStr)    > longest) longest = (int)strlen(dStr);
    if ((int)strlen(yearStr) > longest) longest = (int)strlen(yearStr);
    if ((int)strlen(weekStr) > longest) longest = (int)strlen(weekStr);
    int16_t pitch = pitchForCells(longest, W, s_rot ? 7 : 6, 3);
    int16_t r     = dotR(pitch);
    int16_t rowH  = CELL_ROWS * pitch;
    int16_t rowGap = pitch;            // 1 dot row of vertical gap between text rows

    // Default white+yellow combo; overridden by CLOCK_COLOR (uniform).
    uint16_t colTime    = cfg_color_set ? cfg_color : DOT_WHITE;
    uint16_t colWeekday = cfg_color_set ? cfg_color : DOT_YELLOW;
    uint16_t colDate    = cfg_color_set ? cfg_color : DOT_YELLOW;
    uint16_t colYear    = cfg_color_set ? cfg_color : DOT_WHITE;
    uint16_t colWeek    = cfg_color_set ? cfg_color : DOT_WHITE;

    int16_t totalH    = 5 * rowH + 4 * rowGap;
    int16_t topMargin = s_rot ? 22 : 30;
    int16_t y         = topMargin;
    int16_t spare = H - topMargin - totalH - 36;   // 36 px reserved for watermark
    if (spare > 0) y += spare / 2;

    drawDotRow(y, pitch, r, r, colTime,    DOT_OFF, tStr,    W); y += rowH + rowGap;
    drawDotRow(y, pitch, r, r, colWeekday, DOT_OFF, wkStr,   W); y += rowH + rowGap;
    drawDotRow(y, pitch, r, r, colDate,    DOT_OFF, dStr,    W); y += rowH + rowGap;
    drawDotRow(y, pitch, r, r, colYear,    DOT_OFF, yearStr, W); y += rowH + rowGap;
    drawDotRow(y, pitch, r, r, colWeek,    DOT_OFF, weekStr, W);

    canvas->setFont();
    draw_battery_g(canvas, W, H);
    draw_watermark_g(canvas, W, H);
    draw_pill_label(canvas, s_rot, 0, "rot");
    draw_pill_label(canvas, s_rot, 1, "dim");
    canvas->flush();
}

// ── App entry points ─────────────────────────────────────────────────────────
void app_flipclock_setup(Arduino_SH8601 *gfx) {
    s_gfx   = gfx;
    canvas  = g_canvas;
    s_prevMin   = -1;
    s_prevHour  = -1;
    s_prevDay   = -1;
    s_lastCheck = 0;
    s_bootWas   = false;
    s_rot       = 0;
    s_brightIdx = 0;
    memset(cfg_ssid, 0, sizeof(cfg_ssid));
    memset(cfg_pass, 0, sizeof(cfg_pass));
    strncpy(cfg_tz, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof(cfg_tz));
    pinMode(0, INPUT_PULLUP);

    Adafruit_XCA9554 expander;
    if (!expander.begin(0x20)) USBSerial.println("XCA9554 init failed");
    expander.pinMode(1, OUTPUT); expander.digitalWrite(1, LOW);
    expander.pinMode(2, OUTPUT); expander.digitalWrite(2, LOW);
    delay(20);
    expander.digitalWrite(1, HIGH);
    expander.digitalWrite(2, HIGH);

    showStatus("Clock", "Reading config...");

    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    if (SD_MMC.begin("/sdcard", true)) {
        readConfig();
        SD_MMC.end();
    }

    if (cfg_ssid[0][0]) {
        if (!syncTime())
            showStatus("NTP sync failed", "Using system time");
    }

    struct tm ti;
    if (getLocalTime(&ti, 1000)) {
        s_prevMin  = ti.tm_min;
        s_prevHour = ti.tm_hour;
        s_prevDay  = ti.tm_mday;
        drawClock(ti);
    } else {
        showStatus("No time available", "Check WiFi config");
    }
}

void app_flipclock_loop() {
    common_tick();
    uint32_t now = millis();

    if (now - s_lastCheck >= 500) {
        s_lastCheck = now;
        struct tm ti;
        if (getLocalTime(&ti, 50)) {
            if (ti.tm_min != s_prevMin || ti.tm_hour != s_prevHour ||
                ti.tm_mday != s_prevDay) {
                s_prevMin  = ti.tm_min;
                s_prevHour = ti.tm_hour;
                s_prevDay  = ti.tm_mday;
                drawClock(ti);
            }
        }
    }

    bool boot = (digitalRead(0) == LOW);
    if (boot && !s_bootWas) {
        common_activity();
        s_rot ^= 1;
        s_prevMin = -1;
        struct tm ti;
        if (getLocalTime(&ti, 50)) {
            s_prevMin  = ti.tm_min;
            s_prevHour = ti.tm_hour;
            s_prevDay  = ti.tm_mday;
            drawClock(ti);
        }
    }
    s_bootWas = boot;

    if (common_consume_pwr_short()) {
        common_activity();
        s_brightIdx = (s_brightIdx + 1) % (sizeof(BRIGHT_LEVELS));
        s_gfx->setBrightness(BRIGHT_LEVELS[s_brightIdx]);
    }

    delay(50);
}
