#include "filter.h"
#include <palette.h>
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

  static const int bayer16x16[16][16] = {
      {0, 128, 32, 160, 8, 136, 40, 168, 2, 130, 34, 162, 10, 138, 42, 170},
      {192, 64, 224, 96, 200, 72, 232, 104, 194, 66, 226, 98, 202, 74, 234, 106},
      {48, 176, 16, 144, 56, 184, 24, 152, 50, 178, 18, 146, 58, 186, 26, 154},
      {240, 112, 208, 80, 248, 120, 216, 88, 242, 114, 210, 82, 250, 122, 218, 90},
      {12, 140, 44, 172, 4, 132, 36, 164, 14, 142, 46, 174, 6, 134, 38, 166},
      {204, 76, 236, 108, 196, 68, 228, 100, 206, 78, 238, 110, 198, 70, 230, 102},
      {60, 188, 28, 156, 52, 180, 20, 148, 62, 190, 30, 158, 54, 182, 22, 150},
      {252, 124, 220, 92, 244, 116, 212, 84, 254, 126, 222, 94, 246, 118, 214, 86},
      {3, 131, 35, 163, 11, 139, 43, 171, 1, 129, 33, 161, 9, 137, 41, 169},
      {195, 67, 227, 99, 203, 75, 235, 107, 193, 65, 225, 97, 201, 73, 233, 105},
      {51, 179, 19, 147, 59, 187, 27, 155, 49, 177, 17, 145, 57, 185, 25, 153},
      {243, 115, 211, 83, 251, 123, 219, 91, 241, 113, 209, 81, 249, 121, 217, 89},
      {15, 143, 47, 175, 7, 135, 39, 167, 13, 141, 45, 173, 5, 133, 37, 165},
      {207, 79, 239, 111, 199, 71, 231, 103, 205, 77, 237, 109, 197, 69, 229, 101},
      {63, 191, 31, 159, 55, 183, 23, 151, 61, 189, 29, 157, 53, 181, 21, 149},
      {255, 127, 223, 95, 247, 119, 215, 87, 253, 125, 221, 93, 245, 117, 213, 85}};

  if (bayerSize != 2 && bayerSize != 4 && bayerSize != 8 && bayerSize != 16)
    bayerSize = 16;
  const int bayerDivisor = (bayerSize == 2) ? 4 : (bayerSize == 4) ? 16
                                              : (bayerSize == 8)   ? 64
                                                                   : 256;

  // Match the Bayer response used in the camera pipeline: centered-bin normalization
  // with a reduced amplitude and slight dark bias.
  int bayerIntOffsets[16][16] = {};
  const int bayerStrengthPct = 70;
  const int bayerBias = -6;
  for (int by = 0; by < bayerSize; ++by)
  {
    for (int bx = 0; bx < bayerSize; ++bx)
    {
      int bv = (bayerSize == 2)   ? bayer2x2[by][bx]
               : (bayerSize == 4) ? bayer4x4[by][bx]
               : (bayerSize == 8) ? bayer8x8[by][bx]
                                  : bayer16x16[by][bx];
      int baseOffset = (((2 * bv + 1) * 255) / (2 * bayerDivisor)) - 127;
      bayerIntOffsets[by][bx] = (baseOffset * bayerStrengthPct) / 100 + bayerBias;
    }
  }

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

      int bayerX = x % bayerSize;
      int bayerY = y % bayerSize;
      int bayerOffset = bayerIntOffsets[bayerY][bayerX];
      r = clamp8(int(r) + bayerOffset);
      g = clamp8(int(g) + bayerOffset);
      b = clamp8(int(b) + bayerOffset);

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
  applyColorPalette(reinterpret_cast<uint16_t *>(cameraFb->buf), cameraFb->width, cameraFb->height, PICO_PALETTE, 16, 16);
}

void applyElevatePalette(camera_fb_t *cameraFb)
{
  if (!cameraFb || !cameraFb->buf)
    return;
  applyColorPalette(reinterpret_cast<uint16_t *>(cameraFb->buf), cameraFb->width, cameraFb->height, PALETTE_ELEVATE, PALETTE_ELEVATE_SIZE, 16);
}
