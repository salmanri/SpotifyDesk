// display.h — E-paper display driver, layout matching MusicStation V1.
//
// Layout (296x128 landscape):
// +---------------------------------------------------------+
// | title                                                   | 0..15 (14pt Bold)
// | artist                                            album | 16..31 (Artist regular left, Album regular right)
// +--------------------+------------------------------------+
// |                    |                                    |
// |     96x96 art      |       current lyric                | 32..103
// |    (X=4..99)       |       auto-wrapped (up to 4 lines) |
// |                    |                                    |
// +--------------------+------------------------------------+
// |                    | elapsed   play/pause   duration    | 104..119
// |                    | [===progress bar================]  | 120..127
// +--------------------+------------------------------------+

#pragma once
#include <Arduino.h>

// Ukuran layar (landscape)
constexpr int SCREEN_W = 296;
constexpr int SCREEN_H = 128;

// Layout (semua koordinat Y dan H kelipatan 8 agar sesuai batas byte GxEPD2)
constexpr int COVER_X = 4;
constexpr int COVER_Y = 32;        // 4 * 8
constexpr int COVER_SIZE = 96;     // 12 * 8
constexpr int RIGHT_X = 104;       // 13 * 8 (kolom kanan)
constexpr int RIGHT_W = 192;       // 24 * 8 (lebar kolom kanan, 104+192 = 296)

constexpr int LYRIC_AREA_Y = 32;   // 4 * 8
constexpr int LYRIC_AREA_H = 72;   // 9 * 8 (32 + 72 = 104)

constexpr int TRANSPORT_Y = 104;   // 13 * 8
constexpr int TRANSPORT_H = 24;    // 3 * 8 (104 + 24 = 128)

// Inisialisasi SPI + display. Panggil sekali di setup().
void displayBegin();

// Gambar seluruh layar tes (full refresh, layar akan berkedip sekali).
void displayDrawTestScreen();

// Gambar layar standby (full refresh).
void displayDrawIdle();

// Gambar layar lagu lengkap sesuai layout MusicStation V1 (full refresh).
// coverBitmap: pointer ke 1152 bytes (96x96 1-bit), atau nullptr jika belum ada.
void displayDrawTrack(const char *title, const char *artist, const char *album,
                      const uint8_t *coverBitmap,
                      uint32_t elapsedSec, uint32_t durationSec, bool playing,
                      const char *lyricText = nullptr);

// Update area waktu + progress bar saja (partial refresh, tanpa kedip).
void displayUpdateTransport(uint32_t elapsedSec, uint32_t durationSec,
                            bool playing);

// Update area lirik di kolom kanan (partial refresh, tanpa kedip).
void displayUpdateLyrics(const char *lyricText);
