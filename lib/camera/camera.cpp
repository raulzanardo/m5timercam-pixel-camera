#include "camera.h"

#include <esp_camera.h>
#include <M5TimerCAM.h>
#include <LittleFS.h>

#include "../../include/config.h"
#include "../filter/filter.h"

namespace
{
  bool initCamera(pixformat_t fmt, framesize_t size, bool enableAec2 = false, uint32_t xclkHz = XCLK_FREQ_HZ)
  {
    camera_config_t config;
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
    config.fb_count = (fmt == CAPTURE_PIXEL_FORMAT) ? CAPTURE_FB_COUNT : 1;

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
      s->set_aec2(s, enableAec2 ? 1 : 0);
      s->set_whitebal(s, enableAec2 ? 1 : 0);
      s->set_gain_ctrl(s, enableAec2 ? 1 : 0);
    }
    return true;
  }

  bool saveJpgToSpiffs(camera_fb_t *fb, bool littlefsReady, uint32_t &photoCounter, bool preferencesReady, Preferences &preferences)
  {
    if (!littlefsReady || fb == nullptr)
    {
      Serial.println("Cannot save JPG: LittleFS not ready or frame missing");
      return false;
    }

    if (!LittleFS.exists("/photos"))
    {
      LittleFS.mkdir("/photos");
    }

    char path[48];
    snprintf(path, sizeof(path), "/photos/photo_%lu.jpg", static_cast<unsigned long>(photoCounter));

    File file = LittleFS.open(path, FILE_WRITE);
    if (!file)
    {
      Serial.printf("Failed to create %s\n", path);
      return false;
    }

    fmt2jpg_cb(reinterpret_cast<uint8_t *>(fb->buf), fb->len, fb->width, fb->height, PIXFORMAT_RGB565, 100, [](void *arg, size_t index, const void *data, size_t len) -> size_t
               {
                   fs::File *f = reinterpret_cast<fs::File *>(arg);
                   size_t written = f->write(reinterpret_cast<const uint8_t *>(data), len);
                   return written; }, &file);

    file.flush();
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
    return initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE, false, XCLK_FREQ_HZ);
  }

  bool capturePhotoToJpg(bool filterEnabled,
                         bool littlefsReady,
                         uint32_t &photoCounter,
                         bool preferencesReady,
                         Preferences &preferences)
  {
    esp_camera_deinit();

    if (!initCamera(CAPTURE_PIXEL_FORMAT, CAPTURE_FRAME_SIZE, true, CAPTURE_XCLK_FREQ_HZ))
    {
      Serial.println("Failed to init capture mode");
      initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE, false, XCLK_FREQ_HZ);
      return false;
    }

    for (int i = 0; i < 2; ++i)
    {
      camera_fb_t *warm = esp_camera_fb_get();
      if (warm)
        esp_camera_fb_return(warm);
      delay(40);
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
      Serial.println("Photo capture failed");
      esp_camera_deinit();
      initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE, false, XCLK_FREQ_HZ);
      return false;
    }

    if (filterEnabled)
    {
      applyAutoAdjust(fb);
      applyPicoPalette(fb);
    }

    saveJpgToSpiffs(fb, littlefsReady, photoCounter, preferencesReady, preferences);

    esp_camera_fb_return(fb);
    esp_camera_deinit();
    initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE, false, XCLK_FREQ_HZ);
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

    const float scale_x = static_cast<float>(fb->width) / SCREEN_WIDTH;
    const float scale_y = static_cast<float>(fb->height) / SCREEN_HEIGHT;
    const float scale = (scale_x < scale_y) ? scale_x : scale_y;

    const float crop_x = (fb->width - (SCREEN_WIDTH * scale)) * 0.5f;
    const float crop_y = (fb->height - (SCREEN_HEIGHT * scale)) * 0.5f;

    for (int y = 0; y < SCREEN_HEIGHT; y++)
    {
      for (int x = 0; x < SCREEN_WIDTH; x++)
      {
        int cam_x = static_cast<int>(crop_x + x * scale);
        int cam_y = static_cast<int>(crop_y + y * scale);
        int pixel_index = cam_y * fb->width + cam_x;
        buffer[y][x] = fb->buf[pixel_index];
      }
    }

    for (int y = 0; y < SCREEN_HEIGHT; y++)
    {
      for (int x = 0; x < SCREEN_WIDTH; x++)
      {
        int16_t oldPixel = buffer[y][x];
        int16_t newPixel = (oldPixel > 128) ? 255 : 0;

        if (newPixel > 128)
        {
          display.drawPixel(x, y);
        }

        int16_t error = oldPixel - newPixel;

        if (x + 1 < SCREEN_WIDTH)
        {
          buffer[y][x + 1] += (error * 7) / 16;
        }
        if (y + 1 < SCREEN_HEIGHT)
        {
          if (x > 0)
          {
            buffer[y + 1][x - 1] += (error * 3) / 16;
          }
          buffer[y + 1][x] += (error * 5) / 16;
          if (x + 1 < SCREEN_WIDTH)
          {
            buffer[y + 1][x + 1] += error / 16;
          }
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
