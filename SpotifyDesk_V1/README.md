# SpotifyDesk V1 — PC-Assisted Desktop Companion

SpotifyDesk V1 is a PC-assisted music companion for your desk. It connects your ESP32 e-paper display to your computer via USB Serial while a lightweight Python daemon tracks currently playing media directly from Windows OS Media Sessions.

![SpotifyDesk in Action](../assets/spotify_desk_preview.png)

## ✨ Key Features
* **Zero Cloud Setup**: No Spotify Developer App, Client ID, or Client Secret needed.
* **Works with Free & Premium**: Compatible with Spotify Free, Spotify Premium, YouTube Music in your browser, VLC, or local media players.
* **Synchronized Lyrics**: Fetches synchronized lyrics on the fly from [LRCLIB](https://lrclib.net/).
* **Pillow Dithering**: High-quality Floyd-Steinberg dithering processed on the host computer.

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

1. **Install Python Dependencies**:
   ```bash
   pip install -r requirements.txt
   ```

2. **Upload ESP32 Firmware**:
   * Open `SpotifyDesk/SpotifyDesk.ino` in Arduino IDE.
   * Board: **DOIT ESP32 DEVKIT V1**.
   * Libraries needed: `GxEPD2`, `Adafruit GFX`, `U8g2_for_Adafruit_GFX`.
   * Click **Upload**.

3. **Start the Companion Daemon**:
   ```bash
   python desktop_companion.py
   ```
   * It will automatically detect your ESP32 COM port and start displaying your media!
