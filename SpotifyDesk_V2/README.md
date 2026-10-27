# SpotifyDesk V2 — Standalone Wi-Fi ESP32 E-Paper Display

SpotifyDesk V2 is a 100% standalone desktop music display running entirely on an ESP32 microcontroller with no host PC required.

![SpotifyDesk in Action](../assets/spotify_desk_preview.png)

## ✨ Key Features
* **100% Standalone**: Connects directly to your home Wi-Fi; just plug it into a 5V USB charger.
* **Dual-Core Architecture**:
  * **Core 0**: Wi-Fi, Spotify Web API HTTPS keep-alive, JPEG cover art download, LRCLIB lyrics streaming.
  * **Core 1**: Non-blocking SSD1680 display loop, 1-second transport timer, millisecond lyric updates.
* **Embedded Image Processing**: High-fidelity sub-pixel continuous area averaging downscaling and mean-centered contrast enhancement (99.97% identical to desktop PIL).
* **Live Synced Lyrics**: Automatic line-by-line lyrics from LRCLIB with duration matching.

---

## 🔌 Hardware Wiring

| E-Paper Pin | ESP32 DOIT Pin | GPIO |
|---|---|---|
| **3.3V / VCC** | `3V3` | 3.3V |
| **GND** | `GND` | GND |
| **SCL / SCK** | `D18` | GPIO 18 |
| **SDA / MOSI** | `D23` | GPIO 23 |
| **CS** | `D5` | GPIO 5 |
| **DC** | `TX2` | GPIO 17 |
| **RES** | `RX2` | GPIO 16 |
| **BUSY** | `D4` | GPIO 4 |

---

## 🚀 Setup Instructions

1. **Create Spotify Developer App**:
   * Visit [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) and log in.
   * Click **Create app**:
     * App Name: `SpotifyDesk`
     * Redirect URI: `http://127.0.0.1:8888/callback`
   * Note down your **Client ID** and **Client Secret**.

2. **Retrieve Refresh Token**:
   ```bash
   python get_refresh_token.py
   ```
   * Enter your Client ID and Client Secret. Log in via your browser. The token is automatically saved to `SpotifyDeskV2/config.h`!

3. **Configure Wi-Fi**:
   * In `SpotifyDeskV2/config.h`, enter your Wi-Fi SSID and Password:
     ```cpp
     #define WIFI_SSID     "YourWiFiSSID"
     #define WIFI_PASSWORD "YourWiFiPassword"
     ```

4. **Upload to ESP32**:
   * Open `SpotifyDeskV2/SpotifyDeskV2.ino` in Arduino IDE.
   * Required Libraries: `GxEPD2`, `Adafruit GFX`, `U8g2_for_Adafruit_GFX`, `ArduinoJson` (v7), `JPEGDEC`.
   * Click **Upload**.
