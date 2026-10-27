// config.example.h — Template Configuration for SpotifyDesk V2
// Copy this file to config.h and fill in your credentials:
//
//   cp config.example.h config.h
//
// 1. Create a Spotify Developer App at https://developer.spotify.com/dashboard
//    - Set Redirect URI to: http://127.0.0.1:8888/callback
//    - Copy your Client ID and Client Secret
//
// 2. Generate Refresh Token by running:
//    python get_refresh_token.py
//    (This opens your browser to log in and automatically saves your token to config.h)
//
// 3. Fill in your Wi-Fi credentials below.

#pragma once

// ---------------------------------------------------------------------------
// Wi-Fi Configuration
// ---------------------------------------------------------------------------
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ---------------------------------------------------------------------------
// Spotify Web API (OAuth2 Refresh Token)
// ---------------------------------------------------------------------------
#define SPOTIFY_CLIENT_ID     "YOUR_SPOTIFY_CLIENT_ID"
#define SPOTIFY_CLIENT_SECRET "YOUR_SPOTIFY_CLIENT_SECRET"
#define SPOTIFY_REFRESH_TOKEN "YOUR_SPOTIFY_REFRESH_TOKEN"

// ---------------------------------------------------------------------------
// Polling & Timing
// ---------------------------------------------------------------------------
#define SPOTIFY_POLL_MS       3000   // Spotify poll interval (progress bar updates locally every 1s)
#define TRANSPORT_INTERVAL_MS 1000   // Local playback timer update interval (1 second)
#define LYRIC_LEAD_MS         0      // Lyric sync offset in ms (0ms = real-time on vocal onset)
