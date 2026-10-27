// spotify.cpp — Spotify Web API client
//
// Kunci performa: koneksi HTTPS ke api.spotify.com DIPERTAHANKAN (keep-alive),
// jadi TLS handshake (~1.5-3 detik di ESP32 klasik) hanya terjadi sekali.
// Poll berikutnya cuma ~150-300 ms.

#include "spotify.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <base64.h>

static String accessToken;
static unsigned long tokenExpiry = 0;
static uint32_t backoffMs = 0;

static WiFiClientSecure apiClient;
static HTTPClient apiHttp;
static bool apiInit = false;

void spotifyBegin()
{
  Serial.printf("[WiFi] Menghubungkan ke %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // matikan modem-sleep -> latency jauh lebih rendah
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\n[WiFi] Terhubung! IP: %s RSSI: %d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else
    Serial.println("\n[WiFi] GAGAL terhubung, akan dicoba ulang otomatis.");
}

void spotifyEnsureWiFi()
{
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("[WiFi] Terputus, reconnect...");
  spotifyDisconnect();
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);
}

bool spotifyRefreshToken()
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) return false;

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String cred = String(SPOTIFY_CLIENT_ID) + ":" + String(SPOTIFY_CLIENT_SECRET);
  http.addHeader("Authorization", "Basic " + base64::encode(cred));

  int code = http.POST("grant_type=refresh_token&refresh_token=" + String(SPOTIFY_REFRESH_TOKEN));
  String body = http.getString();
  http.end();

  if (code != 200) {
    Serial.printf("[Spotify] Refresh token gagal HTTP %d: %s\n", code, body.c_str());
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  accessToken = doc["access_token"].as<String>();
  int expiresIn = doc["expires_in"] | 3600;
  tokenExpiry = millis() + (uint32_t)(expiresIn - 120) * 1000UL;
  Serial.printf("[Spotify] Token OK (%d s)\n", expiresIn);
  return true;
}

void spotifyDisconnect()
{
  if (apiInit) {
    apiHttp.end();
    apiClient.stop();
  }
}

uint32_t spotifyBackoffMs()
{
  uint32_t b = backoffMs;
  backoffMs = 0;
  return b;
}

PollResult spotifyPoll(SpotifyTrack &out)
{
  if (accessToken.isEmpty() || (long)(millis() - tokenExpiry) > 0) {
    spotifyDisconnect();
    if (!spotifyRefreshToken()) return POLL_ERROR;
  }

  if (!apiInit) {
    apiClient.setInsecure();
    apiHttp.setReuse(true);
    static const char *hdrs[] = {"Retry-After"};
    apiHttp.collectHeaders(hdrs, 1);
    apiInit = true;
  }
  apiHttp.setConnectTimeout(6000);
  apiHttp.setTimeout(6000);

  if (!apiHttp.begin(apiClient, "https://api.spotify.com/v1/me/player/currently-playing")) {
    apiClient.stop();
    return POLL_ERROR;
  }
  apiHttp.addHeader("Authorization", "Bearer " + accessToken);

  unsigned long t0 = millis();
  int code = apiHttp.GET();
  out.respMillis = millis();

  if (code == 204) {           // tidak ada yang diputar
    apiHttp.end();
    return POLL_NOTHING;
  }
  if (code == 401) {
    apiHttp.end();
    accessToken = "";
    return POLL_ERROR;
  }
  if (code == 429) {
    String ra = apiHttp.header("Retry-After");
    backoffMs = (ra.length() ? ra.toInt() : 5) * 1000UL;
    apiHttp.end();
    Serial.printf("[Spotify] Rate limited, tunggu %u ms\n", backoffMs);
    return POLL_ERROR;
  }
  if (code != 200) {
    Serial.printf("[Spotify] HTTP %d\n", code);
    apiHttp.end();
    if (code < 0) apiClient.stop();
    return POLL_ERROR;
  }

  String body = apiHttp.getString();
  apiHttp.end();               // koneksi tetap hidup (keep-alive)

  // Kompensasi setengah waktu round-trip
  out.respMillis = t0 + (out.respMillis - t0) / 2;

  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["item"]["id"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["album"]["name"] = true;
  filter["item"]["album"]["images"][0]["url"] = true;
  filter["item"]["album"]["images"][0]["height"] = true;
  filter["item"]["artists"][0]["name"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[Spotify] JSON error: %s\n", err.c_str());
    return POLL_ERROR;
  }

  JsonObject item = doc["item"];
  if (item.isNull()) return POLL_NOTHING;

  out.playing = doc["is_playing"] | false;
  out.progressMs = doc["progress_ms"] | 0;
  out.durationMs = item["duration_ms"] | 0;
  strlcpy(out.id, item["id"] | "", sizeof(out.id));
  strlcpy(out.title, item["name"] | "", sizeof(out.title));
  strlcpy(out.album, item["album"]["name"] | "", sizeof(out.album));

  out.artist[0] = '\0';
  JsonArray artists = item["artists"];
  for (size_t i = 0; i < artists.size() && i < 3; i++) {
    if (i) strlcat(out.artist, ", ", sizeof(out.artist));
    strlcat(out.artist, artists[i]["name"] | "", sizeof(out.artist));
  }

  // Pilih cover ~300px (paling ideal untuk RAM ESP32 & kualitas 96x96)
  out.artUrl[0] = '\0';
  JsonArray images = item["album"]["images"];
  int bestDiff = 100000;
  for (JsonObject img : images) {
    int h = img["height"] | 0;
    int diff = abs(h - 300);
    if (diff < bestDiff) {
      bestDiff = diff;
      strlcpy(out.artUrl, img["url"] | "", sizeof(out.artUrl));
    }
  }
  return POLL_OK;
}
