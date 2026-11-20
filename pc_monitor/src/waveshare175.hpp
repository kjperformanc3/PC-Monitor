#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"

#define TFT_BLACK 0x0000

#define SCREEN_WIDTH  466
#define SCREEN_HEIGHT 466

// Waveshare ESP32-S3-Touch-AMOLED-1.75 pin map
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  38
#define LCD_CS    12
#define LCD_RESET 39

#define IIC_SDA   15
#define IIC_SCL   14
#define TP_INT    11
#define TP_RESET  40

#define LV_BUFFER_SIZE (SCREEN_WIDTH * 20)

static volatile bool wsTouchPending = false;

static void IRAM_ATTR wsTouchISR() {
    wsTouchPending = true;
}

class DisplayWrapper {
public:
    Arduino_DataBus *bus = nullptr;
    Arduino_CO5300 *gfx = nullptr;
    TouchDrvCST92xx touch;

    bool touchReady = false;

    DisplayWrapper() {
        bus = new Arduino_ESP32QSPI(
            LCD_CS,
            LCD_SCLK,
            LCD_SDIO0,
            LCD_SDIO1,
            LCD_SDIO2,
            LCD_SDIO3
        );

        // IMPORTANT:
        // Arduino_GFX 1.5.7 constructor is:
        // bus, rst, rotation, ips, width, height, col_offset1, row_offset1, col_offset2, row_offset2
        gfx = new Arduino_CO5300(
            bus,
            LCD_RESET,
            0,
            false,
            SCREEN_WIDTH,
            SCREEN_HEIGHT,
            6,
            0,
            0,
            0
        );
    }

    bool init() {
        Wire.begin(IIC_SDA, IIC_SCL);
        Wire.setClock(400000);

        pinMode(TP_RESET, OUTPUT);
        digitalWrite(TP_RESET, LOW);
        delay(30);
        digitalWrite(TP_RESET, HIGH);
        delay(50);
        delay(1000);

        Wire.begin(IIC_SDA, IIC_SCL);
        Wire.setClock(400000);

        touch.setPins(TP_RESET, TP_INT);
        touchReady = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);

        if (touchReady) {
            touch.sleep();
            touch.reset();
            touch.setMaxCoordinates(SCREEN_WIDTH, SCREEN_HEIGHT);
            touch.setMirrorXY(true, true);

            pinMode(TP_INT, INPUT_PULLUP);
            attachInterrupt(TP_INT, wsTouchISR, FALLING);
        }

        bool displayOk = gfx->begin();
        gfx->setBrightness(200);
        return displayOk;
    }

    void initDMA() {}

    void fillScreen(uint16_t color) {
        if (gfx) gfx->fillScreen(color);
    }

    void setBrightness(uint8_t brightness) {
        if (gfx) gfx->setBrightness(brightness);
    }

    bool getTouch(uint16_t *x, uint16_t *y) {
        if (!touchReady) return false;

        // Do not hammer CST9217 every 33ms.
        // The Waveshare example reads touch only after TP_INT fires.
        if (!wsTouchPending) return false;
        wsTouchPending = false;

        int16_t x_arr[5] = {0};
        int16_t y_arr[5] = {0};

        uint8_t touched = touch.getPoint(
            x_arr,
            y_arr,
            touch.getSupportTouchPoint()
        );

        if (!touched) return false;

        *x = constrain(x_arr[0], 0, SCREEN_WIDTH - 1);
        *y = constrain(y_arr[0], 0, SCREEN_HEIGHT - 1);
        return true;
    }
};

DisplayWrapper tft;