// spotify.h — Spotify Web API client (keep-alive HTTPS, low memory)
#pragma once
#include <Arduino.h>

struct SpotifyTrack {
  char id[40];
  char title[96];
  char artist[96];
  char album[96];
  char artUrl[200];
  uint32_t progressMs;
  uint32_t durationMs;
  bool playing;
  unsigned long respMillis;   // millis() saat respons diterima
};

enum PollResult { POLL_OK, POLL_NOTHING, POLL_ERROR };

void spotifyBegin();                    // connect Wi-Fi
void spotifyEnsureWiFi();
bool spotifyRefreshToken();
PollResult spotifyPoll(SpotifyTrack &out);
void spotifyDisconnect();               // tutup koneksi keep-alive (bebaskan RAM TLS)
uint32_t spotifyBackoffMs();            // jeda tambahan jika kena rate limit (429)
