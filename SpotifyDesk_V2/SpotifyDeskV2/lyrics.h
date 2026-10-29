// lyrics.h — LRCLIB synced lyrics (streaming parser, hemat RAM)
#pragma once
#include <Arduino.h>

constexpr int MAX_LYRIC_LINES = 160;
constexpr int LYRIC_TEXT_LEN = 72;

struct LyricLine {
  uint32_t time_ms;
  char text[LYRIC_TEXT_LEN];
};

// Dipanggil dari task jaringan. Return jumlah baris (0 = tidak ada).
int lyricsFetch(const char *title, const char *artist, const char *album, int durationSec);

// Dipanggil dari task display. Menyalin lirik aktif ke buffer out.
void lyricsGetCurrent(uint32_t positionMs, int leadMs, char *out, size_t outLen);
uint32_t lyricsGetNextChangeMs(uint32_t positionMs, int leadMs);

void lyricsReset();
bool lyricsLoaded();
