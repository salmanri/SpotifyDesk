// pins.h — Pin map untuk SpotifyDesk V2
//
// Board aktif  : ESP32 DOIT DevKit V1 (ESP-WROOM-32, Micro-USB)
// Display      : WeAct Studio 2.9" Black/White (SSD1680, 296x128)
//
// Label di board DOIT: D18, D23, D5, TX2, RX2, D4

#pragma once

#define EPD_SCK   18  // D18      -> SCL di display
#define EPD_MOSI  23  // D23      -> SDA di display
#define EPD_CS     5  // D5       -> CS
#define EPD_DC    17  // TX2      -> DC
#define EPD_RST   16  // RX2      -> RES
#define EPD_BUSY   4  // D4       -> BUSY
