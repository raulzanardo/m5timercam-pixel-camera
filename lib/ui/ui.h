#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <U8g2lib.h>

namespace Ui
{
  void init(U8G2_SSD1306_64X32_1F_F_HW_I2C &display,
            bool &inMenu,
            size_t &menuIndex,
            bool &statusModeActive,
            bool &isOff,
            bool &filterEnabled,
            bool &wakeShotEnabled,
            bool &littlefsReady,
            bool &showToast,
            uint32_t &toastUntilMs,
            uint32_t &lastStatusRefreshMs,
            bool &photoBlinkActive,
            uint32_t &photoBlinkUntilMs,
            Preferences &preferences,
            bool &preferencesReady,
            uint32_t &photoCounter,
            void (*enterDeepSleepFn)());

  void renderMenu();
  void handleClick();
  void handleDoubleClick();
  void handleLongPress();
  bool isStatusSelected();
  bool isStatusModeActive();
} // namespace Ui
