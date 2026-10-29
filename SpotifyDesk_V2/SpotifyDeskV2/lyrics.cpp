// lyrics.cpp — Client LRCLIB dengan sinkronisasi presisi, tag [offset], & durasi match
#include "lyrics.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <ctype.h>
#include <string.h>

static LyricLine lines[MAX_LYRIC_LINES];
static int lineCount = 0;
static int stagingCount = 0;
static int32_t lrcOffsetMs = 0;
static SemaphoreHandle_t lyrMutex = nullptr;

static void ensureMutex()
{
  if (!lyrMutex) lyrMutex = xSemaphoreCreateMutex();
}

void lyricsReset()
{
  ensureMutex();
  xSemaphoreTake(lyrMutex, portMAX_DELAY);
  lineCount = 0;
  stagingCount = 0;
  lrcOffsetMs = 0;
  xSemaphoreGive(lyrMutex);
}

int lyricsGetLineCount()
{
  ensureMutex();
  xSemaphoreTake(lyrMutex, portMAX_DELAY);
  int n = lineCount;
  xSemaphoreGive(lyrMutex);
  return n;
}

static void normalizeText(char *s)
{
  const char *r = s;
  char *wr = s;
  while (*r) {
    if ((uint8_t)*r == 0xE2 && (uint8_t)*(r + 1) == 0x80) {
      uint8_t d = (uint8_t)*(r + 2);
      if (d == 0x98 || d == 0x99) { *wr++ = '\''; r += 3; continue; }
      if (d == 0x9C || d == 0x9D) { *wr++ = '"';  r += 3; continue; }
      if (d == 0x93 || d == 0x94) { *wr++ = '-';  r += 3; continue; }
      if (d == 0xA6) { *wr++ = '.'; *wr++ = '.'; *wr++ = '.'; r += 3; continue; }
    }
    *wr++ = *r++;
  }
  *wr = '\0';
}

static bool parseUInt(const char *&p, int &val, int &digits)
{
  val = 0; digits = 0;
  while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; digits++; }
  return digits > 0;
}

// Parse satu baris LRC: "[mm:ss.xx][mm:ss.xx] teks" atau "[offset:+/-ms]"
static void parseLrcLine(char *line)
{
  if (strncmp(line, "[offset:", 8) == 0 || strncmp(line, "[OFFSET:", 8) == 0) {
    lrcOffsetMs = atoi(line + 8);
    return;
  }

  const char *p = line;
  uint32_t times[4];
  int nt = 0;

  while (*p == '[') {
    const char *q = p + 1;
    int mm, ss, frac = 0, d;
    if (!parseUInt(q, mm, d) || *q != ':') break;
    q++;
    if (!parseUInt(q, ss, d)) break;
    int fracMs = 0;
    if (*q == '.' || *q == ':') {
      q++;
      if (parseUInt(q, frac, d)) {
        if (d == 1) fracMs = frac * 100;
        else if (d == 2) fracMs = frac * 10;
        else { while (d > 3) { frac /= 10; d--; } fracMs = frac; }
      }
    }
    if (*q != ']') break;
    if (nt < 4) times[nt++] = (uint32_t)(mm * 60 + ss) * 1000UL + fracMs;
    p = q + 1;
  }
  if (nt == 0) return;

  while (*p == ' ') p++;
  char text[LYRIC_TEXT_LEN];
  strlcpy(text, p, sizeof(text));
  // trim kanan
  int len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\r')) text[--len] = '\0';
  normalizeText(text);
  // Baris simbol musik dianggap jeda instrumental (kosong)
  if (strcmp(text, "\xE2\x99\xAA") == 0 || strcmp(text, "\xE2\x99\xAB") == 0) text[0] = '\0';

  for (int i = 0; i < nt && stagingCount < MAX_LYRIC_LINES; i++) {
    lines[stagingCount].time_ms = times[i];
    strlcpy(lines[stagingCount].text, text, LYRIC_TEXT_LEN);
    stagingCount++;
  }
}

static int readByteTimeout(WiFiClient *s, uint32_t timeoutMs)
{
  unsigned long t = millis();
  while (!s->available()) {
    if (!s->connected() || millis() - t > timeoutMs) return -1;
    delay(1);
  }
  return s->read();
}

static void appendUtf8(char *buf, int &li, int cap, uint32_t cp)
{
  if (cp == 0x2018 || cp == 0x2019) cp = '\'';
  else if (cp == 0x201C || cp == 0x201D) cp = '"';
  else if (cp == 0x2013 || cp == 0x2014) cp = '-';

  if (cp < 0x80) { if (li < cap - 1) buf[li++] = (char)cp; }
  else if (cp < 0x800) {
    if (li < cap - 2) { buf[li++] = 0xC0 | (cp >> 6); buf[li++] = 0x80 | (cp & 0x3F); }
  } else {
    if (li < cap - 3) {
      buf[li++] = 0xE0 | (cp >> 12);
      buf[li++] = 0x80 | ((cp >> 6) & 0x3F);
      buf[li++] = 0x80 | (cp & 0x3F);
    }
  }
}

// Baca stream JSON. Jika filterDurationSec > 0 (saat search), lewati hasil dengan durasi beda > 2 detik.
static bool streamSyncedLyrics(WiFiClient *s, int filterDurationSec)
{
  static const char KEY_DUR[] = "\"duration\":";
  const int KLEN_DUR = sizeof(KEY_DUR) - 1;
  int kpos_dur = 0;

  static const char KEY_SYN[] = "\"syncedLyrics\":";
  const int KLEN_SYN = sizeof(KEY_SYN) - 1;
  int kpos_syn = 0;

  enum { SEARCH, IN_STRING } st = SEARCH;
  char line[200];
  int li = 0;
  int curDuration = 0;

  while (true) {
    int c = readByteTimeout(s, 6000);
    if (c < 0) break;

    if (st == SEARCH) {
      // 1. Deteksi "duration":
      if (c == KEY_DUR[kpos_dur]) {
        if (++kpos_dur == KLEN_DUR) {
          kpos_dur = 0;
          int durVal = 0;
          int firstFrac = 0;
          bool inFrac = false;
          while (true) {
            int d = readByteTimeout(s, 4000);
            if (d < 0) break;
            if (d >= '0' && d <= '9') {
              if (!inFrac) durVal = durVal * 10 + (d - '0');
              else if (firstFrac == 0) firstFrac = (d - '0');
            } else if (d == '.') {
              inFrac = true;
            } else if (d != ' ') {
              break;
            }
          }
          if (firstFrac >= 5) durVal++;
          curDuration = durVal;
        }
      } else {
        kpos_dur = (c == KEY_DUR[0]) ? 1 : 0;
      }

      // 2. Deteksi "syncedLyrics":
      if (c == KEY_SYN[kpos_syn]) {
        if (++kpos_syn == KLEN_SYN) {
          kpos_syn = 0;
          // Lewati spasi sampai '"' atau 'n' (null)
          int quote = -1;
          while (true) {
            int ch = readByteTimeout(s, 4000);
            if (ch < 0) break;
            if (ch == '"' || ch == 'n') { quote = ch; break; }
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
          }

          if (quote == '"') {
            // Cek apakah durasi cocok (jika filter aktif)
            if (filterDurationSec > 0 && curDuration > 0 && abs(curDuration - filterDurationSec) > 2) {
              Serial.printf("[Lyrics] Lewati durasi %d s (target %d s)\n", curDuration, filterDurationSec);
              curDuration = 0;
              // Skip isi string sampai closing quote
              bool esc = false;
              while (true) {
                int ch = readByteTimeout(s, 4000);
                if (ch < 0) break;
                if (esc) esc = false;
                else if (ch == '\\') esc = true;
                else if (ch == '"') break;
              }
              continue; // lanjut cari item berikutnya di stream
            }

            // Durasi cocok atau get langsung -> parse lirik!
            st = IN_STRING;
            li = 0;
            stagingCount = 0;
            lrcOffsetMs = 0;
          }
        }
      } else {
        kpos_syn = (c == KEY_SYN[0]) ? 1 : 0;
      }
    } else { // IN_STRING
      if (c == '\\') {
        int e = readByteTimeout(s, 6000);
        if (e < 0) break;
        if (e == 'n') { line[li] = '\0'; parseLrcLine(line); li = 0; }
        else if (e == 'r') { /* abaikan */ }
        else if (e == 't') { if (li < (int)sizeof(line) - 1) line[li++] = ' '; }
        else if (e == 'u') {
          char hx[5] = {0};
          for (int k = 0; k < 4; k++) { int h = readByteTimeout(s, 6000); if (h < 0) return stagingCount > 0; hx[k] = (char)h; }
          appendUtf8(line, li, sizeof(line), strtoul(hx, nullptr, 16));
        } else {
          if (li < (int)sizeof(line) - 1) line[li++] = (char)e;
        }
      } else if (c == '"') {
        line[li] = '\0';
        parseLrcLine(line);
        if (stagingCount > 0) return true;
        st = SEARCH;
      } else if (c == '\n') {
        line[li] = '\0'; parseLrcLine(line); li = 0;
      } else {
        if (li < (int)sizeof(line) - 1) line[li++] = (char)c;
      }
    }
  }
  return stagingCount > 0;
}

static String urlEncode(const char *s)
{
  String out;
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); out += b; }
  }
  return out;
}

static void cleanTitle(const char *in, char *out, size_t len)
{
  strlcpy(out, in, len);
  char *p = out;
  char *w = out;
  int paren = 0, brack = 0;
  while (*p) {
    if (*p == '(') paren++;
    else if (*p == ')') { if (paren > 0) paren--; }
    else if (*p == '[') brack++;
    else if (*p == ']') { if (brack > 0) brack--; }
    else if (paren == 0 && brack == 0) {
      *w++ = *p;
    }
    p++;
  }
  *w = '\0';

  char *dash = strstr(out, " - ");
  if (dash) *dash = '\0';

  int l = strlen(out);
  while (l > 0 && out[l - 1] == ' ') out[--l] = '\0';
}

static bool fetchUrl(const String &url, int filterDurationSec)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", "SpotifyDeskV2/2.0 (https://github.com/salmanri/SpotifyDesk)");
  int code = http.GET();
  bool ok = false;
  if (code == 200) ok = streamSyncedLyrics(http.getStreamPtr(), filterDurationSec);
  else Serial.printf("[Lyrics] HTTP %d\n", code);
  http.end();
  return ok;
}

static int publish()
{
  // Terapkan tag [offset:+/-ms] jika ada di dalam file LRC
  if (lrcOffsetMs != 0) {
    for (int i = 0; i < stagingCount; i++) {
      int32_t t = (int32_t)lines[i].time_ms - lrcOffsetMs;
      lines[i].time_ms = (t > 0) ? (uint32_t)t : 0;
    }
  }

  for (int i = 1; i < stagingCount; i++) {
    LyricLine tmp = lines[i];
    int j = i - 1;
    while (j >= 0 && lines[j].time_ms > tmp.time_ms) { lines[j + 1] = lines[j]; j--; }
    lines[j + 1] = tmp;
  }
  xSemaphoreTake(lyrMutex, portMAX_DELAY);
  lineCount = stagingCount;
  xSemaphoreGive(lyrMutex);
  Serial.printf("[Lyrics] %d baris dimuat & sinkron (offset %d ms)\n", stagingCount, (int)lrcOffsetMs);
  return stagingCount;
}

int lyricsFetch(const char *title, const char *artist, const char *album, int durationSec)
{
  lyricsReset();

  char firstArtist[96];
  strlcpy(firstArtist, artist, sizeof(firstArtist));
  char *comma = strchr(firstArtist, ',');
  if (comma) *comma = '\0';
  int al = strlen(firstArtist);
  while (al > 0 && firstArtist[al - 1] == ' ') firstArtist[--al] = '\0';

  char ct[96];
  cleanTitle(title, ct, sizeof(ct));

  String url;

  // 1. Pencocokan paling spesifik: judul + artis + album + durasi
  if (album && album[0]) {
    url = "https://lrclib.net/api/get?track_name=" + urlEncode(title) +
          "&artist_name=" + urlEncode(firstArtist) +
          "&album_name=" + urlEncode(album) +
          "&duration=" + String(durationSec);
    if (fetchUrl(url, 0)) return publish();
  }

  // 2. Pencocokan langsung tanpa album_name (agar varian album Deluxe/Anniversary tetap cocok)
  url = "https://lrclib.net/api/get?track_name=" + urlEncode(title) +
        "&artist_name=" + urlEncode(firstArtist) +
        "&duration=" + String(durationSec);
  if (fetchUrl(url, 0)) return publish();

  // 3. Jika judul ada keterangan tambahan (Remastered/feat), coba dengan judul bersih
  if (strcmp(ct, title) != 0 && strlen(ct) > 0) {
    url = "https://lrclib.net/api/get?track_name=" + urlEncode(ct) +
          "&artist_name=" + urlEncode(firstArtist) +
          "&duration=" + String(durationSec);
    if (fetchUrl(url, 0)) return publish();
  }

  // 4. Fallback search (dengan filter durasi presisi +-2 detik agar tidak salah mengambil versi radio/live)
  url = "https://lrclib.net/api/search?track_name=" + urlEncode(ct) +
        "&artist_name=" + urlEncode(firstArtist);
  if (fetchUrl(url, durationSec)) return publish();

  Serial.println("[Lyrics] Tidak ditemukan di LRCLIB");
  return 0;
}

void lyricsGetCurrent(uint32_t positionMs, int leadMs, char *out, size_t outLen)
{
  out[0] = '\0';
  ensureMutex();
  xSemaphoreTake(lyrMutex, portMAX_DELAY);
  int n = lineCount;
  if (n > 0) {
    int32_t p = (int32_t)positionMs + leadMs;
    uint32_t pos = (p > 0) ? (uint32_t)p : 0;
    int idx = -1;
    // binary search: baris terakhir dengan time_ms <= pos
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (lines[mid].time_ms <= pos) { idx = mid; lo = mid + 1; }
      else hi = mid - 1;
    }
    if (idx >= 0) {
      uint32_t inLine = pos - lines[idx].time_ms;
      bool show = true;
      if (idx + 1 < n) {
        uint32_t gap = lines[idx + 1].time_ms - lines[idx].time_ms;
        // Jeda instrumental panjang (>= 20 detik) -> sembunyikan lirik (tampilkan ♪)
        if (gap >= 20000) {
          uint32_t dur = max((uint32_t)8000, (uint32_t)strlen(lines[idx].text) * 350);
          if (inLine >= dur) show = false;
        }
      } else if (inLine >= 10000) {
        show = false;
      }
      if (show) strlcpy(out, lines[idx].text, outLen);
    }
  }
  xSemaphoreGive(lyrMutex);
}

uint32_t lyricsGetNextChangeMs(uint32_t positionMs, int leadMs)
{
  uint32_t nextChange = UINT32_MAX;
  ensureMutex();
  xSemaphoreTake(lyrMutex, portMAX_DELAY);
  int n = lineCount;
  if (n > 0) {
    int32_t p = (int32_t)positionMs + leadMs;
    uint32_t pos = (p > 0) ? (uint32_t)p : 0;
    int idx = -1;
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (lines[mid].time_ms <= pos) { idx = mid; lo = mid + 1; }
      else hi = mid - 1;
    }
    if (idx < 0) {
      nextChange = lines[0].time_ms;
    } else if (idx + 1 < n) {
      nextChange = lines[idx + 1].time_ms;
    }
  }
  xSemaphoreGive(lyrMutex);
  return nextChange;
}
