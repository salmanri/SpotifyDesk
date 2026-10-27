// pins.h — Pin map untuk SpotifyDesk
//
// Board aktif  : ESP32 DOIT DevKit V1 (ESP-WROOM-32, Micro-USB)
// Display      : WeAct Studio 2.9" Black/White (SSD1680, 296x128)
//
// Kalau nanti pindah ke ESP32-S3, cukup ganti blok di bawah dengan blok
// ESP32-S3 yang dikomentari. File lain tidak perlu diubah.

#pragma once

// ---------------------------------------------------------------------------
// ESP32 DOIT DevKit V1  (pin SPI bawaan / VSPI)
// Label di board:  D18, D23, D5, TX2, RX2, D4
// JANGAN pakai GPIO 6-11 di ESP32 biasa: dipakai chip flash internal.
// ---------------------------------------------------------------------------
#define EPD_SCK   18  // D18      -> SCL di display
#define EPD_MOSI  23  // D23      -> SDA di display
#define EPD_CS     5  // D5       -> CS
#define EPD_DC    17  // TX2      -> DC
#define EPD_RST   16  // RX2      -> RES
#define EPD_BUSY   4  // D4       -> BUSY

// ---------------------------------------------------------------------------
// ESP32-S3 (sama seperti referensi MusicStation) — untuk nanti
// ---------------------------------------------------------------------------
// #define EPD_SCK   12
// #define EPD_MOSI  11
// #define EPD_CS    10
// #define EPD_DC     9
// #define EPD_RST    8
// #define EPD_BUSY   7
