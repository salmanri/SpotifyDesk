// display.cpp — Driver e-paper + layout MusicStation V1.
//
// Layout & teknik diadaptasi dari MusicStation oleh Roman Likhachev
// (MIT License) — https://github.com/ralihachev/MusicStation

#include "display.h"
#include "pins.h"

#include <SPI.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

// WeAct 2.9" B/W = panel DEPG0290BS, controller SSD1680.
GxEPD2_BW<GxEPD2_290_BS, GxEPD2_290_BS::HEIGHT> epd(
    GxEPD2_290_BS(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
U8G2_FOR_ADAFRUIT_GFX u8g2;

// Font proporsional Helvetica — teks besar, tebal, dan jelas
static const uint8_t *const TITLE_FONT = u8g2_font_helvB14_tf;    // Judul lagu besar & tebal (14pt)
static const uint8_t *const ARTIST_FONT = u8g2_font_helvR08_tf;   // Artis (regular 8pt)
static const uint8_t *const SUBTITLE_FONT = u8g2_font_helvR08_tf; // Album / Keterangan (regular 8pt)
static const uint8_t *const LYRIC_FONT = u8g2_font_helvB12_tf;    // Lirik aktif (besar, tebal, auto-wrap & center)
static const uint8_t *const TIME_FONT = u8g2_font_helvB08_tf;     // Waktu angka tebal

// Full refresh berkala untuk mencegah ghosting
constexpr uint16_t FULL_REFRESH_AFTER_PARTIALS = 300;
static uint16_t partialCount = 0;

// ---------------------------------------------------------------------------
// Simbol Not Balok Musik 32x32 px (tampil saat jeda / solo musik tanpa lirik)
// ---------------------------------------------------------------------------
static const uint8_t MUSIC_NOTE_32x32[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x01, 0xF0,
  0x00, 0x00, 0x1F, 0xF0, 0x00, 0x01, 0xFF, 0xF0,
  0x00, 0x1F, 0xFF, 0xF0, 0x00, 0x1F, 0xFF, 0x70,
  0x00, 0x1F, 0xF1, 0xF0, 0x00, 0x1F, 0x1F, 0xF0,
  0x00, 0x1D, 0xFF, 0xF0, 0x00, 0x1F, 0xFF, 0x70,
  0x00, 0x1F, 0xF0, 0x70, 0x00, 0x1F, 0x00, 0x70,
  0x00, 0x1C, 0x07, 0xF0, 0x00, 0x1C, 0x0F, 0xF0,
  0x00, 0x1C, 0x1F, 0xF0, 0x00, 0x1C, 0x3F, 0xF0,
  0x00, 0x1C, 0x3F, 0xF0, 0x01, 0xFC, 0x3F, 0xF0,
  0x03, 0xFC, 0x1F, 0xE0, 0x07, 0xFC, 0x0F, 0xC0,
  0x0F, 0xFC, 0x07, 0x80, 0x0F, 0xFC, 0x00, 0x00,
  0x0F, 0xFC, 0x00, 0x00, 0x07, 0xF8, 0x00, 0x00,
  0x03, 0xF0, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void drawMusicIcon(int cx, int cy)
{
  epd.drawBitmap(cx - 16, cy - 16, MUSIC_NOTE_32x32, 32, 32, GxEPD_BLACK, GxEPD_WHITE);
}

// ---------------------------------------------------------------------------
// Helper Gambar Teks
// ---------------------------------------------------------------------------
static void drawText(int x, int baselineY, const char *text,
                     const uint8_t *font)
{
  if (!text || !text[0]) return;
  u8g2.setFont(font);
  u8g2.setCursor(x, baselineY);
  u8g2.print(text);
}

static void drawTextRight(int rightX, int baselineY, const char *text,
                          const uint8_t *font)
{
  if (!text || !text[0]) return;
  u8g2.setFont(font);
  int w = u8g2.getUTF8Width(text);
  u8g2.setCursor(rightX - w, baselineY);
  u8g2.print(text);
}

static void drawTextCenter(int centerX, int baselineY, const char *text,
                           const uint8_t *font)
{
  if (!text || !text[0]) return;
  u8g2.setFont(font);
  int w = u8g2.getUTF8Width(text);
  u8g2.setCursor(centerX - w / 2, baselineY);
  u8g2.print(text);
}

static void drawTextFitted(int x, int baselineY, const char *text, int maxW,
                           const uint8_t *font)
{
  if (!text || !text[0]) return;
  u8g2.setFont(font);
  int w = u8g2.getUTF8Width(text);
  if (w <= maxW)
  {
    u8g2.setCursor(x, baselineY);
    u8g2.print(text);
    return;
  }
  char buf[128];
  strncpy(buf, text, sizeof(buf) - 4);
  buf[sizeof(buf) - 4] = '\0';
  int len = strlen(buf);
  while (len > 0)
  {
    buf[len] = '\0';
    strcpy(buf + len, "...");
    if (u8g2.getUTF8Width(buf) <= maxW) break;
    len--;
  }
  u8g2.setCursor(x, baselineY);
  u8g2.print(buf);
}

static void drawTextRightFitted(int rightX, int baselineY, const char *text, int maxW,
                                const uint8_t *font)
{
  if (!text || !text[0]) return;
  u8g2.setFont(font);
  int w = u8g2.getUTF8Width(text);
  if (w <= maxW)
  {
    u8g2.setCursor(rightX - w, baselineY);
    u8g2.print(text);
    return;
  }
  char buf[128];
  strncpy(buf, text, sizeof(buf) - 4);
  buf[sizeof(buf) - 4] = '\0';
  int len = strlen(buf);
  while (len > 0)
  {
    buf[len] = '\0';
    strcpy(buf + len, "...");
    if (u8g2.getUTF8Width(buf) <= maxW) break;
    len--;
  }
  int fitW = u8g2.getUTF8Width(buf);
  u8g2.setCursor(rightX - fitW, baselineY);
  u8g2.print(buf);
}

static void formatTime(uint32_t seconds, char *out, size_t size)
{
  snprintf(out, size, "%lu:%02lu", (unsigned long)(seconds / 60),
           (unsigned long)(seconds % 60));
}

// Pola uji cover art
static void drawCoverTestPattern()
{
  static const uint8_t bayer[4][4] = {
      {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  for (int y = 0; y < COVER_SIZE; y++)
  {
    for (int x = 0; x < COVER_SIZE; x++)
    {
      uint8_t level = (uint8_t)(x * 16 / COVER_SIZE);
      if (level > bayer[y & 3][x & 3])
        epd.drawPixel(COVER_X + x, COVER_Y + y, GxEPD_BLACK);
    }
  }
  epd.drawRect(COVER_X, COVER_Y, COVER_SIZE, COVER_SIZE, GxEPD_BLACK);

  epd.fillRect(COVER_X + 24, COVER_Y + 38, 48, 20, GxEPD_WHITE);
  epd.drawRect(COVER_X + 24, COVER_Y + 38, 48, 20, GxEPD_BLACK);
  drawTextCenter(COVER_X + COVER_SIZE / 2, COVER_Y + 52, "ART", SUBTITLE_FONT);
}

static void drawPlayPauseIcon(int cx, int cy, bool playing)
{
  if (playing)
  {
    epd.fillRect(cx - 5, cy - 5, 3, 10, GxEPD_BLACK);
    epd.fillRect(cx + 2, cy - 5, 3, 10, GxEPD_BLACK);
  }
  else
  {
    epd.fillTriangle(cx - 4, cy - 5, cx - 4, cy + 5, cx + 5, cy, GxEPD_BLACK);
  }
}

// ---------------------------------------------------------------------------
// Header: Judul Lagu (Baris 1) & Artis + Album (Baris 2 Split Seimbang)
// ---------------------------------------------------------------------------
static void drawHeader(const char *title, const char *artist, const char *album)
{
  // Baris 1: Judul lagu penuh (Font besar 14pt tebal)
  if (title && title[0])
  {
    drawTextFitted(4, 15, title, SCREEN_W - 8, TITLE_FONT);
  }

  // Baris 2: Artis di kiri (tebal), Album di kanan (regular)
  const bool hasArtist = (artist && artist[0]);
  const bool hasAlbum = (album && album[0] && (!title || strcmp(title, album) != 0));

  if (hasArtist && hasAlbum)
  {
    // Hitung lebar masing-masing agar tidak saling bertabrakan jika panjang
    u8g2.setFont(ARTIST_FONT);
    int wArt = u8g2.getUTF8Width(artist);
    u8g2.setFont(SUBTITLE_FONT);
    int wAlb = u8g2.getUTF8Width(album);

    const int totalAvailable = SCREEN_W - 8 - 10; // 296 - 8 padding - 10 gap = 278 px
    int maxArtW, maxAlbW;

    if (wArt + wAlb <= totalAvailable)
    {
      maxArtW = wArt;
      maxAlbW = wAlb;
    }
    else if (wArt <= totalAvailable / 2)
    {
      maxArtW = wArt;
      maxAlbW = totalAvailable - wArt;
    }
    else if (wAlb <= totalAvailable / 2)
    {
      maxAlbW = wAlb;
      maxArtW = totalAvailable - wAlb;
    }
    else
    {
      maxArtW = totalAvailable / 2;
      maxAlbW = totalAvailable - maxArtW;
    }

    drawTextFitted(4, 28, artist, maxArtW, ARTIST_FONT);
    drawTextRightFitted(SCREEN_W - 4, 28, album, maxAlbW, SUBTITLE_FONT);
  }
  else if (hasArtist)
  {
    drawTextFitted(4, 28, artist, SCREEN_W - 8, ARTIST_FONT);
  }
  else if (hasAlbum)
  {
    drawTextRightFitted(SCREEN_W - 4, 28, album, SCREEN_W - 8, SUBTITLE_FONT);
  }

  // Garis pemisah di Y=31
  epd.drawFastHLine(0, 31, SCREEN_W, GxEPD_BLACK);
}

// ---------------------------------------------------------------------------
// Area Transport: Waktu (hanya titik dua) & Progress Bar
// ---------------------------------------------------------------------------
static void drawTransport(uint32_t elapsedSec, uint32_t durationSec,
                          bool playing)
{
  epd.fillRect(RIGHT_X, TRANSPORT_Y, RIGHT_W, TRANSPORT_H, GxEPD_WHITE);

  char elapsedText[12];
  char durationText[12];
  formatTime(elapsedSec, elapsedText, sizeof(elapsedText));
  formatTime(durationSec, durationText, sizeof(durationText));

  // Waktu putar di kiri (contoh: 2:31)
  drawText(RIGHT_X + 4, TRANSPORT_Y + 11, elapsedText, TIME_FONT);

  // Total durasi di kanan (contoh: 3:58) — format bersih tanpa tanda minus
  drawTextRight(RIGHT_X + RIGHT_W - 4, TRANSPORT_Y + 11, durationText, TIME_FONT);

  // Ikon Play/Pause di tengah
  drawPlayPauseIcon(RIGHT_X + RIGHT_W / 2, TRANSPORT_Y + 7, playing);

  // Progress bar
  const int barX = RIGHT_X + 4;
  const int barW = RIGHT_W - 8;
  const int barY = TRANSPORT_Y + 16;
  const int barH = 5;

  epd.drawRect(barX, barY, barW, barH, GxEPD_BLACK);
  if (durationSec > 0)
  {
    int fill = (int)((uint64_t)(barW - 2) * min(elapsedSec, durationSec) /
                     durationSec);
    if (fill > 0)
    {
      epd.fillRect(barX + 1, barY + 1, fill, barH - 2, GxEPD_BLACK);
    }
  }
}

// ---------------------------------------------------------------------------
// Area Lirik: HANYA lirik yang sedang dinyanyikan, teks besar, auto-wrap,
// dan CENTER ALIGNMENT (tengah horizontal & tengah vertikal).
// Jika teks kosong (saat jeda panjang/solo musik), digambar simbol not balok.
// ---------------------------------------------------------------------------
static void drawLyrics(const char *text)
{
  // Bersihkan seluruh area lirik
  epd.fillRect(RIGHT_X, LYRIC_AREA_Y, RIGHT_W, LYRIC_AREA_H, GxEPD_WHITE);

  const int centerX = RIGHT_X + (RIGHT_W / 2); // X = 200 (titik tengah kolom kanan)
  const int centerY = LYRIC_AREA_Y + (LYRIC_AREA_H / 2); // Y = 68

  // Jika teks kosong (jeda lagu / solo musik), gambar simbol not balok di tengah!
  if (!text || !text[0])
  {
    drawMusicIcon(centerX, centerY);
    return;
  }

  u8g2.setFont(LYRIC_FONT);
  u8g2.setForegroundColor(GxEPD_BLACK);
  u8g2.setBackgroundColor(GxEPD_WHITE);

  const int maxW = RIGHT_W - 16; // 176 piksel batas lebar

  // Word wrap: pecah teks per kata menjadi beberapa baris ke bawah
  char lines[4][64];
  int lineCount = 0;
  lines[0][0] = '\0';

  char textCopy[256];
  strncpy(textCopy, text, sizeof(textCopy) - 1);
  textCopy[sizeof(textCopy) - 1] = '\0';

  char *word = strtok(textCopy, " ");
  char currentLine[64] = "";

  while (word && lineCount < 4)
  {
    char testLine[64];
    if (currentLine[0] == '\0')
    {
      strncpy(testLine, word, sizeof(testLine) - 1);
      testLine[sizeof(testLine) - 1] = '\0';
    }
    else
    {
      snprintf(testLine, sizeof(testLine), "%s %s", currentLine, word);
    }

    if (u8g2.getUTF8Width(testLine) <= maxW)
    {
      strncpy(currentLine, testLine, sizeof(currentLine) - 1);
      currentLine[sizeof(currentLine) - 1] = '\0';
      word = strtok(nullptr, " ");
    }
    else
    {
      if (currentLine[0] != '\0')
      {
        strncpy(lines[lineCount++], currentLine, sizeof(lines[0]) - 1);
        lines[lineCount - 1][sizeof(lines[0]) - 1] = '\0';
        currentLine[0] = '\0';
      }
      else
      {
        strncpy(lines[lineCount++], word, sizeof(lines[0]) - 1);
        lines[lineCount - 1][sizeof(lines[0]) - 1] = '\0';
        word = strtok(nullptr, " ");
      }
    }
  }

  if (currentLine[0] != '\0' && lineCount < 4)
  {
    strncpy(lines[lineCount++], currentLine, sizeof(lines[0]) - 1);
    lines[lineCount - 1][sizeof(lines[0]) - 1] = '\0';
  }

  // Tentukan koordinat Y baseline untuk perataan vertikal di area 72px (Y=32..103)
  int baseHeights[4];
  if (lineCount == 1)
  {
    baseHeights[0] = LYRIC_AREA_Y + 41;
  }
  else if (lineCount == 2)
  {
    baseHeights[0] = LYRIC_AREA_Y + 31;
    baseHeights[1] = LYRIC_AREA_Y + 54;
  }
  else if (lineCount == 3)
  {
    baseHeights[0] = LYRIC_AREA_Y + 22;
    baseHeights[1] = LYRIC_AREA_Y + 42;
    baseHeights[2] = LYRIC_AREA_Y + 63;
  }
  else // 4 baris
  {
    baseHeights[0] = LYRIC_AREA_Y + 16;
    baseHeights[1] = LYRIC_AREA_Y + 33;
    baseHeights[2] = LYRIC_AREA_Y + 50;
    baseHeights[3] = LYRIC_AREA_Y + 67;
  }

  // Gambar setiap baris dengan perataan TENGAH HORIZONTAL (Center Aligned)
  for (int i = 0; i < lineCount; i++)
  {
    int w = u8g2.getUTF8Width(lines[i]);
    int lineX = centerX - (w / 2);
    if (lineX < RIGHT_X + 4) lineX = RIGHT_X + 4;
    u8g2.setCursor(lineX, baseHeights[i]);
    u8g2.print(lines[i]);
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void displayBegin()
{
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  epd.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));

  epd.init(115200, true, 2, false);
  epd.setRotation(1);  // landscape 296x128
  epd.setTextWrap(false);

  u8g2.begin(epd);
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);
  u8g2.setForegroundColor(GxEPD_BLACK);
  u8g2.setBackgroundColor(GxEPD_WHITE);
}

void displayDrawTestScreen()
{
  epd.setFullWindow();
  epd.fillScreen(GxEPD_WHITE);

  drawText(4, 15, "SpotifyDesk", TITLE_FONT);
  drawText(4, 28, "Hardware Test", ARTIST_FONT);
  drawTextRight(SCREEN_W - 4, 28, "ESP32 + E-Paper", SUBTITLE_FONT);
  epd.drawFastHLine(0, 31, SCREEN_W, GxEPD_BLACK);

  drawCoverTestPattern();

  drawLyrics(""); // Tes simbol not balok
  drawTransport(0, 210, true);

  epd.display(false);
  partialCount = 0;
}

void displayDrawIdle()
{
  epd.setFullWindow();
  epd.fillScreen(GxEPD_WHITE);

  drawText(4, 15, "SpotifyDesk", TITLE_FONT);
  drawText(4, 28, "Standby", ARTIST_FONT);
  drawTextRight(SCREEN_W - 4, 28, "Menunggu musik...", SUBTITLE_FONT);
  epd.drawFastHLine(0, 31, SCREEN_W, GxEPD_BLACK);

  drawCoverTestPattern();

  drawLyrics(""); // Simbol not balok
  drawTransport(0, 0, false);

  epd.display(false);
  partialCount = 0;
}

void displayDrawTrack(const char *title, const char *artist, const char *album,
                      const uint8_t *coverBitmap,
                      uint32_t elapsedSec, uint32_t durationSec, bool playing,
                      const char *lyricText)
{
  epd.setFullWindow();
  epd.fillScreen(GxEPD_WHITE);

  // 1. Header (Judul besar di baris 1, Artis - Album di baris 2)
  drawHeader(title, artist, album);

  // 2. Artwork (96x96 px di X=4, Y=32)
  if (coverBitmap)
  {
    epd.drawBitmap(COVER_X, COVER_Y, coverBitmap, COVER_SIZE, COVER_SIZE,
                   GxEPD_BLACK, GxEPD_WHITE);
  }
  else
  {
    drawCoverTestPattern();
  }
  epd.drawRect(COVER_X, COVER_Y, COVER_SIZE, COVER_SIZE, GxEPD_BLACK);

  // 3. Lirik (atau simbol not balok di tengah jika jeda/intro)
  drawLyrics(lyricText);

  // 4. Transport (waktu elapsed, icon, total duration, progress bar)
  drawTransport(elapsedSec, durationSec, playing);

  epd.display(false);  // 1x full refresh untuk seluruh layar
  partialCount = 0;
}

void displayUpdateTransport(uint32_t elapsedSec, uint32_t durationSec,
                            bool playing)
{
  drawTransport(elapsedSec, durationSec, playing);

  if (++partialCount >= FULL_REFRESH_AFTER_PARTIALS)
  {
    epd.display(false);
    partialCount = 0;
  }
  else
  {
    epd.displayWindow(RIGHT_X, TRANSPORT_Y, RIGHT_W, TRANSPORT_H);
  }
}

void displayUpdateLyrics(const char *lyricText)
{
  drawLyrics(lyricText);

  if (++partialCount >= FULL_REFRESH_AFTER_PARTIALS)
  {
    epd.display(false);
    partialCount = 0;
  }
  else
  {
    epd.displayWindow(RIGHT_X, LYRIC_AREA_Y, RIGHT_W, LYRIC_AREA_H);
  }
}


void displayUpdateLyricsAndTransport(const char *lyricText, uint32_t elapsedSec,
                                     uint32_t durationSec, bool playing)
{
  drawLyrics(lyricText);
  drawTransport(elapsedSec, durationSec, playing);

  if (++partialCount >= FULL_REFRESH_AFTER_PARTIALS)
  {
    epd.display(false);
    partialCount = 0;
  }
  else
  {
    // Update seluruh kolom kanan dari Y=32 sampai Y=128 sekaligus
    epd.displayWindow(RIGHT_X, LYRIC_AREA_Y, RIGHT_W, LYRIC_AREA_H + TRANSPORT_H);
  }
}
