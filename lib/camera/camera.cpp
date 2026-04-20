#include "camera.h"

#include <esp_camera.h>
#include <M5TimerCAM.h>
#include <LittleFS.h>

#include "../../include/config.h"
#include "../filter/filter.h"

namespace
{
  constexpr int CAPTURE_GET_FB_RETRIES = 4;
  constexpr int CAPTURE_GET_FB_RETRY_DELAY_MS = 30;

  uint16_t g_xMap[SCREEN_WIDTH];
  uint16_t g_yMap[SCREEN_HEIGHT];
  uint16_t g_cachedSrcWidth = 0;
  uint16_t g_cachedSrcHeight = 0;

  void updateSamplingMap(uint16_t srcWidth, uint16_t srcHeight)
  {
    if (srcWidth == g_cachedSrcWidth && srcHeight == g_cachedSrcHeight)
    {
      return;
    }

    // 16.16 fixed-point mapping to avoid float work in the hot preview loop.
    const uint32_t scaleXfp = (static_cast<uint32_t>(srcWidth) << 16) / SCREEN_WIDTH;
    const uint32_t scaleYfp = (static_cast<uint32_t>(srcHeight) << 16) / SCREEN_HEIGHT;
    const uint32_t scaleFp = (scaleXfp < scaleYfp) ? scaleXfp : scaleYfp;

    const int32_t cropXfp = ((static_cast<int32_t>(srcWidth) << 16) - static_cast<int32_t>(SCREEN_WIDTH * scaleFp)) / 2;
    const int32_t cropYfp = ((static_cast<int32_t>(srcHeight) << 16) - static_cast<int32_t>(SCREEN_HEIGHT * scaleFp)) / 2;

    for (int x = 0; x < SCREEN_WIDTH; ++x)
    {
      int32_t camX = (cropXfp + static_cast<int32_t>(x) * static_cast<int32_t>(scaleFp)) >> 16;
      if (camX < 0)
        camX = 0;
      if (camX >= srcWidth)
        camX = srcWidth - 1;
      g_xMap[x] = static_cast<uint16_t>(camX);
    }

    for (int y = 0; y < SCREEN_HEIGHT; ++y)
    {
      int32_t camY = (cropYfp + static_cast<int32_t>(y) * static_cast<int32_t>(scaleFp)) >> 16;
      if (camY < 0)
        camY = 0;
      if (camY >= srcHeight)
        camY = srcHeight - 1;
      g_yMap[y] = static_cast<uint16_t>(camY);
    }

    g_cachedSrcWidth = srcWidth;
    g_cachedSrcHeight = srcHeight;
  }

  bool initCamera(pixformat_t fmt, framesize_t size, uint32_t xclkHz = XCLK_FREQ_HZ)
  {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = xclkHz;
    config.pixel_format = fmt;
    config.frame_size = size;
    config.jpeg_quality = 0;
    if (fmt == PIXFORMAT_JPEG)
    {
      config.fb_count = 2;
    }
    else
    {
      config.fb_count = (fmt == CAPTURE_PIXEL_FORMAT) ? CAPTURE_FB_COUNT : 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
      Serial.printf("Camera init failed with error 0x%x\n", err);
      return false;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s)
    {
      s->set_vflip(s, 1);
      s->set_whitebal(s, 1);      // enable AWB
      s->set_awb_gain(s, 1);      // enable AWB gain
      s->set_wb_mode(s, 0);       // 0=Auto
      s->set_bpc(s, 1);           // bad pixel correction ON
      s->set_wpc(s, 1);           // white pixel correction ON
      s->set_dcw(s, 1);           // downsize/crop weighting ON
      s->set_raw_gma(s, 1);       // raw gamma ON (more natural tones)
      s->set_exposure_ctrl(s, 1); // enable exposure control (required for auto exposure to work)
      s->set_aec2(s, 1);          // AEC2 ON (nighttime auto exposure algorithm)
      s->set_lenc(s, 1);          // lens correction ON
    }
    return true;
  }

  bool saveJpgToSpiffs(camera_fb_t *fb, bool isAlreadyJpeg, bool littlefsReady, uint32_t &photoCounter, bool preferencesReady, Preferences &preferences)
  {
    if (!littlefsReady || fb == nullptr)
    {
      Serial.println("Cannot save JPG: LittleFS not ready or frame missing");
      return false;
    }

    static bool photosDirReady = false;
    if (!photosDirReady)
    {
      if (!LittleFS.exists("/photos"))
        LittleFS.mkdir("/photos");
      photosDirReady = true;
    }

    char path[48];
    snprintf(path, sizeof(path), "/photos/photo_%lu.jpg", static_cast<unsigned long>(photoCounter));

    File file = LittleFS.open(path, FILE_WRITE);
    if (!file)
    {
      Serial.printf("Failed to create %s\n", path);
      return false;
    }

    if (isAlreadyJpeg)
    {
      file.write(fb->buf, fb->len);
    }
    else
    {
      fmt2jpg_cb(reinterpret_cast<uint8_t *>(fb->buf), fb->len, fb->width, fb->height, PIXFORMAT_RGB565, JPG_ENCODE_QUALITY, [](void *arg, size_t index, const void *data, size_t len) -> size_t
                 {
                     fs::File *f = reinterpret_cast<fs::File *>(arg);
                     return f->write(reinterpret_cast<const uint8_t *>(data), len); }, &file);
    }

    file.close();
    photoCounter++;
    if (preferencesReady)
    {
      preferences.putUInt("photo_idx", photoCounter);
    }
    Serial.printf("Saved JPG to %s (next:%lu)\n", path, static_cast<unsigned long>(photoCounter));
    return true;
  }
} // namespace

namespace CameraService
{
  bool initLive()
  {
    return initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE, XCLK_FREQ_HZ);
  }

  bool capturePhotoToJpg(uint8_t paletteMode,
                         bool littlefsReady,
                         uint32_t &photoCounter,
                         bool preferencesReady,
                         Preferences &preferences)
  {
    const bool filterEnabled = paletteMode != 0;
    esp_camera_deinit();

    // Without a filter we can use hardware JPEG — OV2640 encodes on-sensor,
    // skipping the expensive software fmt2jpg_cb pass.
    const pixformat_t capFmt = filterEnabled ? CAPTURE_PIXEL_FORMAT : PIXFORMAT_JPEG;
    const framesize_t capFrameSize = filterEnabled ? CAPTURE_FRAME_SIZE_FILTER_ON : CAPTURE_FRAME_SIZE_FILTER_OFF;
    const uint32_t capXclkHz = filterEnabled ? CAPTURE_XCLK_FREQ_HZ : XCLK_FREQ_HZ;

    if (!initCamera(capFmt, capFrameSize, capXclkHz))
    {
      Serial.println("Failed to init capture mode");
      initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE, XCLK_FREQ_HZ);
      return false;
    }

    if (!filterEnabled)
    {
      // Set hardware JPEG quality on the sensor
      sensor_t *s = esp_camera_sensor_get();
      if (s)
        s->set_quality(s, CAPTURE_SENSOR_JPEG_QUALITY);
    }

    for (int i = 0; i < CAPTURE_WARMUP_FRAMES; ++i)
    {
      camera_fb_t *warm = esp_camera_fb_get();
      if (warm)
        esp_camera_fb_return(warm);
      delay(CAPTURE_WARMUP_DELAY_MS);
    }

    camera_fb_t *fb = nullptr;
    for (int attempt = 0; attempt < CAPTURE_GET_FB_RETRIES; ++attempt)
    {
      fb = esp_camera_fb_get();
      if (fb)
      {
        break;
      }

      Serial.printf("Photo capture failed (attempt %d/%d)\n", attempt + 1, CAPTURE_GET_FB_RETRIES);
      delay(CAPTURE_GET_FB_RETRY_DELAY_MS);
    }

    if (!fb && !filterEnabled && capFrameSize == CAPTURE_FRAME_SIZE_FILTER_OFF)
    {
      Serial.println("VGA capture unstable, retrying with QVGA fallback");
      esp_camera_deinit();

      if (initCamera(capFmt, CAPTURE_FRAME_SIZE_FILTER_ON, capXclkHz))
      {
        sensor_t *s = esp_camera_sensor_get();
        if (s)
          s->set_quality(s, CAPTURE_SENSOR_JPEG_QUALITY);

        for (int i = 0; i < CAPTURE_WARMUP_FRAMES; ++i)
        {
          camera_fb_t *warm = esp_camera_fb_get();
          if (warm)
            esp_camera_fb_return(warm);
          delay(CAPTURE_WARMUP_DELAY_MS);
        }

        for (int attempt = 0; attempt < CAPTURE_GET_FB_RETRIES; ++attempt)
        {
          fb = esp_camera_fb_get();
          if (fb)
          {
            break;
          }

          Serial.printf("QVGA fallback capture failed (attempt %d/%d)\n", attempt + 1, CAPTURE_GET_FB_RETRIES);
          delay(CAPTURE_GET_FB_RETRY_DELAY_MS);
        }
      }
    }

    if (!fb)
    {
      Serial.println("Photo capture failed");
      esp_camera_deinit();
      initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE, XCLK_FREQ_HZ);
      return false;
    }

    if (filterEnabled)
    {
      applyAutoAdjust(fb);
      if (paletteMode == 2)
      {
        applyElevatePalette(fb);
      }
      else
      {
        applyPicoPalette(fb);
      }
    }

    saveJpgToSpiffs(fb, !filterEnabled, littlefsReady, photoCounter, preferencesReady, preferences);

    esp_camera_fb_return(fb);
    esp_camera_deinit();
    initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE, XCLK_FREQ_HZ);
    return true;
  }

  bool renderLivePreview(U8G2_SSD1306_64X32_1F_F_HW_I2C &display,
                         int16_t *ditherBuffer)
  {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
      Serial.println("Camera capture failed");
      return false;
    }

    display.clearBuffer();

    int16_t (*buffer)[SCREEN_WIDTH] = reinterpret_cast<int16_t (*)[SCREEN_WIDTH]>(ditherBuffer);

    updateSamplingMap(fb->width, fb->height);

    for (int y = 0; y < SCREEN_HEIGHT; y++)
    {
      const int rowBase = static_cast<int>(g_yMap[y]) * fb->width;
      for (int x = 0; x < SCREEN_WIDTH; x++)
      {
        const int pixel_index = rowBase + g_xMap[x];
        buffer[y][x] = fb->buf[pixel_index];
      }
    }

    for (int y = 0; y < SCREEN_HEIGHT; y++)
    {
      for (int x = 0; x < SCREEN_WIDTH; x++)
      {
        const int16_t oldPixel = constrain(static_cast<int>(buffer[y][x]), 0, 255);
        const int16_t newPixel = (oldPixel >= 128) ? 255 : 0;

        if (newPixel == 255)
        {
          display.drawPixel(x, y);
        }

        const int16_t error = oldPixel - newPixel;

        if (x + 1 < SCREEN_WIDTH)
        {
          buffer[y][x + 1] += static_cast<int16_t>(error >> 1);
        }

        if (y + 1 < SCREEN_HEIGHT)
        {
          if (x > 0)
          {
            buffer[y + 1][x - 1] += static_cast<int16_t>(error >> 2);
          }
          buffer[y + 1][x] += static_cast<int16_t>(error >> 2);
        }
      }
    }

    esp_camera_fb_return(fb);
    display.sendBuffer();
    return true;
  }

  void shutdownForSleep()
  {
    sensor_t *s = esp_camera_sensor_get();
    if (s)
    {
      s->set_reg(s, 0x3008, 0xff, 0x01);
      delay(100);
      s->set_reg(s, 0x3008, 0xff, 0x80);
      delay(100);
      s->set_reg(s, 0x3008, 0xff, 0x40);
      delay(100);
    }
    esp_camera_deinit();
  }
} // namespace CameraService
