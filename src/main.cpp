#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <M5TimerCAM.h>
#include <U8g2lib.h>
#include <OneButton.h>
#include <LittleFS.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include <esp_system.h>
#include <cstdlib>
#include "driver/gpio.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include "config.h"
#include "camera.h"
#include "web.h"

U8G2_SSD1306_64X32_1F_F_HW_I2C display(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
OneButton button(BUTTON_PIN, true, false);

enum class MenuItem
{
    Off,
    Export,
    ToggleFilter,
    Status,
};

bool inMenu = false;
size_t menuIndex = 0;
bool isOff = false;
bool filterEnabled = false;
bool littlefsReady = false;
bool showToast = false;
uint32_t toastUntilMs = 0;
uint32_t lastStatusRefreshMs = 0;
bool photoBlinkActive = false;
uint32_t photoBlinkUntilMs = 0;
Preferences preferences;
bool preferencesReady = false;
uint32_t photoCounter = 0;
bool timerPowerReady = false; // tracks TimerCAM.begin success for power features

static int16_t ditherBuffer[SCREEN_HEIGHT][SCREEN_WIDTH];

void enterDeepSleep()
{
    // Mirror the sleep flow from teste.cpp_: hold power, configure wake, wait for button release, then sleep
    gpio_hold_en((gpio_num_t)POWER_HOLD_PIN);
    gpio_deep_sleep_hold_en();
    // Tear down peripherals to cut current
    if (WebExport::isActive())
    {
        WebExport::stop();
    }
    WiFi.mode(WIFI_OFF);
    btStop();

    CameraService::shutdownForSleep();
    // Ensure LED is off before sleeping
    TimerCAM.Power.setLed(0);
    // Put PMIC into timer sleep to lower quiescent current
    if (timerPowerReady)
    {
        TimerCAM.Power.timerSleep(5);
    }
    display.setPowerSave(1); // put OLED to sleep
    Wire.end();
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BUTTON_PIN), 0); // wake on button low
    // Avoid immediate re-wake by waiting until the button is released (goes high)
    while (digitalRead(BUTTON_PIN) == LOW)
    {
        delay(1);
    }
    if (preferencesReady)
    {
        preferences.end();
    }
    esp_deep_sleep_start();
}

void renderMenu()
{
    display.clearBuffer();
    display.setFont(u8g2_font_5x8_mf);
    display.drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    // Highlight current item with a light banner near the top (using freed space)
    display.drawBox(1, 3, SCREEN_WIDTH - 2, 10);
    display.setDrawColor(0); // invert inside banner
    display.drawStr(3, 11, MENU_LABEL(menuIndex));
    display.setDrawColor(1);

    // Show state hints
    switch (static_cast<MenuItem>(menuIndex))
    {
    case MenuItem::Off:
    {
        char state[16];
        snprintf(state, sizeof(state), "state:%s", isOff ? "off" : "on");
        display.setFont(u8g2_font_5x8_mf);
        display.drawStr(2, 31, state);
        break;
    }
    case MenuItem::Export:
    {
        display.setFont(u8g2_font_5x8_mf);
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
        display.drawStr(2, 20, ipLine1);
        display.drawStr(2, 31, ipLine2);
        break;
    }
    case MenuItem::ToggleFilter:
    {
        char state[16];
        snprintf(state, sizeof(state), "state:%s", filterEnabled ? "on" : "off");
        display.setFont(u8g2_font_5x8_mf);
        display.drawStr(2, 31, state);
        break;
    }
    case MenuItem::Status:
    {
        // Use smaller font to fit more info
        display.setFont(u8g2_font_5x8_mf);
        size_t fsFreeKB = 0;
        if (littlefsReady)
        {
            size_t fsTotal = LittleFS.totalBytes();
            size_t fsUsed = LittleFS.usedBytes();
            fsFreeKB = (fsTotal > fsUsed) ? (fsTotal - fsUsed) / 1024 : 0;
        }
        int batteryPct = TimerCAM.Power.getBatteryLevel();
        char line1[22];
        snprintf(line1, sizeof(line1), "bat:%d%%", batteryPct);
        display.drawStr(2, 23, line1);
        char line2[18];
        if (littlefsReady)
        {
            snprintf(line2, sizeof(line2), "fs:%luk", static_cast<unsigned long>(fsFreeKB));
        }
        else
        {
            snprintf(line2, sizeof(line2), "fs:err");
        }
        display.drawStr(2, 31, line2);
        lastStatusRefreshMs = millis();
        break;
    }
    }
    display.sendBuffer();
}

void handleAction(MenuItem item)
{
    switch (item)
    {
    case MenuItem::Off:
        display.clearBuffer();
        display.setFont(u8g2_font_5x8_mf);
        display.drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
        display.drawStr(2, 12, "Shutting down");
        display.sendBuffer();
        delay(600);

        enterDeepSleep();
        break; // not reached
    case MenuItem::Export:
        // Show connecting state while Wi-Fi is being established
        display.clearBuffer();
        display.setFont(u8g2_font_5x8_mf);
        display.drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
        display.drawStr(2, 12, "connecting...");
        display.sendBuffer();

        WebExport::start();
        renderMenu(); // refresh to show IP if connected
        break;
    case MenuItem::ToggleFilter:
        filterEnabled = !filterEnabled;
        preferences.putBool("filter_enabled", filterEnabled);
        break;
    case MenuItem::Status:
        // no toggle; just refresh
        break;
    }
    // stay in menu to keep navigating
    renderMenu();
}

void handleClick()
{
    if (inMenu)
    {
        menuIndex = (menuIndex + 1) % MENU_COUNT;
        renderMenu();
        return;
    }

    // Show immediate feedback before capture starts
    TimerCAM.Power.setLed(PHOTO_LED_BRIGHTNESS);
    photoBlinkActive = true;
    photoBlinkUntilMs = millis() + PHOTO_LED_DURATION_MS;

    // Show processing message
    display.clearBuffer();
    display.setFont(u8g2_font_5x8_mf);
    display.drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    display.drawStr(2, 18, "processing...");
    display.sendBuffer();

    // Take photo (this is the slow part)
    CameraService::capturePhotoToJpg(filterEnabled, littlefsReady, photoCounter, preferencesReady, preferences);

    // Show completion toast after capture
    showToast = true;
    toastUntilMs = millis() + TOAST_DURATION_MS;
    display.clearBuffer();
    display.setFont(u8g2_font_5x8_mf);
    display.drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    display.drawStr(5, 18, "photo saved");
    display.sendBuffer();
}

void handleDoubleClick()
{
    if (inMenu)
    {
        inMenu = false;
    }
}

void handleLongPress()
{
    if (!inMenu)
    {
        inMenu = true;
        menuIndex = 0;
        lastStatusRefreshMs = 0;
        renderMenu();
        return;
    }
    handleAction(static_cast<MenuItem>(menuIndex));
}

void setup()
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable detector

    Serial.begin(115200);
    delay(150);
    Serial.println();
    Serial.printf("reset_reason:%d\n", static_cast<int>(esp_reset_reason()));
    // delay(2000);
    Serial.println("TimerCAM snapshot server booting…");
    // Run CPU a bit slower to cut active power (still enough for display + camera)
    setCpuFrequencyMhz(80);
    // Ensure radios are off by default
    WiFi.mode(WIFI_OFF);
    btStop();
    // Init power/battery (required before calling Power.getBattery*)
    // RTC-enabled init was preventing camera start; use default init and skip timerSleep
    TimerCAM.begin();
    timerPowerReady = false; // disable timerSleep path

    // Mount LittleFS for storage stats
    littlefsReady = LittleFS.begin(true);
    if (!littlefsReady)
    {
        Serial.println("LittleFS mount failed");
    }

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    button.setDebounceMs(30);
    button.setPressMs(700);
    button.attachClick(handleClick);
    button.attachDoubleClick(handleDoubleClick);
    button.attachLongPressStart(handleLongPress);

    preferencesReady = preferences.begin("photos", false);
    if (!preferencesReady)
    {
        Serial.println("Preferences init failed; photo index will rely on filesystem scan only");
    }
    if (preferencesReady)
    {
        photoCounter = preferences.getUInt("photo_idx", 0);
        filterEnabled = preferences.getBool("filter_enabled", false);
    }

    // Ensure photoCounter is absolute to avoid overwriting older photos
    if (littlefsReady)
    {
        if (!LittleFS.exists("/photos"))
        {
            LittleFS.mkdir("/photos");
        }
        uint32_t maxIdx = 0;
        File rootIdx = LittleFS.open("/photos");
        if (rootIdx)
        {
            File fIdx = rootIdx.openNextFile();
            while (fIdx)
            {
                String fname = String(fIdx.name());
                fIdx.close();
                int us = fname.lastIndexOf('_');
                int dot = fname.lastIndexOf('.');
                if (us >= 0 && dot > us)
                {
                    String numStr = fname.substring(us + 1, dot);
                    uint32_t idx = static_cast<uint32_t>(numStr.toInt());
                    if (idx > maxIdx)
                        maxIdx = idx;
                }
                fIdx = rootIdx.openNextFile();
            }
            rootIdx.close();
        }
        uint32_t fsNext = maxIdx + 1;
        if (fsNext > photoCounter)
        {
            photoCounter = fsNext;
        }
        if (preferencesReady)
        {
            preferences.putUInt("photo_idx", photoCounter);
        }
    }

    // if (!TimerCAM.Camera.begin())
    // {
    //   Serial.println("Camera init failed!");
    //   while (true)
    //     delay(1000);
    // }

    display.begin();
    display.setPowerSave(0);  // wake OLED after init
    display.setContrast(255); // max contrast for sharper image (can be adjusted down to save power)
    display.setDisplayRotation(U8G2_R2);
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_mf);
    display.drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    display.drawStr(10, 12, "Start");

    display.sendBuffer(); // transfer internal memory to the display

    if (!CameraService::initLive())
    {
        Serial.println("Camera init failed!");
        return;
    }
}

void loop()
{

    // Always tick the button first
    button.tick();

    // If menu was closed, stop export server/Wi-Fi
    if (!inMenu && WebExport::isActive())
    {
        WebExport::stop();
    }

    // Turn off photo blink LED after duration
    if (photoBlinkActive && millis() >= photoBlinkUntilMs)
    {
        TimerCAM.Power.setLed(0);
        photoBlinkActive = false;
    }

    // If menu is open, just keep the menu shown and skip camera work
    if (inMenu)
    {
        WebExport::poll();
        if (static_cast<MenuItem>(menuIndex) == MenuItem::Status)
        {
            const uint32_t now = millis();
            if (now - lastStatusRefreshMs >= STATUS_REFRESH_MS)
            {
                renderMenu();
            }
        }
        return;
    }

    // If a toast is active, keep it on screen until timeout
    if (showToast)
    {
        if (millis() >= toastUntilMs)
        {
            showToast = false;
        }
        else
        {
            return;
        }
    }

    // If turned off, blank the screen and skip camera capture
    if (isOff)
    {
        display.clearBuffer();
        display.setFont(u8g2_font_6x10_mf);
        display.drawStr(0, 12, "OFF");
        display.sendBuffer();
        delay(50);
        return;
    }

    CameraService::renderLivePreview(display, &ditherBuffer[0][0]);
}