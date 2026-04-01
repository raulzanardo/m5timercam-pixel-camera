#include "ui.h"

#include <M5TimerCAM.h>
#include <LittleFS.h>

#include "../../include/config.h"
#include "../camera/camera.h"
#include "../web/web.h"

namespace
{
  U8G2_SSD1306_64X32_1F_F_HW_I2C *g_display = nullptr;
  bool *g_inMenu = nullptr;
  size_t *g_menuIndex = nullptr;
  bool *g_isOff = nullptr;
  bool *g_filterEnabled = nullptr;
  bool *g_littlefsReady = nullptr;
  bool *g_showToast = nullptr;
  uint32_t *g_toastUntilMs = nullptr;
  uint32_t *g_lastStatusRefreshMs = nullptr;
  bool *g_photoBlinkActive = nullptr;
  uint32_t *g_photoBlinkUntilMs = nullptr;
  Preferences *g_preferences = nullptr;
  bool *g_preferencesReady = nullptr;
  uint32_t *g_photoCounter = nullptr;
  void (*g_enterDeepSleepFn)() = nullptr;

  enum class MenuItem
  {
    Off,
    Export,
    ToggleFilter,
    Status,
  };

  void handleAction(MenuItem item)
  {
    switch (item)
    {
    case MenuItem::Off:
      g_display->clearBuffer();
      g_display->setFont(u8g2_font_5x8_mf);
      g_display->drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
      g_display->drawStr(2, 12, "Shutting down");
      g_display->sendBuffer();
      delay(600);

      if (g_enterDeepSleepFn)
      {
        g_enterDeepSleepFn();
      }
      break;
    case MenuItem::Export:
      g_display->clearBuffer();
      g_display->setFont(u8g2_font_5x8_mf);
      g_display->drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
      g_display->drawStr(2, 12, "connecting...");
      g_display->sendBuffer();

      WebExport::start();
      Ui::renderMenu();
      break;
    case MenuItem::ToggleFilter:
      *g_filterEnabled = !*g_filterEnabled;
      if (*g_preferencesReady)
      {
        g_preferences->putBool("filter_enabled", *g_filterEnabled);
      }
      break;
    case MenuItem::Status:
      break;
    }

    Ui::renderMenu();
  }
} // namespace

namespace Ui
{
  void init(U8G2_SSD1306_64X32_1F_F_HW_I2C &display,
            bool &inMenu,
            size_t &menuIndex,
            bool &isOff,
            bool &filterEnabled,
            bool &littlefsReady,
            bool &showToast,
            uint32_t &toastUntilMs,
            uint32_t &lastStatusRefreshMs,
            bool &photoBlinkActive,
            uint32_t &photoBlinkUntilMs,
            Preferences &preferences,
            bool &preferencesReady,
            uint32_t &photoCounter,
            void (*enterDeepSleepFn)())
  {
    g_display = &display;
    g_inMenu = &inMenu;
    g_menuIndex = &menuIndex;
    g_isOff = &isOff;
    g_filterEnabled = &filterEnabled;
    g_littlefsReady = &littlefsReady;
    g_showToast = &showToast;
    g_toastUntilMs = &toastUntilMs;
    g_lastStatusRefreshMs = &lastStatusRefreshMs;
    g_photoBlinkActive = &photoBlinkActive;
    g_photoBlinkUntilMs = &photoBlinkUntilMs;
    g_preferences = &preferences;
    g_preferencesReady = &preferencesReady;
    g_photoCounter = &photoCounter;
    g_enterDeepSleepFn = enterDeepSleepFn;
  }

  void renderMenu()
  {
    g_display->clearBuffer();
    g_display->setFont(u8g2_font_5x8_mf);
    g_display->drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    g_display->drawBox(1, 3, SCREEN_WIDTH - 2, 10);
    g_display->setDrawColor(0);
    g_display->drawStr(3, 11, MENU_LABEL(*g_menuIndex));
    g_display->setDrawColor(1);

    switch (static_cast<MenuItem>(*g_menuIndex))
    {
    case MenuItem::Off:
    {
      char state[16];
      snprintf(state, sizeof(state), "state:%s", *g_isOff ? "off" : "on");
      g_display->setFont(u8g2_font_5x8_mf);
      g_display->drawStr(2, 31, state);
      break;
    }
    case MenuItem::Export:
    {
      g_display->setFont(u8g2_font_5x8_mf);
      char ipLine1[18];
      char ipLine2[18];
      if (WebExport::isWifiReady())
      {
        String ip = WebExport::localIP();
        int firstDot = ip.indexOf('.');
        int secondDot = firstDot >= 0 ? ip.indexOf('.', firstDot + 1) : -1;
        if (secondDot > 0)
        {
          ip.substring(0, secondDot).toCharArray(ipLine1, sizeof(ipLine1));
          ip.substring(secondDot + 1).toCharArray(ipLine2, sizeof(ipLine2));
        }
        else
        {
          size_t len = ip.length();
          size_t mid = len / 2;
          ip.substring(0, mid).toCharArray(ipLine1, sizeof(ipLine1));
          ip.substring(mid).toCharArray(ipLine2, sizeof(ipLine2));
        }
      }
      else
      {
        snprintf(ipLine1, sizeof(ipLine1), "--");
        snprintf(ipLine2, sizeof(ipLine2), "");
      }
      g_display->drawStr(2, 20, ipLine1);
      g_display->drawStr(2, 31, ipLine2);
      break;
    }
    case MenuItem::ToggleFilter:
    {
      char state[16];
      snprintf(state, sizeof(state), "state:%s", *g_filterEnabled ? "on" : "off");
      g_display->setFont(u8g2_font_5x8_mf);
      g_display->drawStr(2, 31, state);
      break;
    }
    case MenuItem::Status:
    {
      g_display->setFont(u8g2_font_5x8_mf);
      size_t fsFreeKB = 0;
      if (*g_littlefsReady)
      {
        size_t fsTotal = LittleFS.totalBytes();
        size_t fsUsed = LittleFS.usedBytes();
        fsFreeKB = (fsTotal > fsUsed) ? (fsTotal - fsUsed) / 1024 : 0;
      }
      int batteryPct = TimerCAM.Power.getBatteryLevel();
      char line1[22];
      snprintf(line1, sizeof(line1), "bat:%d%%", batteryPct);
      g_display->drawStr(2, 23, line1);
      char line2[18];
      if (*g_littlefsReady)
      {
        snprintf(line2, sizeof(line2), "fs:%luk", static_cast<unsigned long>(fsFreeKB));
      }
      else
      {
        snprintf(line2, sizeof(line2), "fs:err");
      }
      g_display->drawStr(2, 31, line2);
      *g_lastStatusRefreshMs = millis();
      break;
    }
    }
    g_display->sendBuffer();
  }

  void handleClick()
  {
    if (*g_inMenu)
    {
      *g_menuIndex = (*g_menuIndex + 1) % MENU_COUNT;
      renderMenu();
      return;
    }

    TimerCAM.Power.setLed(PHOTO_LED_BRIGHTNESS);
    *g_photoBlinkActive = true;
    *g_photoBlinkUntilMs = millis() + PHOTO_LED_DURATION_MS;

    g_display->clearBuffer();
    g_display->setFont(u8g2_font_5x8_mf);
    g_display->drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    g_display->drawStr(2, 18, "processing...");
    g_display->sendBuffer();

    CameraService::capturePhotoToJpg(*g_filterEnabled, *g_littlefsReady, *g_photoCounter, *g_preferencesReady, *g_preferences);

    *g_showToast = true;
    *g_toastUntilMs = millis() + TOAST_DURATION_MS;
    g_display->clearBuffer();
    g_display->setFont(u8g2_font_5x8_mf);
    g_display->drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    g_display->drawStr(5, 18, "photo saved");
    g_display->sendBuffer();
  }

  void handleDoubleClick()
  {
    if (*g_inMenu)
    {
      *g_inMenu = false;
    }
  }

  void handleLongPress()
  {
    if (!*g_inMenu)
    {
      *g_inMenu = true;
      *g_menuIndex = 0;
      *g_lastStatusRefreshMs = 0;
      renderMenu();
      return;
    }
    handleAction(static_cast<MenuItem>(*g_menuIndex));
  }

  bool isStatusSelected()
  {
    return static_cast<MenuItem>(*g_menuIndex) == MenuItem::Status;
  }
} // namespace Ui
