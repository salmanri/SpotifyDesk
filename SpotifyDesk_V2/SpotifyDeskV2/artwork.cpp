// artwork.cpp — Album art downloader & dithering kualitas tinggi untuk ESP32
//
// FITUR KUALITAS TINGGI (Setara Desktop Companion V1 / PIL):
// - Continuous sub-pixel area averaging (menghilangkan grid moiré & distorsi teks)
// - Mean-centered contrast enhancement faktor 1.25 (identik PIL ImageEnhance.Contrast)
// - Floyd-Steinberg dithering 16-fraction dengan error diffusion presisi tinggi
// - Sangat hemat RAM: hanya 2 baris error buffer (bukan 18 KB) & heap-safe

#include "artwork.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <new>
#include <math.h>

static uint8_t grayBuf[COVER_SIZE * COVER_SIZE];
static uint8_t *decoded = nullptr;
static int outW = 0, outH = 0;

static int jpegDraw(JPEGDRAW *d)
{
  for (int y = 0; y < d->iHeight; y++) {
    int dy = d->y + y;
    if (dy >= outH) break;
    const uint16_t *row = d->pPixels + y * d->iWidth;
    for (int x = 0; x < d->iWidth; x++) {
      int dx = d->x + x;
      if (dx >= outW) break;
      uint16_t px = row[x];
      // Konversi RGB565 ke RGB888
      uint32_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
      r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
      // Grayscale BT.601 (identik dengan PIL Image.convert('L'))
      decoded[dy * outW + dx] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    }
  }
  return 1;
}

// Continuous sub-pixel area averaging downscale (kualitas setara LANCZOS/Box di PIL Python)
static void resizeTo96(const uint8_t *src, int sw, int sh, uint8_t *dst)
{
  int side = min(sw, sh);
  int ox = (sw - side) / 2;
  int oy = (sh - side) / 2;
  const float scale = (float)side / (float)COVER_SIZE;

  for (int y = 0; y < COVER_SIZE; y++) {
    float y_start = oy + y * scale;
    float y_end   = oy + (y + 1) * scale;
    int iy0 = (int)floorf(y_start);
    int iy1 = (int)ceilf(y_end);
    if (iy0 < 0) iy0 = 0;
    if (iy1 > sh) iy1 = sh;

    for (int x = 0; x < COVER_SIZE; x++) {
      float x_start = ox + x * scale;
      float x_end   = ox + (x + 1) * scale;
      int ix0 = (int)floorf(x_start);
      int ix1 = (int)ceilf(x_end);
      if (ix0 < 0) ix0 = 0;
      if (ix1 > sw) ix1 = sw;

      float sumVal = 0.0f;
      float weightSum = 0.0f;

      for (int iy = iy0; iy < iy1; iy++) {
        float wy = min(y_end, (float)(iy + 1)) - max(y_start, (float)iy);
        if (wy <= 0.0f) continue;
        const uint8_t *row = src + iy * sw;
        for (int ix = ix0; ix < ix1; ix++) {
          float wx = min(x_end, (float)(ix + 1)) - max(x_start, (float)ix);
          if (wx <= 0.0f) continue;
          float w = wx * wy;
          sumVal += row[ix] * w;
          weightSum += w;
        }
      }
      dst[y * COVER_SIZE + x] = (weightSum > 0.0f) ? (uint8_t)(sumVal / weightSum + 0.5f) : 128;
    }
  }
}

static void ditherToMono(uint8_t *gray, uint8_t *bitmap)
{
  const int N = COVER_SIZE * COVER_SIZE;

  // 1. Hitung brightness rata-rata (mean) persis seperti PIL ImageEnhance.Contrast
  uint32_t sum = 0;
  for (int i = 0; i < N; i++) sum += gray[i];
  int mean = (int)((sum + N / 2) / N);

  // 2. Contrast enhancement faktor 1.25 (mean-centered)
  // v = mean + 1.25 * (gray[i] - mean) = mean + ((gray[i] - mean) * 5) / 4
  for (int i = 0; i < N; i++) {
    int v = mean + (((int)gray[i] - mean) * 5) / 4;
    if (v < 0) v = 0;
    else if (v > 255) v = 255;
    gray[i] = (uint8_t)v;
  }

  // 3. Floyd-Steinberg dithering (bit 1 = hitam di GxEPD / SSD1680)
  static int16_t errA[COVER_SIZE + 2], errB[COVER_SIZE + 2];
  int16_t *cur = errA, *nxt = errB;
  memset(errA, 0, sizeof(errA));
  memset(bitmap, 0, N / 8);

  for (int y = 0; y < COVER_SIZE; y++) {
    memset(nxt, 0, sizeof(errA));
    for (int x = 0; x < COVER_SIZE; x++) {
      int v = gray[y * COVER_SIZE + x] + cur[x + 1];
      int nv = v >= 128 ? 255 : 0;
      int e = v - nv;
      if (nv == 0) bitmap[(y * COVER_SIZE + x) >> 3] |= (0x80 >> (x & 7)); // 1 = hitam
      cur[x + 2] += (e * 7) / 16;
      nxt[x]     += (e * 3) / 16;
      nxt[x + 1] += (e * 5) / 16;
      nxt[x + 2] += (e * 1) / 16;
    }
    int16_t *t = cur; cur = nxt; nxt = t;
  }
}

bool artworkDownload(const char *url, uint8_t *coverBitmap)
{
  if (!url || !url[0]) return false;
  unsigned long t0 = millis();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[Art] HTTP %d\n", code);
    http.end();
    return false;
  }

  int len = http.getSize();
  size_t cap = (len > 0 && len <= 90000) ? (size_t)len : 70000;
  uint8_t *jpg = (uint8_t *)malloc(cap);
  if (!jpg) {
    Serial.printf("[Art] malloc %u gagal (heap %u, blok %u)\n", cap,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    http.end();
    return false;
  }

  WiFiClient *s = http.getStreamPtr();
  size_t got = 0;
  unsigned long last = millis();
  while (got < cap && (len <= 0 || got < (size_t)len)) {
    size_t a = s->available();
    if (a) {
      got += s->readBytes(jpg + got, min(a, cap - got));
      last = millis();
    } else {
      if (!s->connected() || millis() - last > 5000) break;
      delay(1);
    }
  }
  http.end();
  client.stop(); // bebaskan RAM TLS sebelum decode

  if (got < 500) {
    Serial.printf("[Art] data terlalu kecil (%u)\n", got);
    free(jpg);
    return false;
  }

  JPEGDEC *jpeg = new (std::nothrow) JPEGDEC();
  if (!jpeg) { free(jpg); return false; }

  bool ok = false;
  if (jpeg->openRAM(jpg, got, jpegDraw)) {
    int w = jpeg->getWidth(), h = jpeg->getHeight();
    int scale = 0, div = 1;
    if (w >= 480)      { scale = JPEG_SCALE_QUARTER; div = 4; }
    else if (w >= 200) { scale = JPEG_SCALE_HALF;    div = 2; }
    outW = (w + div - 1) / div;
    outH = (h + div - 1) / div;
    decoded = (uint8_t *)malloc(outW * outH);
    if (decoded) {
      memset(decoded, 255, outW * outH);
      jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
      if (jpeg->decode(0, 0, scale)) {
        resizeTo96(decoded, outW, outH, grayBuf);
        ditherToMono(grayBuf, coverBitmap);
        ok = true;
      } else {
        Serial.printf("[Art] decode gagal (%d)\n", jpeg->getLastError());
      }
      free(decoded);
      decoded = nullptr;
    }
    jpeg->close();
  } else {
    Serial.printf("[Art] JPEG tidak valid (%d)\n", jpeg->getLastError());
  }
  delete jpeg;
  free(jpg);

  Serial.printf("[Art] %s dalam %lu ms (%u bytes)\n", ok ? "OK" : "GAGAL", millis() - t0, got);
  return ok;
}
