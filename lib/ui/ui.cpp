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
  bool *g_statusModeActive = nullptr;
  bool *g_isOff = nullptr;
  bool *g_filterEnabled = nullptr;
  bool *g_wakeShotEnabled = nullptr;
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
    ToggleWakeShot,
    Status,
  };

  bool isToggleItem(MenuItem item)
  {
    return item == MenuItem::ToggleFilter || item == MenuItem::ToggleWakeShot;
  }

  const char *toggleStateLabel(MenuItem item)
  {
    switch (item)
    {
    case MenuItem::ToggleFilter:
      return *g_filterEnabled ? "on" : "off";
    case MenuItem::ToggleWakeShot:
      return *g_wakeShotEnabled ? "on" : "off";
    default:
      return nullptr;
    }
  }

  void splitIpForDisplay(char *line1, size_t line1Size, char *line2, size_t line2Size)
  {
    if (!WebExport::isWifiReady())
    {
      snprintf(line1, line1Size, "");
      snprintf(line2, line2Size, "");
      return;
    }

    String ip = WebExport::localIP();
    int firstDot = ip.indexOf('.');
    int secondDot = firstDot >= 0 ? ip.indexOf('.', firstDot + 1) : -1;
    if (secondDot > 0)
    {
      ip.substring(0, secondDot).toCharArray(line1, line1Size);
      ip.substring(secondDot + 1).toCharArray(line2, line2Size);
      return;
    }

    size_t len = ip.length();
    size_t mid = len / 2;
    ip.substring(0, mid).toCharArray(line1, line1Size);
    ip.substring(mid).toCharArray(line2, line2Size);
  }

  void formatFreeSpace(char *buffer, size_t bufferSize)
  {
    if (!*g_littlefsReady)
    {
      snprintf(buffer, bufferSize, "free:err");
      return;
    }

    const size_t totalBytes = LittleFS.totalBytes();
    const size_t usedBytes = LittleFS.usedBytes();
    const size_t freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
    const float freeMegabytes = static_cast<float>(freeBytes) / (1024.0f * 1024.0f);

    if (freeMegabytes >= 10.0f)
    {
      snprintf(buffer, bufferSize, "free:%.0fMB", freeMegabytes);
      return;
    }

    snprintf(buffer, bufferSize, "free:%.1fMB", freeMegabytes);
  }

  void renderStatusScreen()
  {
    g_display->clearBuffer();
    g_display->setFont(u8g2_font_5x8_mf);

    const int batteryPct = TimerCAM.Power.getBatteryLevel();
    char batteryLine[18];
    char freeLine[18];

    snprintf(batteryLine, sizeof(batteryLine), "bat:%d%%", batteryPct);
    formatFreeSpace(freeLine, sizeof(freeLine));

    g_display->drawStr(2, 12, batteryLine);
    g_display->drawStr(2, 24, freeLine);
    g_display->sendBuffer();
    *g_lastStatusRefreshMs = millis();
  }

  void drawMenuRow(uint8_t row, MenuItem item, bool selected)
  {
    const uint8_t rowHeight = 8;
    const uint8_t rowY = row * rowHeight;
    const uint8_t textY = rowY + 7;

    if (selected)
    {
      g_display->drawBox(0, rowY, SCREEN_WIDTH, rowHeight);
      g_display->setDrawColor(0);
    }

    g_display->drawStr(2, textY, MENU_LABEL(static_cast<size_t>(item)));

    if (isToggleItem(item))
    {
      const char *state = toggleStateLabel(item);
      const uint8_t stateWidth = g_display->getStrWidth(state);
      const uint8_t stateX = SCREEN_WIDTH - stateWidth - 2;
      g_display->drawStr(stateX, textY, state);
    }

    if (selected)
    {
      g_display->setDrawColor(1);
    }
  }

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
    case MenuItem::ToggleWakeShot:
      *g_wakeShotEnabled = !*g_wakeShotEnabled;
      if (*g_preferencesReady)
      {
        g_preferences->putBool("wake_shot", *g_wakeShotEnabled);
      }
      break;
    case MenuItem::Status:
      *g_statusModeActive = true;
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
            void (*enterDeepSleepFn)())
  {
    g_display = &display;
    g_inMenu = &inMenu;
    g_menuIndex = &menuIndex;
    g_statusModeActive = &statusModeActive;
    g_isOff = &isOff;
    g_filterEnabled = &filterEnabled;
    g_wakeShotEnabled = &wakeShotEnabled;
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
    if (WebExport::isActive())
    {
      g_display->clearBuffer();
      g_display->setFont(u8g2_font_5x8_mf);

      char ipLine1[18];
      char ipLine2[18];
      splitIpForDisplay(ipLine1, sizeof(ipLine1), ipLine2, sizeof(ipLine2));
      g_display->drawStr(2, 14, ipLine1);
      g_display->drawStr(2, 24, ipLine2);
      g_display->sendBuffer();
      return;
    }

    if (*g_statusModeActive)
    {
      renderStatusScreen();
      return;
    }

    g_display->clearBuffer();
    g_display->setFont(u8g2_font_5x8_mf);
    const uint8_t maxVisibleRows = SCREEN_HEIGHT / 8;
    size_t firstVisibleIndex = 0;
    if (*g_menuIndex >= maxVisibleRows)
    {
      firstVisibleIndex = *g_menuIndex - maxVisibleRows + 1;
    }

    const size_t lastVisibleIndex = min(firstVisibleIndex + maxVisibleRows, static_cast<size_t>(MENU_COUNT));
    for (size_t index = firstVisibleIndex; index < lastVisibleIndex; ++index)
    {
      const MenuItem item = static_cast<MenuItem>(index);
      drawMenuRow(static_cast<uint8_t>(index - firstVisibleIndex), item, index == *g_menuIndex);
    }

    g_display->sendBuffer();
  }

  void handleClick()
  {
    if (*g_inMenu)
    {
      if (WebExport::isActive() || *g_statusModeActive)
      {
        renderMenu();
        return;
      }

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
      *g_statusModeActive = false;
      *g_inMenu = false;
    }
  }

  void handleLongPress()
  {
    if (!*g_inMenu)
    {
      *g_inMenu = true;
      *g_menuIndex = 0;
      *g_statusModeActive = false;
      *g_lastStatusRefreshMs = 0;
      renderMenu();
      return;
    }

    if (WebExport::isActive() || *g_statusModeActive)
    {
      renderMenu();
      return;
    }

    handleAction(static_cast<MenuItem>(*g_menuIndex));
  }

  bool isStatusSelected()
  {
    return static_cast<MenuItem>(*g_menuIndex) == MenuItem::Status;
  }

  bool isStatusModeActive()
  {
    return *g_statusModeActive;
  }
} // namespace Ui
