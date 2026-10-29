// SpotifyDesk V2 — Standalone Spotify Web API E-Paper Display
//
// ARSITEKTUR DUAL-CORE:
//   Core 0 (netTask)  : Wi-Fi, Spotify API (keep-alive), download cover, lirik.
//   Core 1 (loop)     : layar e-paper — progress bar tiap 1 detik & lirik,
//                       TIDAK PERNAH menunggu jaringan. Hasilnya semulus V1.
//
// Board   : DOIT ESP32 DevKit V1
// Display : WeAct Studio 2.9" SSD1680 (296x128)
// Library : GxEPD2, Adafruit GFX, U8g2_for_Adafruit_GFX, ArduinoJson 7, JPEGDEC

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#include "pins.h"
#include "display.h"
#include "spotify.h"
#include "lyrics.h"
#include "artwork.h"

#ifndef SPOTIFY_POLL_MS
#define SPOTIFY_POLL_MS 3000
#endif
#ifndef LYRIC_LEAD_MS
#define LYRIC_LEAD_MS 0
#endif

constexpr size_t COVER_BYTES = (COVER_SIZE * COVER_SIZE) / 8;

// ---------------------------------------------------------------------------
// State bersama (dilindungi mutex)
// ---------------------------------------------------------------------------
struct SharedState {
  bool active;
  bool playing;
  char title[96];
  char artist[96];
  char album[96];
  uint32_t durationMs;
  uint32_t baseMs;           // posisi lagu pada baseMillis
  unsigned long baseMillis;
  bool hasCover;
  uint32_t screenVersion;    // naik saat layar penuh harus digambar ulang
};

static SharedState shared = {};
static uint8_t sharedCover[COVER_BYTES];
static SemaphoreHandle_t stateMutex;

static void lockState()   { xSemaphoreTake(stateMutex, portMAX_DELAY); }
static void unlockState() { xSemaphoreGive(stateMutex); }

// ---------------------------------------------------------------------------
// Task jaringan (Core 0)
// ---------------------------------------------------------------------------
static void netTask(void *)
{
  static uint8_t cover[COVER_BYTES];
  char currentId[40] = "";
  int nothingCount = 0;
  int driftHits = 0;
  SpotifyTrack t;

  for (;;) {
    unsigned long loopStart = millis();
    spotifyEnsureWiFi();

    PollResult r = spotifyPoll(t);

    if (r == POLL_OK) {
      nothingCount = 0;

      if (strcmp(t.id, currentId) != 0) {
        // ===== Lagu baru =====
        strlcpy(currentId, t.id, sizeof(currentId));
        driftHits = 0;
        Serial.printf("\n[Track] %s — %s\n", t.title, t.artist);
        Serial.printf("[Mem] heap %u, blok max %u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

        lyricsReset();
        spotifyDisconnect();                       // bebaskan ~40KB RAM TLS untuk cover

        bool hasCover = artworkDownload(t.artUrl, cover);

        lockState();
        shared.active = true;
        shared.playing = t.playing;
        strlcpy(shared.title, t.title, sizeof(shared.title));
        strlcpy(shared.artist, t.artist, sizeof(shared.artist));
        strlcpy(shared.album, t.album, sizeof(shared.album));
        shared.durationMs = t.durationMs;
        shared.baseMs = t.progressMs;
        shared.baseMillis = t.respMillis;          // waktu download cover ikut terhitung
        shared.hasCover = hasCover;
        if (hasCover) memcpy(sharedCover, cover, COVER_BYTES);
        shared.screenVersion++;
        unlockState();

        // Ambil lirik setelah layar mulai digambar (durasi dibulatkan ke detik terdekat)
        int durSec = (int)((t.durationMs + 500) / 1000);
        lyricsFetch(t.title, t.artist, t.album, durSec);
        Serial.printf("[Mem] heap %u\n", ESP.getFreeHeap());

        // Re-sync segera setelah cover & lirik selesai diunduh agar posisi waktu
        // bebas dari jeda buffering awal transisi lagu di Spotify
        SpotifyTrack syncT;
        if (spotifyPoll(syncT) == POLL_OK && strcmp(syncT.id, currentId) == 0) {
          lockState();
          shared.playing = syncT.playing;
          shared.baseMs = syncT.progressMs;
          shared.baseMillis = syncT.respMillis;
          unlockState();
          Serial.printf("[Sync] Initial lock %u ms\n", syncT.progressMs);
        }
      } else {
        // ===== Lagu sama: jaga timer lokal tetap mulus 1 detik tanpa loncat =====
        lockState();
        uint32_t local = shared.baseMs;
        if (shared.playing) local += (uint32_t)(t.respMillis - shared.baseMillis);
        int32_t drift = (int32_t)t.progressMs - (int32_t)local;

        if (t.playing != shared.playing || abs(drift) >= 1500) {
          // Perubahan play/pause atau user melakukan seek -> snap langsung
          shared.playing = t.playing;
          shared.baseMs = t.progressMs;
          shared.baseMillis = t.respMillis;
          driftHits = 0;
          if (abs(drift) >= 1500) Serial.printf("[Sync] Seek/snap %d ms\n", (int)drift);
        } else if (abs(drift) >= 300) {
          // Hanya koreksi jika drift konsisten ke arah yang sama selama 3x poll berturut-turut
          int dir = (drift > 0) ? 1 : -1;
          if ((driftHits > 0 && dir > 0) || (driftHits < 0 && dir < 0)) driftHits += dir;
          else driftHits = dir;

          if (abs(driftHits) >= 3) {
            shared.baseMs = (uint32_t)((int32_t)local + drift / 2);
            shared.baseMillis = t.respMillis;
            driftHits = 0;
            Serial.printf("[Sync] Smooth drift %d ms\n", (int)drift);
          }
        } else {
          driftHits = 0;
        }
        unlockState();
      }
    } else if (r == POLL_NOTHING) {
      if (++nothingCount >= 3) {
        lockState();
        if (shared.active) {
          shared.active = false;
          shared.screenVersion++;
          Serial.println("[Track] Tidak ada musik -> standby");
        }
        unlockState();
        currentId[0] = '\0';
      }
    }

    uint32_t wait = SPOTIFY_POLL_MS + spotifyBackoffMs();
    if (r == POLL_ERROR) wait = max(wait, (uint32_t)3000);
    unsigned long spent = millis() - loopStart;
    if (spent < wait) vTaskDelay(pdMS_TO_TICKS(wait - spent));
    else vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== SpotifyDesk V2 (dual-core) ===");

  stateMutex = xSemaphoreCreateMutex();
  lyricsReset();

  displayBegin();
  displayDrawIdle();

  spotifyBegin();

  // Stack 16 KB: TLS handshake (mbedTLS) butuh stack besar
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Loop = task display (Core 1)
// ---------------------------------------------------------------------------
void loop()
{
  static uint32_t seenVersion = 0;
  static uint32_t lastSec = UINT32_MAX;
  static bool lastPlaying = false;
  static char lastLyric[LYRIC_TEXT_LEN] = "";
  static uint8_t cover[COVER_BYTES];

  SharedState s;
  bool newScreen;
  lockState();
  s = shared;
  newScreen = (s.screenVersion != seenVersion);
  if (newScreen && s.active && s.hasCover) memcpy(cover, sharedCover, COVER_BYTES);
  unlockState();

  uint32_t posMs = s.baseMs;
  if (s.active && s.playing) posMs += (uint32_t)(millis() - s.baseMillis);
  if (s.durationMs && posMs > s.durationMs) posMs = s.durationMs;
  uint32_t sec = posMs / 1000;

  char lyric[LYRIC_TEXT_LEN];
  lyricsGetCurrent(posMs, LYRIC_LEAD_MS, lyric, sizeof(lyric));

  if (newScreen) {
    seenVersion = s.screenVersion;
    if (!s.active) {
      displayDrawIdle();
      lastSec = UINT32_MAX;
    } else {
      displayDrawTrack(s.title, s.artist, s.album, s.hasCover ? cover : nullptr,
                       sec, s.durationMs / 1000, s.playing, lyric);
      // Hitung ulang detik terkini seusai full refresh (~1.8s) agar langsung lanjut per 1 detik
      uint32_t nowMs = s.baseMs;
      if (s.playing) nowMs += (uint32_t)(millis() - s.baseMillis);
      if (s.durationMs && nowMs > s.durationMs) nowMs = s.durationMs;
      lastSec = nowMs / 1000;
    }
    lastPlaying = s.playing;
    strlcpy(lastLyric, lyric, sizeof(lastLyric));
  } else if (s.active) {
    bool lyricChanged = strcmp(lyric, lastLyric) != 0;
    if (lyricChanged) {
      displayUpdateLyricsAndTransport(lyric, sec, s.durationMs / 1000, s.playing);
      strlcpy(lastLyric, lyric, sizeof(lastLyric));
      lastSec = sec;
      lastPlaying = s.playing;
    } else if (sec != lastSec || s.playing != lastPlaying) {
      // Hanya tunda maksimal 220ms jika lirik baru akan masuk tepat di awal detik ini,
      // sehingga detik tetap ter-update setiap 1 detik penuh (tidak pernah melompat)
      uint32_t nextMs = lyricsGetNextChangeMs(posMs, LYRIC_LEAD_MS);
      bool lyricRightNow = (nextMs != UINT32_MAX && nextMs > posMs && (nextMs - posMs) <= 220);

      if (!lyricRightNow || s.playing != lastPlaying) {
        displayUpdateTransport(sec, s.durationMs / 1000, s.playing);
        lastSec = sec;
        lastPlaying = s.playing;
      }
    }
  }

  delay(20);
}
