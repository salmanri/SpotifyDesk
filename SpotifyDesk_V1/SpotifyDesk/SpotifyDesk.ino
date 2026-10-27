// SpotifyDesk — Desktop Companion E-Paper Display
//
// Menerima data lagu dari desktop_companion.py via Serial USB,
// lalu menampilkan cover art, metadata, lirik yang sedang dinyanyikan (auto-wrap),
// dan progress bar di layar e-paper 2.9" sesuai layout MusicStation V1.
//
// Board  : DOIT ESP32 DEVKIT V1
// Library: GxEPD2, Adafruit GFX Library, U8g2_for_Adafruit_GFX

#include "display.h"

// ---------------------------------------------------------------------------
// Cover Art Buffer
// ---------------------------------------------------------------------------
constexpr size_t COVER_BYTES = (COVER_SIZE * COVER_SIZE) / 8;  // 1152
constexpr size_t ART_CHUNK_SIZE = COVER_BYTES / 4;             // 288 bytes
uint8_t coverBuffer[COVER_BYTES];
bool hasCover = false;
int artChunksReceived = 0;

// ---------------------------------------------------------------------------
// State lagu saat ini
// ---------------------------------------------------------------------------
String currentTitle = "";
String currentArtist = "";
String currentAlbum = "";
uint32_t currentElapsed = 0;
uint32_t currentDuration = 0;
bool currentPlaying = false;
bool isIdle = true;
bool trackDrawn = false;

// Lirik yang sedang dinyanyikan saat ini (auto-wrap)
String currentLyric = "";

// ---------------------------------------------------------------------------
// Timer lokal — ESP32 menjalankan timer lokal berbasis millis()
// sehingga progress bar dan detik berjalan mulus tanpa delay Serial.
// ---------------------------------------------------------------------------
uint32_t baseElapsed = 0;
unsigned long baseMillis = 0;
constexpr uint32_t TRANSPORT_INTERVAL_MS = 1000;  // refresh akurat per 1 detik sesuai permintaan user
unsigned long lastTransportMs = 0;

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
static uint8_t hexNibble(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// Parse delimited string: ambil field ke-N (0-based)
static String getField(const String &str, int startIdx, int fieldNum,
                       char delimiter = '|')
{
  int idx = startIdx;
  for (int f = 0; f < fieldNum; f++)
  {
    idx = str.indexOf(delimiter, idx);
    if (idx < 0) return "";
    idx++;
  }
  int endIdx = str.indexOf(delimiter, idx);
  return (endIdx < 0) ? str.substring(idx) : str.substring(idx, endIdx);
}

// ---------------------------------------------------------------------------
// Command Handler
// ---------------------------------------------------------------------------
void handleCommand(const String &line)
{
  // META|<playing>|<elapsed>|<duration>|<title>|<artist>|<album>
  if (line.startsWith("META|"))
  {
    currentPlaying = (getField(line, 5, 0).toInt() == 1);
    currentElapsed = getField(line, 5, 1).toInt();
    currentDuration = getField(line, 5, 2).toInt();
    currentTitle = getField(line, 5, 3);
    currentArtist = getField(line, 5, 4);
    currentAlbum = getField(line, 5, 5);

    baseElapsed = currentElapsed;
    baseMillis = millis();

    // Reset cover & lyric state
    artChunksReceived = 0;
    hasCover = false;
    currentLyric = "";
    isIdle = false;
    trackDrawn = false;
    lastTransportMs = millis();
    Serial.println("ACK_META");
  }
  // ART:<chunk_index>:<hex data>  (4 chunks, masing-masing 576 hex chars = 288 bytes)
  else if (line.startsWith("ART:"))
  {
    int colonPos = line.indexOf(':', 4);
    if (colonPos > 4)
    {
      int chunkIdx = line.substring(4, colonPos).toInt();
      if (chunkIdx >= 0 && chunkIdx < 4)
      {
        String hexData = line.substring(colonPos + 1);
        size_t offset = chunkIdx * ART_CHUNK_SIZE;
        size_t bytesToRead = (size_t)(hexData.length() / 2);
        if (bytesToRead > ART_CHUNK_SIZE) bytesToRead = ART_CHUNK_SIZE;
        for (size_t i = 0; i < bytesToRead; i++)
        {
          uint8_t hi = hexNibble(hexData.charAt(i * 2));
          uint8_t lo = hexNibble(hexData.charAt(i * 2 + 1));
          coverBuffer[offset + i] = (hi << 4) | lo;
        }
        artChunksReceived |= (1 << chunkIdx);
      }
    }
    Serial.printf("ACK_ART:%d\n", artChunksReceived);
  }
  // ART_END — semua chunk artwork diterima, gambar seluruh layar dalam 1 full refresh
  else if (line == "ART_END")
  {
    hasCover = (artChunksReceived == 0x0F);  // 4 chunk lengkap
    displayDrawTrack(currentTitle.c_str(), currentArtist.c_str(),
                     currentAlbum.c_str(), hasCover ? coverBuffer : nullptr,
                     currentElapsed, currentDuration, currentPlaying,
                     currentLyric.c_str());
    trackDrawn = true;
    baseMillis = millis();
    lastTransportMs = millis();
    Serial.println("ACK_DRAW");
  }
  // SYNC|<playing>|<elapsed>|<duration> — koreksi posisi waktu dari PC
  else if (line.startsWith("SYNC|"))
  {
    bool wasPlaying = currentPlaying;
    currentPlaying = (getField(line, 5, 0).toInt() == 1);
    uint32_t syncElapsed = getField(line, 5, 1).toInt();
    currentDuration = getField(line, 5, 2).toInt();

    int diff = abs((int)currentElapsed - (int)syncElapsed);

    // Update basis waktu jika play/pause berubah atau beda waktu >= 3 detik (seek manual)
    if (wasPlaying != currentPlaying || diff >= 3)
    {
      baseElapsed = syncElapsed;
      baseMillis = millis();
      currentElapsed = syncElapsed;

      if (trackDrawn)
      {
        displayUpdateTransport(currentElapsed, currentDuration, currentPlaying);
        lastTransportMs = millis();
      }
      else
      {
        Serial.println("REQ_META");
      }
    }
    else if (!trackDrawn)
    {
      Serial.println("REQ_META");
    }
  }
  // LYRIC|<text lirik yang sedang dinyanyikan>
  else if (line.startsWith("LYRIC|"))
  {
    String newLyric = line.substring(6);

    if (newLyric != currentLyric)
    {
      currentLyric = newLyric;
      if (trackDrawn)
      {
        displayUpdateLyrics(currentLyric.c_str());
        lastTransportMs = millis();  // Jeda transport sejenak agar tidak tabrakan
      }
      else
      {
        Serial.println("REQ_META");
      }
    }
  }
  // IDLE
  else if (line == "IDLE")
  {
    if (!isIdle)
    {
      isIdle = true;
      trackDrawn = false;
      displayDrawIdle();
      Serial.println("ACK_IDLE");
    }
  }
}

// ---------------------------------------------------------------------------
// Arduino setup & loop
// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  Serial.setRxBufferSize(4096);
  Serial.setTimeout(50);
  delay(300);

  Serial.println();
  Serial.println("=== SpotifyDesk Ready ===");

  displayBegin();
  displayDrawIdle();
  Serial.println("READY");
  Serial.println("REQ_META");
}

void loop()
{
  // 1. Handle serial commands dari PC companion
  while (Serial.available())
  {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      handleCommand(line);
    }
  }

  // 2. Timer lokal untuk transport (hanya saat tidak ada data Serial antre agar lirik diprioritaskan)
  if (Serial.available() == 0 && trackDrawn && currentPlaying && currentDuration > 0)
  {
    unsigned long now = millis();
    if (now - lastTransportMs >= TRANSPORT_INTERVAL_MS)
    {
      lastTransportMs = now;
      uint32_t sec = baseElapsed + (now - baseMillis) / 1000;
      if (sec > currentDuration) sec = currentDuration;
      currentElapsed = sec;
      displayUpdateTransport(currentElapsed, currentDuration, currentPlaying);
    }
  }
}
