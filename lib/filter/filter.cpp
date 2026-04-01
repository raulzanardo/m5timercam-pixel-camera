#include "filter.h"
#include <palette_pico.h>
#include <math.h>
#include <limits.h>

// Apply a palette with ordered Bayer dithering (default 2x2)
void applyColorPalette(uint16_t *imageBuffer, int width, int height, const uint32_t *palette, int paletteSize, int bayerSize)
{
  if (!imageBuffer || width <= 0 || height <= 0 || !palette || paletteSize <= 0)
    return;

  // OV3660: RGB565 arrives byte-swapped in RAM
  const bool swapBytes = true;

  // Bayer matrices
  static const int bayer2x2[2][2] = {{0, 2}, {3, 1}};
  static const int bayer4x4[4][4] = {
      {0, 8, 2, 10},
      {12, 4, 14, 6},
      {3, 11, 1, 9},
      {15, 7, 13, 5}};
  static const int bayer8x8[8][8] = {
      {0, 32, 8, 40, 2, 34, 10, 42},
      {48, 16, 56, 24, 50, 18, 58, 26},
      {12, 44, 4, 36, 14, 46, 6, 38},
      {60, 28, 52, 20, 62, 30, 54, 22},
      {3, 35, 11, 43, 1, 33, 9, 41},
      {51, 19, 59, 27, 49, 17, 57, 25},
      {15, 47, 7, 39, 13, 45, 5, 37},
      {63, 31, 55, 23, 61, 29, 53, 21}};

  if (bayerSize != 2 && bayerSize != 4 && bayerSize != 8)
    bayerSize = 2;
  const int divisor = (bayerSize == 2) ? 4 : (bayerSize == 4) ? 16
                                                              : 64;

  auto bayerValue = [&](int x, int y)
  {
    x %= bayerSize;
    y %= bayerSize;
    if (bayerSize == 2)
      return bayer2x2[y][x];
    if (bayerSize == 4)
      return bayer4x4[y][x];
    return bayer8x8[y][x];
  };

  auto dist2 = [](uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2)
  {
    int dr = int(r1) - int(r2);
    int dg = int(g1) - int(g2);
    int db = int(b1) - int(b2);
    return dr * dr + dg * dg + db * db;
  };

  auto clamp8 = [](int v)
  {
    if (v < 0)
      return 0;
    if (v > 255)
      return 255;
    return v;
  };

  for (int y = 0; y < height; ++y)
  {
    for (int x = 0; x < width; ++x)
    {
      uint16_t pix = imageBuffer[y * width + x];
      if (swapBytes)
        pix = uint16_t((pix << 8) | (pix >> 8));

      uint8_t r = ((pix >> 11) & 0x1F) << 3;
      uint8_t g = ((pix >> 5) & 0x3F) << 2;
      uint8_t b = (pix & 0x1F) << 3;

      int bv = bayerValue(x, y);
      int offset = int(((bv + 0.5f) * 255.0f) / divisor - 127.5f);
      r = clamp8(int(r) + offset);
      g = clamp8(int(g) + offset);
      b = clamp8(int(b) + offset);

      int bestIdx = 0;
      int bestDist = INT32_MAX;
      for (int i = 0; i < paletteSize; ++i)
      {
        uint32_t c = palette[i];
        uint8_t pr = (c >> 16) & 0xFF;
        uint8_t pg = (c >> 8) & 0xFF;
        uint8_t pb = c & 0xFF;
        int d = dist2(r, g, b, pr, pg, pb);
        if (d < bestDist)
        {
          bestDist = d;
          bestIdx = i;
        }
      }

      uint32_t chosen = palette[bestIdx];
      uint8_t cr = (chosen >> 16) & 0xFF;
      uint8_t cg = (chosen >> 8) & 0xFF;
      uint8_t cb = chosen & 0xFF;
      uint16_t out = uint16_t(((cr >> 3) << 11) | ((cg >> 2) << 5) | (cb >> 3));
      if (swapBytes)
        out = uint16_t((out << 8) | (out >> 8));
      imageBuffer[y * width + x] = out;
    }
  }
}

// Auto adjust brightness/contrast/gamma on RGB565 framebuffer
void applyAutoAdjust(camera_fb_t *cameraFb)
{
  if (!cameraFb || !cameraFb->buf)
    return;

  const bool swapBytes = true; // OV3660
  uint16_t *buf = reinterpret_cast<uint16_t *>(cameraFb->buf);
  int total = cameraFb->width * cameraFb->height;

  int hist[256] = {0};
  for (int i = 0; i < total; ++i)
  {
    uint16_t pix = buf[i];
    if (swapBytes)
      pix = uint16_t((pix << 8) | (pix >> 8));
    uint8_t r = ((pix >> 11) & 0x1F) << 3;
    uint8_t g = ((pix >> 5) & 0x3F) << 2;
    uint8_t b = (pix & 0x1F) << 3;
    uint8_t lum = uint8_t((r * 30 + g * 59 + b * 11) / 100);
    hist[lum]++;
  }

  int thresh1 = total / 100;       // 1%
  int thresh99 = total * 99 / 100; // 99%
  int acc = 0;
  int minVal = 0, maxVal = 255;
  for (int i = 0; i < 256; ++i)
  {
    acc += hist[i];
    if (acc >= thresh1 && minVal == 0)
      minVal = i;
    if (acc >= thresh99)
    {
      maxVal = i;
      break;
    }
  }
  if (maxVal <= minVal)
    maxVal = minVal + 1;

  float contrast = 255.0f / float(maxVal - minVal);
  float brightness = -minVal * contrast;
  float gamma = 1.0f;

  auto clamp8 = [](int v)
  {
    if (v < 0)
      return 0;
    if (v > 255)
      return 255;
    return v;
  };

  for (int i = 0; i < total; ++i)
  {
    uint16_t pix = buf[i];
    if (swapBytes)
      pix = uint16_t((pix << 8) | (pix >> 8));

    float r = float(((pix >> 11) & 0x1F) << 3);
    float g = float(((pix >> 5) & 0x3F) << 2);
    float b = float((pix & 0x1F) << 3);

    auto adjust = [&](float c)
    {
      c = c * contrast + brightness;
      c = clamp8(int(c));
      c = powf(c / 255.0f, gamma) * 255.0f;
      return clamp8(int(c));
    };

    int r8 = adjust(r);
    int g8 = adjust(g);
    int b8 = adjust(b);

    uint16_t out = uint16_t(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
    if (swapBytes)
      out = uint16_t((out << 8) | (out >> 8));
    buf[i] = out;
  }
}

void applyPicoPalette(camera_fb_t *cameraFb)
{
  if (!cameraFb || !cameraFb->buf)
    return;
  applyColorPalette(reinterpret_cast<uint16_t *>(cameraFb->buf), cameraFb->width, cameraFb->height, PICO_PALETTE, 16, 2);
}
