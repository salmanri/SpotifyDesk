// artwork.h — Download cover JPEG -> 96x96 1-bit dithered bitmap
#pragma once
#include <Arduino.h>
#include "display.h"

bool artworkDownload(const char *url, uint8_t *coverBitmap);
