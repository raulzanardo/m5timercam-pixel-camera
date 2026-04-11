#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <U8g2lib.h>

namespace CameraService
{
  bool initLive();

  bool capturePhotoToJpg(bool filterEnabled,
                         bool littlefsReady,
                         uint32_t &photoCounter,
                         bool preferencesReady,
                         Preferences &preferences);

  bool renderLivePreview(U8G2_SSD1306_64X32_1F_F_HW_I2C &display,
                         int16_t *ditherBuffer);

  void shutdownForSleep();
} // namespace CameraService
