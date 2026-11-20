/*
 * PC Monitor - firmware v0.4.1
 * Waveshare ESP32-S3-Touch-AMOLED-1.75
 *
 * All five screens come from the SquareLine design (swipe LEFT / RIGHT to change):
 *   1) Gaming                  2) Memory and internet     3) Power draw
 *   4) Temperatures            5) Network, SSD and power
 *
 * Slide a finger UP / DOWN = brightness (double tap = full brightness)
 * Turn the display          = auto rotation (motion sensor)
 *
 * The display, touch, rotation, brightness and swipe code comes from the
 * BMW gauge firmware and is kept the same on purpose.
 *
 * The PC script (pc_monitor.py) sends one line per second:
 *   PC,CPU:3,CPUT:35.4,GPU:1,GPUT:49.1,RAM:41,...
 */

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "waveshare175.hpp"
#include "lvgl.h"
#include "ui/ui.h"            // the SquareLine export
#include <SensorQMI8658.hpp>

// ─── Basic settings (same as the gauge) ─────────────────────────────────────
#define SCREEN_W 466
#define SCREEN_H 466
#define BUF_PIXELS (SCREEN_W * SCREEN_H / 4)   // quarter-screen double buffer
#define LVGL_TICK_MS 2

#define MADCTL_0   0x00
#define MADCTL_90  0xA0
#define MADCTL_180 0xC0
#define MADCTL_270 0x60

#define ROT_CHECK_MS    100
#define ROT_CONFIRM_MS  900
#define ROT_COOLDOWN_MS 1200
#define ROT_THRESHOLD   0.78f
#define STUCK_MS        180

// ─── Settings for this project (safe to change) ─────────────────────────────
#define SCREEN_COUNT   5
#define ACCENT_COLOR   0xFF4500   // orange, same as the CPU colour in the design
#define DOT_SIZE       8
#define DOT_GAP        6
#define DOT_Y          (SCREEN_H - 34)

#define VALUE_HOLD_MS  6000       // keep showing the last value this long when new data stops
                                  // (after that: "--")
#define DISPLAY_FPS    30         // screen redraw rate (lower it to 25 if it ever stutters)
#define ARC_STEPS      1000       // arcs and bars count 0..1000 instead of 0..100 = smoother
#define ARC_ANIM_MS    600        // time an arc or bar takes to glide to a new value
#define DISPLAY_DELAY_MS 200      // numbers appear this long after the arc starts moving

#define CPU_POWER_MAX_W 150.0f    // CPU power arc is full at this many watts
#define GPU_POWER_MAX_W 250.0f    // GPU power arc is full at this many watts

// ─── The values we receive (names must match pc_monitor.py) ─────────────────
enum Key {
    K_CPU, K_CPUT, K_CCD1, K_CCD2, K_CPUS, K_PCPU,
    K_GPU, K_GPUT, K_GPUM, K_PGPU,
    K_RAM, K_DIMM1, K_DIMM2,
    K_SSDT, K_DSKU, K_DSKF, K_DSKR, K_DSKW,
    K_NETD, K_NETU,
    K_CHIP, K_MOBO,
    K_PTOT,
    K_RAMU, K_RAMT, K_VRMU, K_VRMT, K_VRAM,
    K_PKCPU, K_PKGPU, K_DSKT,
    K_COUNT
};
static const char *KEY_NAMES[K_COUNT] = {
    "CPU", "CPUT", "CCD1", "CCD2", "CPUS", "PCPU",
    "GPU", "GPUT", "GPUM", "PGPU",
    "RAM", "DIMM1", "DIMM2",
    "SSDT", "DSKU", "DSKF", "DSKR", "DSKW",
    "NETD", "NETU",
    "CHIP", "MOBO",
    "PTOT",
    "RAMU", "RAMT", "VRMU", "VRMT", "VRAM",
    "PKCPU", "PKGPU", "DSKT"
};

// Latest data from the PC (updated the moment a line arrives):
static float    g_val[K_COUNT];      // last received value of each key
static uint32_t g_seenMs[K_COUNT];   // when each key was last received (0 = never)
static uint32_t g_lastRxMs = 0;      // when the last good line arrived (0 = never)
static uint32_t g_rxLines = 0, g_rxBad = 0;

// What the numbers on screen show: a copy of the above taken DISPLAY_DELAY_MS
// after the data arrived, so the numbers line up with the gliding arcs.
static float    d_val[K_COUNT];
static uint32_t d_ts[K_COUNT];       // data time of each shown value (0 = nothing to show yet)
static bool     g_commitPending = false;
static uint32_t g_commitAtMs = 0;

// A value is shown while it is younger than VALUE_HOLD_MS: short gaps in the data
// (for example while the PC is busy starting a big program) are not visible.
static bool shown(int k, uint32_t now) { return d_ts[k] != 0 && (now - d_ts[k]) < VALUE_HOLD_MS; }
static bool fresh(int k, uint32_t now) { return g_seenMs[k] != 0 && (now - g_seenMs[k]) < VALUE_HOLD_MS; }

static void commitDisplay() {
    for (int k = 0; k < K_COUNT; k++) { d_val[k] = g_val[k]; d_ts[k] = g_seenMs[k]; }
}

// ─── Display / touch / rotation state (same as the gauge) ───────────────────
#if LV_TICK_CUSTOM == 0
static esp_timer_handle_t lvglTick = nullptr;
static void lvglTickCB(void *) { lv_tick_inc(LVGL_TICK_MS); }
static void startLvglTick() {
    if (lvglTick) return;
    esp_timer_create_args_t a{}; a.callback = lvglTickCB; a.name = "lv";
    esp_timer_create(&a, &lvglTick);
    esp_timer_start_periodic(lvglTick, LVGL_TICK_MS * 1000);
}
#endif

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_disp_t *g_disp = nullptr;

static bool     g_touchPressed = false;
static uint16_t g_touchX = 0, g_touchY = 0;
static bool     isTouching = false;
static uint16_t touchStartX = 0, touchStartY = 0, touchLastX = 0, touchLastY = 0;
static uint32_t touchStartMs = 0, lastTapMs = 0;
static uint8_t  g_tapCount = 0;
static int      brightness = 255;

static uint8_t  g_rotation = 0;
static SensorQMI8658 qmi;
static IMUdata  acc;
static bool     qmiReady = false;
static uint8_t  pendRot = 255;
static uint32_t pendRotMs = 0, lastRotChgMs = 0, lastIMUMs = 0;

static lv_obj_t *g_screens[SCREEN_COUNT];
static int       currentScreen = 0;

// ─── Receiving data from the PC ─────────────────────────────────────────────
// Takes one complete line (without the end-of-line character) and stores the
// values. Returns false if the line is not one of ours.
static bool parseLine(char *line) {
    if (strncmp(line, "PC,", 3) != 0) return false;

    uint32_t now = millis();
    if (now == 0) now = 1;

    char *save = nullptr;
    char *token = strtok_r(line + 3, ",", &save);
    while (token) {
        char *colon = strchr(token, ':');
        if (colon) {
            *colon = 0;
            for (int k = 0; k < K_COUNT; k++) {
                if (strcmp(token, KEY_NAMES[k]) == 0) {
                    g_val[k] = (float)atof(colon + 1);
                    g_seenMs[k] = now;       // a key missing from a later line simply keeps its old value
                    break;
                }
            }
        }
        token = strtok_r(nullptr, ",", &save);
    }

    g_lastRxMs = now;
    if (!g_commitPending) { g_commitPending = true; g_commitAtMs = now + DISPLAY_DELAY_MS; }
    return true;
}

// Collects bytes from USB until a line is complete, then parses it.
static void pollSerial() {
    static char buf[768];
    static size_t len = 0;
    static bool overflow = false;
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n') {
            if (!overflow && len > 0) {
                buf[len] = 0;
                if (parseLine(buf)) g_rxLines++; else g_rxBad++;
            }
            len = 0;
            overflow = false;
        } else if (c != '\r') {
            if (len < sizeof(buf) - 1) buf[len++] = (char)c;
            else overflow = true;
        }
    }
}

// Every 5 seconds, while data is flowing, tell the PC script we are alive.
static void heartbeat(uint32_t now) {
    static uint32_t last = 0;
    if (now - last < 5000) return;
    last = now;
    if (g_lastRxMs == 0 || now - g_lastRxMs > VALUE_HOLD_MS) return;
    Serial.printf("OK lines=%lu bad=%lu\n", (unsigned long)g_rxLines, (unsigned long)g_rxBad);
}

// ─── Display driver glue (same as the gauge) ────────────────────────────────
static void my_rounder(lv_disp_drv_t *, lv_area_t *a) {
    if (a->x1 & 1) a->x1--;
    if (!(a->x2 & 1)) a->x2++;
    if (a->y1 & 1) a->y1--;
    if (!(a->y2 & 1)) a->y2++;
}
static void my_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c) {
    uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1; int32_t x = a->x1, y = a->y1;
    if (g_rotation == 1 || g_rotation == 3) x -= 6;
    tft.gfx->draw16bitRGBBitmap(x, y, (uint16_t *)&c->full, w, h);
    lv_disp_flush_ready(d);
}
static void my_touch_read(lv_indev_drv_t *, lv_indev_data_t *d) {
    if (g_touchPressed && g_touchX < SCREEN_W && g_touchY < SCREEN_H) {
        d->state = LV_INDEV_STATE_PR; d->point.x = g_touchX; d->point.y = g_touchY;
    } else d->state = LV_INDEV_STATE_REL;
}

// ─── Touch reading (same as the gauge) ──────────────────────────────────────
static void pollTouch() {
    static uint16_t px = 0, py = 0;
    static uint32_t lastMoveMs = 0;
    uint16_t tx = 0, ty = 0; bool raw = tft.getTouch(&tx, &ty);
    uint32_t now = millis(); bool pressed = raw;
    if (raw && tx < SCREEN_W && ty < SCREEN_H) {
        if (tx != px || ty != py) { px = tx; py = ty; lastMoveMs = now; }
        if (now - lastMoveMs > STUCK_MS) pressed = false;
    } else {
        px = 0; py = 0; lastMoveMs = now; pressed = false;
    }
    g_touchPressed = pressed;
    if (pressed) {
        switch (g_rotation) {
            case 0: g_touchX = tx;                g_touchY = ty;                break;
            case 1: g_touchX = SCREEN_H - 1 - ty; g_touchY = tx;                break;
            case 2: g_touchX = SCREEN_W - 1 - tx; g_touchY = SCREEN_H - 1 - ty; break;
            case 3: g_touchX = ty;                g_touchY = SCREEN_W - 1 - tx; break;
        }
    }
}

// ─── Auto rotation (same as the gauge) ──────────────────────────────────────
static void applyRotation(uint8_t r) {
    r %= 4; g_rotation = r; uint8_t madctl = MADCTL_0;
    switch (g_rotation) {
        case 1: madctl = MADCTL_90; break;
        case 2: madctl = MADCTL_180; break;
        case 3: madctl = MADCTL_270; break;
    }
    tft.bus->beginWrite(); tft.bus->writeC8D8(0x36, madctl); tft.bus->endWrite();
    lv_obj_invalidate(lv_scr_act());
}
static void updateIMU() {
    if (!qmiReady) return;
    uint32_t now = millis();
    if (now - lastIMUMs < ROT_CHECK_MS) return;
    lastIMUMs = now;
    if (!qmi.getDataReady() || !qmi.getAccelerometer(acc.x, acc.y, acc.z)) return;
    float ax = acc.x, ay = acc.y; uint8_t w = 255;
    if (fabsf(ax) >= fabsf(ay)) { if (ax > ROT_THRESHOLD) w = 0; else if (ax < -ROT_THRESHOLD) w = 2; }
    else { if (ay > ROT_THRESHOLD) w = 3; else if (ay < -ROT_THRESHOLD) w = 1; }
    if (w == 255) { pendRot = 255; pendRotMs = 0; return; }
    if (w != pendRot) { pendRot = w; pendRotMs = now; return; }
    if (w == g_rotation || now - pendRotMs < ROT_CONFIRM_MS || now - lastRotChgMs < ROT_COOLDOWN_MS) return;
    applyRotation(w); lastRotChgMs = now;
}

// ─── Gestures: swipe left/right = screen, slide up/down = brightness ────────
static void switchScreen(int idx, bool forward) {
    currentScreen = idx;
    lv_scr_load_anim(g_screens[idx],
                     forward ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                     180, 0, false);
}

static void handleGestures() {
    bool pressed = g_touchPressed; uint32_t now = millis();
    uint16_t tx = g_touchX, ty = g_touchY;
    if (pressed) { touchLastX = tx; touchLastY = ty; }

    // Finger just touched the screen
    if (pressed && !isTouching) {
        isTouching = true; touchStartX = tx; touchStartY = ty; touchStartMs = now;
        if (now - lastTapMs < 500) g_tapCount++; else g_tapCount = 1;
        if (g_tapCount == 2) { brightness = 255; tft.setBrightness(255); }   // double tap = full brightness
        lastTapMs = now;
    }

    // Finger is moving: mostly vertical = change brightness
    if (pressed && isTouching) {
        int curDx = (int)g_touchX - (int)touchStartX;
        int dy = (int)touchStartY - (int)g_touchY;
        if (abs(dy) > 12 && abs(dy) > abs(curDx) * 2.5f) {
            brightness = constrain(brightness + (dy / 3), 0, 255);
            tft.setBrightness((uint8_t)brightness);
            touchStartY = g_touchY;
        }
    }

    // Finger lifted: was it a quick left/right swipe?
    if (!pressed && isTouching) {
        isTouching = false; uint32_t dur = now - touchStartMs;
        int dx = (int)touchLastX - (int)touchStartX, dy = (int)touchLastY - (int)touchStartY;
        if (dur < 500 && abs(dx) > 45 && abs(dx) > abs(dy) * 1.2f) {
            int nxt = currentScreen + (dx < 0 ? 1 : -1);
            if (nxt >= 0 && nxt < SCREEN_COUNT) switchScreen(nxt, dx < 0);
        }
    }
}

// ─── Page dots (added in code to every screen) ──────────────────────────────
static void addDots(lv_obj_t *scr, int index) {
    int totalW = SCREEN_COUNT * DOT_SIZE + (SCREEN_COUNT - 1) * DOT_GAP;
    int startX = (SCREEN_W - totalW) / 2;
    for (int d = 0; d < SCREEN_COUNT; d++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(dot, DOT_SIZE / 2, 0);
        bool cur = (d == index);
        lv_obj_set_style_bg_opa(dot, cur ? 230 : 90, 0);
        lv_obj_set_style_bg_color(dot, cur ? lv_color_hex(ACCENT_COLOR) : lv_color_hex(0x666666), 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(dot, startX + d * (DOT_SIZE + DOT_GAP), DOT_Y);
    }
}

// ─── Smooth arcs and bars ───────────────────────────────────────────────────
// An arc (or bar) glides from where it is to each new value, instead of jumping.
struct Glide { lv_obj_t *obj; int32_t target; };
static Glide g_arcCpu, g_arcGpu;          // screen 1: usage
static Glide g_arcPwrCpu, g_arcPwrGpu;    // screen 5: power
static Glide g_barSsd;                    // screen 5: SSD used

static void arcAnimExec(void *o, int32_t v) { lv_arc_set_value((lv_obj_t *)o, (int16_t)v); }
static void barAnimExec(void *o, int32_t v) { lv_bar_set_value((lv_obj_t *)o, v, LV_ANIM_OFF); }

static void setupArc(Glide &g, lv_obj_t *arc) {
    g.obj = arc; g.target = 0;
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);   // touching the arc must not move it
    lv_arc_set_range(arc, 0, ARC_STEPS);             // finer steps than the design's 0..100
    lv_arc_set_value(arc, 0);
}
static void setupBar(Glide &g, lv_obj_t *bar) {
    g.obj = bar; g.target = 0;
    lv_bar_set_range(bar, 0, ARC_STEPS);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
}

static void glideStart(Glide &g, float percent, lv_anim_exec_xcb_t exec, int32_t current) {
    if (!g.obj) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int32_t target = (int32_t)(percent * (ARC_STEPS / 100.0f) + 0.5f);
    if (target == g.target) return;                  // nothing new: let any running glide finish
    g.target = target;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g.obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, current, target);
    lv_anim_set_time(&a, ARC_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);  // quick start, gentle landing
    lv_anim_start(&a);                               // replaces a running glide of the same object
}
static void arcTo(Glide &g, float percent) { if (g.obj) glideStart(g, percent, arcAnimExec, lv_arc_get_value(g.obj)); }
static void barTo(Glide &g, float percent) { if (g.obj) glideStart(g, percent, barAnimExec, lv_bar_get_value(g.obj)); }

static void buildUI() {
    ui_init();                                   // creates the five SquareLine screens
    g_screens[0] = ui_Screen1;
    g_screens[1] = ui_Screen2;
    g_screens[2] = ui_Screen3;
    g_screens[3] = ui_Screen4;
    g_screens[4] = ui_Screen5;

    setupArc(g_arcGpu, ui_Arc1);                 // screen 1: outer green arc = GPU usage
    setupArc(g_arcCpu, ui_Arc2);                 //           inner orange arc = CPU usage
    setupArc(g_arcPwrCpu, ui_cpupowerarc);       // screen 5: CPU power arc
    setupArc(g_arcPwrGpu, ui_gpupowerarc);       //           GPU power arc
    setupBar(g_barSsd, ui_Bar1);                 //           SSD used bar

    lv_obj_add_flag(ui_MB3, LV_OBJ_FLAG_HIDDEN); // stray duplicate "MB" label in the design

    for (int i = 0; i < SCREEN_COUNT; i++) addDots(g_screens[i], i);
}

// ─── Showing the values ─────────────────────────────────────────────────────
// Only touches a label when its text really changed (less redrawing).
static void setTextIfChanged(lv_obj_t *o, const char *t) {
    if (!o) return;
    const char *cur = lv_label_get_text(o);
    if (!cur || strcmp(cur, t) != 0) lv_label_set_text(o, t);
}

static void setNumber(lv_obj_t *o, bool have, float v, int decimals) {
    char b[24];
    if (have) snprintf(b, sizeof(b), "%.*f", decimals, v); else snprintf(b, sizeof(b), "--");
    setTextIfChanged(o, b);
}
static void setTemp(lv_obj_t *o, bool have, float v) {                    // "34.3°C"
    char b[24];
    if (have) snprintf(b, sizeof(b), "%.1f\xC2\xB0""C", v); else snprintf(b, sizeof(b), "--");
    setTextIfChanged(o, b);
}
static void setPercent(lv_obj_t *o, bool have, float v) {                 // "63%"
    char b[24];
    if (have) snprintf(b, sizeof(b), "%.0f%%", v); else snprintf(b, sizeof(b), "--");
    setTextIfChanged(o, b);
}
static void setGB(lv_obj_t *o, bool have, float v) {                      // "20.2 GB"
    char b[24];
    if (have) snprintf(b, sizeof(b), "%.1f GB", v); else snprintf(b, sizeof(b), "--");
    setTextIfChanged(o, b);
}
static void setWatts(lv_obj_t *o, bool have, float v, int decimals, bool tilde) {   // "86.4W", "~303 W"
    char b[24];
    if (have) snprintf(b, sizeof(b), tilde ? "~%.*f W" : "%.*fW", decimals, v); else snprintf(b, sizeof(b), "--");
    setTextIfChanged(o, b);
}

// Speed given in KB/s. Shows the number and its unit separately, because the design has two labels.
// perSecond = true gives "KB/s", "MB/s", "GB/s"; false gives "KB", "MB", "GB".
static void setSpeed(lv_obj_t *num, lv_obj_t *unit, bool have, float kb, bool perSecond) {
    char b[24];
    if (!have) { setTextIfChanged(num, "--"); return; }       // the unit stays as it was
    float v; const char *u;
    if (kb >= 1024.0f * 1024.0f)  { v = kb / (1024.0f * 1024.0f); u = perSecond ? "GB/s" : "GB"; }
    else if (kb >= 1024.0f)       { v = kb / 1024.0f;             u = perSecond ? "MB/s" : "MB"; }
    else                          { v = kb;                       u = perSecond ? "KB/s" : "KB"; }
    if (v >= 100.0f) snprintf(b, sizeof(b), "%.0f", v); else snprintf(b, sizeof(b), "%.1f", v);
    setTextIfChanged(num, b);
    if (unit) setTextIfChanged(unit, u);
}

static void updateUI() {
    uint32_t now = millis();
    #define HAVE(k) shown(k, now)

    // --- Screen 1: Gaming ---
    setNumber(ui_Label3, HAVE(K_CPU),  d_val[K_CPU],  0);       // CPU usage number
    setNumber(ui_Label6, HAVE(K_GPU),  d_val[K_GPU],  0);       // GPU usage number
    setNumber(ui_Label4, HAVE(K_CPUT), d_val[K_CPUT], 1);       // CPU temperature
    setNumber(ui_Label5, HAVE(K_GPUT), d_val[K_GPUT], 1);       // GPU temperature
    arcTo(g_arcCpu, fresh(K_CPU, now) ? g_val[K_CPU] : 0.0f);   // arcs glide to the live values (or back to 0)
    arcTo(g_arcGpu, fresh(K_GPU, now) ? g_val[K_GPU] : 0.0f);

    // --- Screen 2: Memory and internet ---
    char b[24];
    if (HAVE(K_NETD)) {
        float kb = d_val[K_NETD];
        if (kb >= 1024.0f * 1024.0f) snprintf(b, sizeof(b), "%.1f GB", kb / (1024.0f * 1024.0f));
        else if (kb >= 1024.0f)      snprintf(b, sizeof(b), "%.1f MB", kb / 1024.0f);
        else                         snprintf(b, sizeof(b), "%.0f KB", kb);
    } else snprintf(b, sizeof(b), "--");
    setTextIfChanged(ui_Label13, b);                            // download
    if (HAVE(K_NETU)) {
        float kb = d_val[K_NETU];
        if (kb >= 1024.0f * 1024.0f) snprintf(b, sizeof(b), "%.1f GB", kb / (1024.0f * 1024.0f));
        else if (kb >= 1024.0f)      snprintf(b, sizeof(b), "%.1f MB", kb / 1024.0f);
        else                         snprintf(b, sizeof(b), "%.0f KB", kb);
    } else snprintf(b, sizeof(b), "--");
    setTextIfChanged(ui_Label14, b);                            // upload
    setGB(ui_Label15, HAVE(K_RAMU), d_val[K_RAMU]);             // RAM used
    setPercent(ui_Label17, HAVE(K_RAM), d_val[K_RAM]);          // RAM %
    setGB(ui_Label16, HAVE(K_VRMU), d_val[K_VRMU]);             // VRAM used
    setPercent(ui_Label18, HAVE(K_VRAM), d_val[K_VRAM]);        // VRAM %

    // --- Screen 3: Power draw ---
    setNumber(ui_Label20, HAVE(K_PCPU), d_val[K_PCPU], 0);      // CPU watts
    setNumber(ui_Label21, HAVE(K_PGPU), d_val[K_PGPU], 0);      // GPU watts
    setWatts(ui_Label19, HAVE(K_PTOT), d_val[K_PTOT], 0, true); // total

    // --- Screen 4: Temperatures ---
    setTemp(ui_cputemp,      HAVE(K_CPUT),  d_val[K_CPUT]);
    setTemp(ui_ccd1temp1,    HAVE(K_CCD1),  d_val[K_CCD1]);
    setTemp(ui_ccd2temp2,    HAVE(K_CCD2),  d_val[K_CCD2]);
    setTemp(ui_ccd2temp1,    HAVE(K_CPUS),  d_val[K_CPUS]);     // "Socket" row
    setTemp(ui_gpucoretemp,  HAVE(K_GPUT),  d_val[K_GPUT]);
    setTemp(ui_gpumemtemp,   HAVE(K_GPUM),  d_val[K_GPUM]);
    setTemp(ui_ram1temp,     HAVE(K_DIMM1), d_val[K_DIMM1]);
    setTemp(ui_ram2temp,     HAVE(K_DIMM2), d_val[K_DIMM2]);
    setTemp(ui_chipsettemp,  HAVE(K_CHIP),  d_val[K_CHIP]);
    setTemp(ui_MBoardtemp,   HAVE(K_MOBO),  d_val[K_MOBO]);
    setTemp(ui_ssdtemp,      HAVE(K_SSDT),  d_val[K_SSDT]);
    setTemp(ui_PEAKCPU,      HAVE(K_PKCPU), d_val[K_PKCPU]);
    setTemp(ui_PEAKGPU,      HAVE(K_PKGPU), d_val[K_PKGPU]);

    // --- Screen 5: Network, SSD and power ---
    setSpeed(ui_downloadspeed, ui_MB1,      HAVE(K_NETD), d_val[K_NETD], false);
    setSpeed(ui_uploadspeed,   ui_MB2,      HAVE(K_NETU), d_val[K_NETU], false);
    setSpeed(ui_readspeed,     ui_readunit,  HAVE(K_DSKR), d_val[K_DSKR], true);
    setSpeed(ui_writespeed,    ui_writeunit, HAVE(K_DSKW), d_val[K_DSKW], true);

    setPercent(ui_ssdpercentage, HAVE(K_DSKU), d_val[K_DSKU]);
    bool haveSize = HAVE(K_DSKT) && HAVE(K_DSKF);
    setNumber(ui_usedmemoryspace, haveSize, d_val[K_DSKT] - d_val[K_DSKF], 0);   // used GB = size - free
    setNumber(ui_totalmemory,     HAVE(K_DSKT), d_val[K_DSKT], 0);
    barTo(g_barSsd, fresh(K_DSKU, now) ? g_val[K_DSKU] : 0.0f);

    setWatts(ui_cpupower, HAVE(K_PCPU), d_val[K_PCPU], 1, false);
    setWatts(ui_gpupower, HAVE(K_PGPU), d_val[K_PGPU], 1, false);
    setWatts(ui_totalpower, HAVE(K_PTOT), d_val[K_PTOT], 0, false);
    arcTo(g_arcPwrCpu, fresh(K_PCPU, now) ? g_val[K_PCPU] * 100.0f / CPU_POWER_MAX_W : 0.0f);
    arcTo(g_arcPwrGpu, fresh(K_PGPU, now) ? g_val[K_PGPU] * 100.0f / GPU_POWER_MAX_W : 0.0f);
    #undef HAVE
}

// ─── Start-up ───────────────────────────────────────────────────────────────
void setup() {
    // The USB receive buffer is only 256 bytes by default. Our line from the PC is longer than that,
    // so the end of it was lost whenever the loop was busy drawing. 2048 bytes is plenty.
    Serial.setRxBufferSize(2048);
    Serial.begin(115200);
    delay(200);

    tft.init();
    tft.setBrightness(brightness);
    tft.fillScreen(TFT_BLACK);

    qmiReady = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (qmiReady) {
        qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_1000Hz,
                                SensorQMI8658::LPF_MODE_0);
        qmi.enableAccelerometer();
    }

    lv_init();
    buf1 = (lv_color_t *)heap_caps_malloc(BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        if (buf1) free(buf1);
        if (buf2) free(buf2);
        buf1 = (lv_color_t *)heap_caps_malloc(BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        buf2 = (lv_color_t *)heap_caps_malloc(BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        if (!buf1 || !buf2) while (1) delay(100);
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, BUF_PIXELS);

    static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
    dd.hor_res = SCREEN_W; dd.ver_res = SCREEN_H;
    dd.flush_cb = my_flush; dd.rounder_cb = my_rounder; dd.draw_buf = &draw_buf; dd.full_refresh = 1;
    g_disp = lv_disp_drv_register(&dd);
    lv_timer_set_period(_lv_disp_get_refr_timer(g_disp), 1000 / DISPLAY_FPS);   // redraw at about 30 fps

    static lv_indev_drv_t id; lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER; id.read_cb = my_touch_read; lv_indev_drv_register(&id);

#if LV_TICK_CUSTOM == 0
    startLvglTick();
#endif

    applyRotation(0);
    buildUI();
    updateUI();
    lv_disp_load_scr(g_screens[0]);
}

void loop() {
    uint32_t now = millis();

    pollSerial();
    static uint32_t seenLines = 0;
    if (g_rxLines != seenLines) { seenLines = g_rxLines; updateUI(); }   // new data: start the arcs at once
    updateIMU();

    static uint32_t lastTouch = 0;
    if (now - lastTouch >= 33) { lastTouch = now; pollTouch(); handleGestures(); }

    static uint32_t lastLvgl = 0;
    if (now - lastLvgl >= 10) { lastLvgl = now; lv_timer_handler(); }

    if (g_commitPending && (int32_t)(now - g_commitAtMs) >= 0) {   // time to show the new numbers
        g_commitPending = false;
        commitDisplay();
        updateUI();
    }

    static uint32_t lastUI = 0;
    if (now - lastUI >= 250) { lastUI = now; updateUI(); }

    static uint32_t lastBL = 0;
    if (now - lastBL >= 1000) { lastBL = now; tft.setBrightness((uint8_t)brightness); }

    heartbeat(now);
}
