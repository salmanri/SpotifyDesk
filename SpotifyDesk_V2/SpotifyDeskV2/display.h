// display.h — Driver E-Paper WeAct Studio 2.9" SSD1680 (296x128)
// MusicStation Layout:
// +--------------------+------------------------------------+
// | [Cover Art 96x96]  | [Title - bold]                     | 0..31
// |                    | [Artist - normal]                  |
// |                    | [Album - kecil]                    |
// |                    +------------------------------------+
// |                    | [Lyric - autowrap 2-3 baris]       | 32..103
// |                    +------------------------------------+
// |                    | [===progress bar================]  | 104..127
// +--------------------+------------------------------------+

#pragma once
#include <Arduino.h>

// Ukuran layar (landscape)
constexpr int SCREEN_W = 296;
constexpr int SCREEN_H = 128;

// Layout
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

// Gambar seluruh layar tes (full refresh).
void displayDrawTestScreen();

// Gambar layar standby (full refresh).
void displayDrawIdle();

// Gambar layar lagu lengkap (full refresh).
void displayDrawTrack(const char *title, const char *artist, const char *album,
                      const uint8_t *coverBitmap,
                      uint32_t elapsedSec, uint32_t durationSec, bool playing,
                      const char *lyricText = nullptr);

// Update progress bar saja (partial refresh).
void displayUpdateTransport(uint32_t elapsedSec, uint32_t durationSec, bool playing);

// Update lirik saja (partial refresh).
void displayUpdateLyrics(const char *lyricText);

// Update lirik dan progress bar sekaligus dalam 1 partial refresh
void displayUpdateLyricsAndTransport(const char *lyricText, uint32_t elapsedSec,
                                     uint32_t durationSec, bool playing);
