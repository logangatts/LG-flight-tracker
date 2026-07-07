// LilyGO T-Encoder Pro pin assignments.
// Facts restated from the board schematic / vendor README (pin numbers are
// not copyrightable; no vendor code is used).
#pragma once

// 1.2" round AMOLED, 390x390, QSPI bus.
// Two panel revisions exist; detected at runtime via the touch controller:
//   - CHSC5816 touch @ I2C 0x2E  -> SH8601 display driver (original panel)
//   - CST816  touch @ I2C 0x15  -> CO5300 display driver (2025+ panel)
#define PIN_LCD_CS 10
#define PIN_LCD_SCLK 12
#define PIN_LCD_SDIO0 11
#define PIN_LCD_SDIO1 13
#define PIN_LCD_SDIO2 7
#define PIN_LCD_SDIO3 14
#define PIN_LCD_RST 4
#define PIN_LCD_EN 3  // AMOLED VCI enable — drive HIGH before panel init

#define PIN_I2C_SDA 5
#define PIN_I2C_SCL 6
#define PIN_TOUCH_INT 9
#define PIN_TOUCH_RST 8

#define PIN_ENC_A 1
#define PIN_ENC_B 2
#define PIN_ENC_KEY 0  // shared with BOOT strap; active low, needs pullup

#define PIN_BUZZER 17

#define TOUCH_ADDR_CHSC5816 0x2E
#define TOUCH_ADDR_CST816 0x15
