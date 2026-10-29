// artwork.cpp — Album art downloader & Clean HD Dithering untuk ESP32
//
// FITUR UTAMA:
// 1. Zero-Heap-Fragmentation Accumulator (accSum & accCnt statis di RAM, anti gagal malloc)
// 2. Auto-Retry Download + Buffer Shrink (realloc) agar gambar selalu berhasil di-render
// 3. Full-Resolution 1:1 2x2 Supersampling Downscale dari 300x300 ke 96x96
// 4. 3x3 Edge-Preserving Bilateral Denoise (membersihkan noise/bintik pada background)
// 5. S-Curve Contrast + Edge Sharpening + Clean Clipping (putih bersih & hitam pekat)
// 6. Serpentine Hybrid Atkinson-FS Dithering (13/16 diffusion: sangat bersih & tajam)

#include "artwork.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <new>
#include <string.h>

static uint8_t grayBuf[COVER_SIZE * COVER_SIZE];
static uint8_t workBuf[COVER_SIZE * COVER_SIZE];
static uint16_t accSum[COVER_SIZE * COVER_SIZE];
static uint8_t accCnt[COVER_SIZE * COVER_SIZE];

static int srcW = 0, srcH = 0;
static int cropSide = 0, cropOx = 0, cropOy = 0;

// Callback JPEGDEC: 2x2 supersampling langsung dari resolusi penuh ke 96x96
// Tanpa alokasi heap tambahan sehingga tidak pernah gagal karena kehabisan memori
static int jpegDraw(JPEGDRAW *d)
{
  if (cropSide <= 0) return 0;
  const uint32_t side2 = (uint32_t)cropSide * 2;

  for (int y = 0; y < d->iHeight; y++) {
    int sy = d->y + y;
    if (sy < cropOy || sy >= cropOy + cropSide) continue;

    uint32_t uY = (uint32_t)(sy - cropOy) * 2;
    uint32_t ty0 = (uY * COVER_SIZE) / side2;
    uint32_t ty1 = ((uY + 1) * COVER_SIZE) / side2;
    if (ty0 >= COVER_SIZE) ty0 = COVER_SIZE - 1;
    if (ty1 >= COVER_SIZE) ty1 = COVER_SIZE - 1;

    const uint16_t *row = d->pPixels + y * d->iWidth;

    for (int x = 0; x < d->iWidth; x++) {
      int sx = d->x + x;
      if (sx < cropOx || sx >= cropOx + cropSide) continue;

      uint16_t px = row[x];
      uint32_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
      r = (r << 3) | (r >> 2);
      g = (g << 2) | (g >> 4);
      b = (b << 3) | (b >> 2);
      uint16_t lum = (uint16_t)((r * 77 + g * 150 + b * 29) >> 8);

      uint32_t uX = (uint32_t)(sx - cropOx) * 2;
      uint32_t tx0 = (uX * COVER_SIZE) / side2;
      uint32_t tx1 = ((uX + 1) * COVER_SIZE) / side2;
      if (tx0 >= COVER_SIZE) tx0 = COVER_SIZE - 1;
      if (tx1 >= COVER_SIZE) tx1 = COVER_SIZE - 1;

      uint32_t i00 = ty0 * COVER_SIZE + tx0;
      uint32_t i01 = ty0 * COVER_SIZE + tx1;
      uint32_t i10 = ty1 * COVER_SIZE + tx0;
      uint32_t i11 = ty1 * COVER_SIZE + tx1;

      if (accCnt[i00] < 250) { accSum[i00] += lum; accCnt[i00]++; }
      if (accCnt[i01] < 250) { accSum[i01] += lum; accCnt[i01]++; }
      if (accCnt[i10] < 250) { accSum[i10] += lum; accCnt[i10]++; }
      if (accCnt[i11] < 250) { accSum[i11] += lum; accCnt[i11]++; }
    }
  }
  return 1;
}

static void enhanceAndDither(uint8_t *gray, uint8_t *tmp, uint8_t *bitmap)
{
  const int N = COVER_SIZE * COVER_SIZE;

  // 1. Adaptive Percentile Auto-Levels (2% - 98%) agar kontras gambar selalu optimal
  uint16_t hist[256] = {0};
  for (int i = 0; i < N; i++) hist[gray[i]]++;

  int pLow = 0, pHigh = 255;
  int acc = 0;
  for (int i = 0; i < 256; i++) {
    acc += hist[i];
    if (acc >= (N * 20) / 1000) { pLow = i; break; }
  }
  acc = 0;
  for (int i = 255; i >= 0; i--) {
    acc += hist[i];
    if (acc >= (N * 20) / 1000) { pHigh = i; break; }
  }

  int blackPt = (pLow * 2) / 3;
  int whitePt = 255 - ((255 - pHigh) * 2) / 3;
  if (whitePt - blackPt < 90) {
    blackPt = 0;
    whitePt = 255;
  }

  int span = whitePt - blackPt;
  for (int i = 0; i < N; i++) {
    int v = ((int)gray[i] - blackPt) * 255 / span;
    if (v < 0) v = 0;
    else if (v > 255) v = 255;
    gray[i] = (uint8_t)v;
  }

  // 2. 3x3 Edge-Preserving Bilateral Denoise:
  // Meratakan noise/bintik pada permukaan rata (selisih <= 18) tanpa memblur garis tepi (> 18)
  for (int y = 0; y < COVER_SIZE; y++) {
    int ym1 = (y > 0) ? (y - 1) : 0;
    int yp1 = (y + 1 < COVER_SIZE) ? (y + 1) : (COVER_SIZE - 1);
    for (int x = 0; x < COVER_SIZE; x++) {
      int xm1 = (x > 0) ? (x - 1) : 0;
      int xp1 = (x + 1 < COVER_SIZE) ? (x + 1) : (COVER_SIZE - 1);

      int c = gray[y * COVER_SIZE + x];
      int sumW = 4;
      int sumV = c * 4;

      const int nb[8] = {
        gray[ym1 * COVER_SIZE + x],   gray[yp1 * COVER_SIZE + x],
        gray[y * COVER_SIZE + xm1],   gray[y * COVER_SIZE + xp1],
        gray[ym1 * COVER_SIZE + xm1], gray[ym1 * COVER_SIZE + xp1],
        gray[yp1 * COVER_SIZE + xm1], gray[yp1 * COVER_SIZE + xp1]
      };
      for (int k = 0; k < 8; k++) {
        int diff = abs(nb[k] - c);
        if (diff <= 18) {
          int w = (k < 4) ? 2 : 1;
          sumV += nb[k] * w;
          sumW += w;
        }
      }
      tmp[y * COVER_SIZE + x] = (uint8_t)((sumV + (sumW >> 1)) / sumW);
    }
  }

  // 3. Edge Sharpening (Unsharp Mask pada tepi nyata) + S-Curve Contrast + Clean Clipping
  uint32_t totalLum = 0;
  for (int i = 0; i < N; i++) totalLum += tmp[i];
  int mean = (int)((totalLum + N / 2) / N);
  // Jaga titik tengah S-curve di rentang wajar (105..150)
  if (mean < 105) mean = 105;
  else if (mean > 150) mean = 150;

  for (int y = 0; y < COVER_SIZE; y++) {
    int ym1 = (y > 0) ? (y - 1) : 0;
    int yp1 = (y + 1 < COVER_SIZE) ? (y + 1) : (COVER_SIZE - 1);
    for (int x = 0; x < COVER_SIZE; x++) {
      int xm1 = (x > 0) ? (x - 1) : 0;
      int xp1 = (x + 1 < COVER_SIZE) ? (x + 1) : (COVER_SIZE - 1);

      int c = tmp[y * COVER_SIZE + x];
      int blur = (4 * c +
                  2 * (tmp[ym1 * COVER_SIZE + x] +
                       tmp[yp1 * COVER_SIZE + x] +
                       tmp[y * COVER_SIZE + xm1] +
                       tmp[y * COVER_SIZE + xp1]) +
                  tmp[ym1 * COVER_SIZE + xm1] +
                  tmp[ym1 * COVER_SIZE + xp1] +
                  tmp[yp1 * COVER_SIZE + xm1] +
                  tmp[yp1 * COVER_SIZE + xp1] + 8) >> 4;

      int detail = c - blur;
      // Hanya pertajam jika benar-benar garis/teks (bukan bintik halus)
      int v = (abs(detail) >= 5) ? (c + (detail * 7) / 8) : c;

      // Kontras 1.30x berpusat pada mean
      v = mean + ((v - mean) * 13) / 10;

      // Clean-Clip agresif pada highlight & shadow agar background putih bersih tanpa titik-titik kotor
      // dan area gelap menjadi hitam solid yang tegas
      if (v >= 226) v = 255;
      else if (v <= 24) v = 0;

      gray[y * COVER_SIZE + x] = (uint8_t)v;
    }
  }

  // 4. Serpentine Hybrid Atkinson-FS Dithering (13/16 error diffusion):
  // Menyebarkan 13/16 error (bukan 16/16 penuh), sehingga tepi teks/logo jauh lebih kontras,
  // area putih di sekitar objek tetap bersih, dan bebas artefak garis cacing (worm)
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
        cur[x + 2] += (e * 6) / 16;
        nxt[x]     += (e * 2) / 16;
        nxt[x + 1] += (e * 4) / 16;
        nxt[x + 2] += (e * 1) / 16;
      }
    } else {
      // Baris ganjil: kanan ke kiri (serpentine)
      for (int x = COVER_SIZE - 1; x >= 0; x--) {
        int v = gray[y * COVER_SIZE + x] + cur[x + 1];
        int nv = (v >= 128) ? 255 : 0;
        int e = v - nv;
        if (nv == 0) bitmap[(y * COVER_SIZE + x) >> 3] |= (0x80 >> (x & 7));
        cur[x]     += (e * 6) / 16;
        nxt[x + 2] += (e * 2) / 16;
        nxt[x + 1] += (e * 4) / 16;
        nxt[x]     += (e * 1) / 16;
      }
    }
    int16_t *t = cur; cur = nxt; nxt = t;
  }
}

static bool downloadOnce(const char *url, uint8_t *coverBitmap)
{
  unsigned long t0 = millis();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
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
  // Alokasi ukuran tepat sesuai Content-Length, atau bertahap sesuai blok memori terbesar
  size_t maxBlock = ESP.getMaxAllocHeap();
  size_t cap = (len > 0 && len <= 95000) ? (size_t)len : 55000;
  if (cap > maxBlock - 1024 && maxBlock > 16000) {
    cap = maxBlock - 1024;
  }

  uint8_t *jpg = (uint8_t *)malloc(cap);
  if (!jpg) {
    Serial.printf("[Art] malloc %u gagal (heap %u, blok %u)\n",
                  (unsigned)cap, ESP.getFreeHeap(), (unsigned)maxBlock);
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
  client.stop(); // bebaskan ~45KB RAM TLS sebelum alokasi JPEGDEC

  if (got < 500 || (len > 0 && got < (size_t)len)) {
    Serial.printf("[Art] unduhan tidak lengkap (%u / %d bytes)\n", (unsigned)got, len);
    free(jpg);
    return false;
  }

  // Kecilkan blok jpg sesuai ukuran asli agar tidak memfragmentasi heap
  if (got < cap) {
    uint8_t *shrunk = (uint8_t *)realloc(jpg, got);
    if (shrunk) jpg = shrunk;
  }

  JPEGDEC *jpeg = new (std::nothrow) JPEGDEC();
  if (!jpeg) {
    Serial.println("[Art] alokasi JPEGDEC gagal");
    free(jpg);
    return false;
  }

  bool ok = false;
  if (jpeg->openRAM(jpg, got, jpegDraw)) {
    srcW = jpeg->getWidth();
    srcH = jpeg->getHeight();
    cropSide = min(srcW, srcH);
    cropOx = (srcW - cropSide) / 2;
    cropOy = (srcH - cropSide) / 2;

    memset(accSum, 0, sizeof(accSum));
    memset(accCnt, 0, sizeof(accCnt));

    jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
    if (jpeg->decode(0, 0, 0)) {
      for (int i = 0; i < COVER_SIZE * COVER_SIZE; i++) {
        uint16_t c = accCnt[i];
        grayBuf[i] = (c > 0) ? (uint8_t)((accSum[i] + (c >> 1)) / c) : 255;
      }
      enhanceAndDither(grayBuf, workBuf, coverBitmap);
      ok = true;
    } else {
      Serial.printf("[Art] decode gagal (%d)\n", jpeg->getLastError());
    }
    jpeg->close();
  } else {
    Serial.printf("[Art] JPEG tidak valid (%d)\n", jpeg->getLastError());
  }
  delete jpeg;
  free(jpg);

  Serial.printf("[Art] %s dalam %lu ms (%u bytes)\n", ok ? "OK" : "GAGAL", millis() - t0, (unsigned)got);
  return ok;
}

bool artworkDownload(const char *url, uint8_t *coverBitmap)
{
  if (!url || !url[0]) return false;
  if (downloadOnce(url, coverBitmap)) return true;

  // Jika percobaan pertama gagal (misal timeout jaringan / TLS), coba sekali lagi
  Serial.println("[Art] Mencoba ulang download cover...");
  delay(250);
  return downloadOnce(url, coverBitmap);
}
