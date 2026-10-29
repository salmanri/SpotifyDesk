// artwork.cpp — Album art downloader & HD dithering pipeline untuk ESP32
//
// FITUR KUALITAS TINGGI (HD E-Paper Rendering):
// 1. Direct Full-Resolution 1:1 Streaming Area Averaging (tanpa blur JPEG_SCALE_HALF)
// 2. Adaptive Soft Auto-Levels (Percentile Stretch) & Mean-Centered Contrast
// 3. 3x3 Unsharp Mask (Edge Sharpening) agar teks & logo band tajam di 96x96
// 4. Highlight/Shadow Clean-Clipping (background terang bersih dari bintik hitam)
// 5. Serpentine Floyd-Steinberg Dithering (bebas artefak garis diagonal / worm)

#include "artwork.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <new>
#include <string.h>

static uint8_t grayBuf[COVER_SIZE * COVER_SIZE];
static uint32_t *accBuf = nullptr;
static int srcW = 0, srcH = 0;
static int cropSide = 0, cropOx = 0, cropOy = 0;

// Callback JPEGDEC: langsung mengakumulasi piksel resolusi penuh 1:1 (300x300)
// ke grid 96x96 dengan bobot sub-piksel eksak (hanya butuh 36 KB heap, bukan 90 KB)
static int jpegDraw(JPEGDRAW *d)
{
  if (!accBuf || cropSide <= 0) return 0;

  for (int y = 0; y < d->iHeight; y++) {
    int sy = d->y + y;
    if (sy < cropOy || sy >= cropOy + cropSide) continue;

    uint32_t uY = (uint32_t)(sy - cropOy) * COVER_SIZE;
    uint32_t uYEnd = uY + COVER_SIZE;
    const uint16_t *row = d->pPixels + y * d->iWidth;

    for (int x = 0; x < d->iWidth; x++) {
      int sx = d->x + x;
      if (sx < cropOx || sx >= cropOx + cropSide) continue;

      uint16_t px = row[x];
      // Konversi RGB565 ke RGB888
      uint32_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
      r = (r << 3) | (r >> 2);
      g = (g << 2) | (g >> 4);
      b = (b << 3) | (b >> 2);
      // Luminance BT.601 (identik PIL Image.convert('L'))
      uint32_t lum = (r * 77 + g * 150 + b * 29) >> 8;

      uint32_t uX = (uint32_t)(sx - cropOx) * COVER_SIZE;
      uint32_t uXEnd = uX + COVER_SIZE;

      uint32_t yCur = uY;
      while (yCur < uYEnd) {
        uint32_t ty = yCur / (uint32_t)cropSide;
        uint32_t yNext = (ty + 1) * (uint32_t)cropSide;
        if (yNext > uYEnd) yNext = uYEnd;
        uint32_t wy = yNext - yCur;

        uint32_t xCur = uX;
        uint32_t *accRow = accBuf + ty * COVER_SIZE;
        while (xCur < uXEnd) {
          uint32_t tx = xCur / (uint32_t)cropSide;
          uint32_t xNext = (tx + 1) * (uint32_t)cropSide;
          if (xNext > uXEnd) xNext = uXEnd;
          uint32_t wx = xNext - xCur;

          accRow[tx] += lum * wx * wy;
          xCur = xNext;
        }
        yCur = yNext;
      }
    }
  }
  return 1;
}

static void enhanceAndDither(uint8_t *gray, uint8_t *tmpBuf, uint8_t *bitmap)
{
  const int N = COVER_SIZE * COVER_SIZE;

  // 1. Hitung histogram untuk Adaptive Soft Auto-Levels (1.5% - 98.5% percentile)
  uint16_t hist[256] = {0};
  for (int i = 0; i < N; i++) hist[gray[i]]++;

  int pLow = 0, pHigh = 255;
  int acc = 0;
  for (int i = 0; i < 256; i++) {
    acc += hist[i];
    if (acc >= (N * 15) / 1000) { pLow = i; break; }
  }
  acc = 0;
  for (int i = 255; i >= 0; i--) {
    acc += hist[i];
    if (acc >= (N * 15) / 1000) { pHigh = i; break; }
  }

  // Blend 50% dengan rentang penuh [0, 255] agar cover low-key tidak kehilangan nuansa
  int blackPt = pLow / 2;
  int whitePt = 255 - (255 - pHigh) / 2;
  if (whitePt - blackPt < 100) {
    blackPt = 0;
    whitePt = 255;
  }

  uint32_t sum = 0;
  int span = whitePt - blackPt;
  for (int i = 0; i < N; i++) {
    int v = ((int)gray[i] - blackPt) * 255 / span;
    if (v < 0) v = 0;
    else if (v > 255) v = 255;
    gray[i] = (uint8_t)v;
    sum += (uint32_t)v;
  }

  // 2. Mean-centered contrast boost (1.25x)
  int mean = (int)((sum + N / 2) / N);
  for (int i = 0; i < N; i++) {
    int v = mean + (((int)gray[i] - mean) * 5) / 4;
    if (v < 0) v = 0;
    else if (v > 255) v = 255;
    tmpBuf[i] = (uint8_t)v;
  }

  // 3. 3x3 Unsharp Mask (Edge Sharpening) + Highlight/Shadow Clean-Clipping
  for (int y = 0; y < COVER_SIZE; y++) {
    int ym1 = (y > 0) ? (y - 1) : 0;
    int yp1 = (y + 1 < COVER_SIZE) ? (y + 1) : (COVER_SIZE - 1);
    for (int x = 0; x < COVER_SIZE; x++) {
      int xm1 = (x > 0) ? (x - 1) : 0;
      int xp1 = (x + 1 < COVER_SIZE) ? (x + 1) : (COVER_SIZE - 1);

      int c = tmpBuf[y * COVER_SIZE + x];
      int blur = (4 * c +
                  2 * (tmpBuf[ym1 * COVER_SIZE + x] +
                       tmpBuf[yp1 * COVER_SIZE + x] +
                       tmpBuf[y * COVER_SIZE + xm1] +
                       tmpBuf[y * COVER_SIZE + xp1]) +
                  tmpBuf[ym1 * COVER_SIZE + xm1] +
                  tmpBuf[ym1 * COVER_SIZE + xp1] +
                  tmpBuf[yp1 * COVER_SIZE + xm1] +
                  tmpBuf[yp1 * COVER_SIZE + xp1] + 8) >> 4;

      // Ketajaman 0.625x (5/8) untuk mempertegas teks & garis logo tanpa halo berlebih
      int v = c + ((c - blur) * 5) / 8;

      // Clean-clip area sangat terang & sangat gelap agar putih bersih bebas bintik
      if (v >= 244) v = 255;
      else if (v <= 10) v = 0;

      gray[y * COVER_SIZE + x] = (uint8_t)v;
    }
  }

  // 4. Serpentine Floyd-Steinberg Dithering (bolak-balik tiap baris, bebas garis cacing/worm)
  static int16_t errA[COVER_SIZE + 2], errB[COVER_SIZE + 2];
  int16_t *cur = errA, *nxt = errB;
  memset(errA, 0, sizeof(errA));
  memset(bitmap, 0, N / 8);

  for (int y = 0; y < COVER_SIZE; y++) {
    memset(nxt, 0, sizeof(errA));
    if ((y & 1) == 0) {
      // Baris genap: kiri ke kanan
      for (int x = 0; x < COVER_SIZE; x++) {
        int v = gray[y * COVER_SIZE + x] + cur[x + 1];
        int nv = (v >= 128) ? 255 : 0;
        int e = v - nv;
        if (nv == 0) bitmap[(y * COVER_SIZE + x) >> 3] |= (0x80 >> (x & 7));
        cur[x + 2] += (e * 7) / 16;
        nxt[x]     += (e * 3) / 16;
        nxt[x + 1] += (e * 5) / 16;
        nxt[x + 2] += (e * 1) / 16;
      }
    } else {
      // Baris ganjil: kanan ke kiri (serpentine)
      for (int x = COVER_SIZE - 1; x >= 0; x--) {
        int v = gray[y * COVER_SIZE + x] + cur[x + 1];
        int nv = (v >= 128) ? 255 : 0;
        int e = v - nv;
        if (nv == 0) bitmap[(y * COVER_SIZE + x) >> 3] |= (0x80 >> (x & 7));
        cur[x]     += (e * 7) / 16;
        nxt[x + 2] += (e * 3) / 16;
        nxt[x + 1] += (e * 5) / 16;
        nxt[x]     += (e * 1) / 16;
      }
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
  client.stop(); // bebaskan ~40KB RAM TLS sebelum decode

  if (got < 500) {
    Serial.printf("[Art] data terlalu kecil (%u)\n", got);
    free(jpg);
    return false;
  }

  JPEGDEC *jpeg = new (std::nothrow) JPEGDEC();
  if (!jpeg) { free(jpg); return false; }

  bool ok = false;
  if (jpeg->openRAM(jpg, got, jpegDraw)) {
    srcW = jpeg->getWidth();
    srcH = jpeg->getHeight();
    cropSide = min(srcW, srcH);
    cropOx = (srcW - cropSide) / 2;
    cropOy = (srcH - cropSide) / 2;

    accBuf = (uint32_t *)calloc(COVER_SIZE * COVER_SIZE, sizeof(uint32_t));
    if (accBuf && cropSide >= COVER_SIZE) {
      jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
      // Decode pada skala 0 (resolusi penuh 1:1, tanpa blur JPEG_SCALE_HALF)
      if (jpeg->decode(0, 0, 0)) {
        uint32_t totalW = (uint32_t)cropSide * (uint32_t)cropSide;
        for (int i = 0; i < COVER_SIZE * COVER_SIZE; i++) {
          grayBuf[i] = (uint8_t)((accBuf[i] + (totalW >> 1)) / totalW);
        }
        // Gunakan byte awal accBuf sebagai buffer sementara untuk unsharp mask
        enhanceAndDither(grayBuf, (uint8_t *)accBuf, coverBitmap);
        ok = true;
      } else {
        Serial.printf("[Art] decode gagal (%d)\n", jpeg->getLastError());
      }
      free(accBuf);
      accBuf = nullptr;
    } else if (accBuf) {
      free(accBuf);
      accBuf = nullptr;
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
