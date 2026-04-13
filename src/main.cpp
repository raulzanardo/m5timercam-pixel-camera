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
#include "ui.h"
#include "web.h"

U8G2_SSD1306_64X32_1F_F_HW_I2C display(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
OneButton button(BUTTON_PIN, true, false);

constexpr uint8_t SSD1306_CMD_CHARGE_PUMP = 0x8D;
constexpr uint8_t SSD1306_CHARGE_PUMP_9V = 0x95;

bool inMenu = false;
size_t menuIndex = 0;
bool statusModeActive = false;
bool isOff = false;
bool filterEnabled = false;
bool wakeShotEnabled = false;
bool littlefsReady = false;
bool showToast = false;
uint32_t toastUntilMs = 0;
uint32_t lastStatusRefreshMs = 0;
bool photoBlinkActive = false;
uint32_t photoBlinkUntilMs = 0;
bool wakeAutoSleepPending = false;
uint32_t wakeAutoSleepDeadlineMs = 0;
uint32_t wakeAutoSleepIgnoreInputUntilMs = 0;
Preferences preferences;
bool preferencesReady = false;
uint32_t photoCounter = 0;
bool timerPowerReady = false; // tracks TimerCAM.begin success for power features

static int16_t ditherBuffer[SCREEN_HEIGHT][SCREEN_WIDTH];

void enableDisplayChargePump()
{
    u8x8_t *u8x8 = display.getU8x8();
    u8x8_cad_StartTransfer(u8x8);
    u8x8_cad_SendCmd(u8x8, SSD1306_CMD_CHARGE_PUMP);
    u8x8_cad_SendArg(u8x8, SSD1306_CHARGE_PUMP_9V);
    u8x8_cad_EndTransfer(u8x8);
}

bool shouldRescanPhotoIndex()
{
    return littlefsReady && (!preferencesReady || photoCounter == 0);
}

void cancelWakeAutoSleep()
{
    wakeAutoSleepPending = false;
    wakeAutoSleepDeadlineMs = 0;
    wakeAutoSleepIgnoreInputUntilMs = 0;
}

bool shouldIgnoreWakeAction()
{
    return wakeAutoSleepPending && millis() < wakeAutoSleepIgnoreInputUntilMs;
}

void handleButtonClick()
{
    if (shouldIgnoreWakeAction())
    {
        return;
    }

    cancelWakeAutoSleep();
    Ui::handleClick();
}

void handleButtonDoubleClick()
{
    if (shouldIgnoreWakeAction())
    {
        return;
    }

    cancelWakeAutoSleep();
    Ui::handleDoubleClick();
}

void handleButtonLongPress()
{
    if (shouldIgnoreWakeAction())
    {
        return;
    }

    cancelWakeAutoSleep();
    Ui::handleLongPress();
}

void runWakeShotCapture()
{
    TimerCAM.Power.setLed(PHOTO_LED_BRIGHTNESS);
    photoBlinkActive = true;
    photoBlinkUntilMs = millis() + PHOTO_LED_DURATION_MS;

    display.clearBuffer();
    display.setFont(u8g2_font_5x8_mf);
    display.drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    display.drawStr(2, 18, "processing...");
    display.sendBuffer();

    const bool saved = CameraService::capturePhotoToJpg(filterEnabled, littlefsReady, photoCounter, preferencesReady, preferences);

    showToast = true;
    toastUntilMs = millis() + TOAST_DURATION_MS;
    display.clearBuffer();
    display.setFont(u8g2_font_5x8_mf);
    display.drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    display.drawStr(5, 18, saved ? "photo saved" : "photo error");
    display.sendBuffer();

    wakeAutoSleepPending = true;
    wakeAutoSleepDeadlineMs = millis() + WAKE_CAPTURE_SLEEP_DELAY_MS;
    wakeAutoSleepIgnoreInputUntilMs = millis() + WAKE_CAPTURE_IGNORE_INPUT_MS;
}

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

void setup()
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable detector
    const esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();

    Serial.begin(115200);
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
    button.setDebounceMs(5);
    button.setPressMs(700);
    button.setLongPressIntervalMs(1000);
    button.attachClick(handleButtonClick);
    button.attachDoubleClick(handleButtonDoubleClick);
    button.attachLongPressStart(handleButtonLongPress);

    preferencesReady = preferences.begin("photos", false);
    if (!preferencesReady)
    {
        Serial.println("Preferences init failed; photo index will rely on filesystem scan only");
    }
    if (preferencesReady)
    {
        photoCounter = preferences.getUInt("photo_idx", 0);
        filterEnabled = preferences.getBool("filter_enabled", false);
        wakeShotEnabled = preferences.getBool("wake_shot", false);
    }

    const bool wakeShotBoot = wakeShotEnabled && wakeupCause == ESP_SLEEP_WAKEUP_EXT0;

    Ui::init(display,
             inMenu,
             menuIndex,
             statusModeActive,
             isOff,
             filterEnabled,
             wakeShotEnabled,
             littlefsReady,
             showToast,
             toastUntilMs,
             lastStatusRefreshMs,
             photoBlinkActive,
             photoBlinkUntilMs,
             preferences,
             preferencesReady,
             photoCounter,
             enterDeepSleep);

    // Ensure photoCounter is absolute to avoid overwriting older photos
    if (shouldRescanPhotoIndex())
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
    else if (littlefsReady && !LittleFS.exists("/photos"))
    {
        LittleFS.mkdir("/photos");
    }

    // if (!TimerCAM.Camera.begin())
    // {
    //   Serial.println("Camera init failed!");
    //   while (true)
    //     delay(1000);
    // }

    display.initDisplay();
    enableDisplayChargePump();
    display.setPowerSave(0);  // wake OLED after init
    display.setContrast(255); // max contrast for sharper image (can be adjusted down to save power)
    display.setDisplayRotation(U8G2_R2);

    if (wakeShotBoot)
    {
        runWakeShotCapture();
        return;
    }

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

    if (wakeAutoSleepPending && millis() >= wakeAutoSleepDeadlineMs)
    {
        enterDeepSleep();
        return;
    }

    // If menu is open, just keep the menu shown and skip camera work
    if (inMenu)
    {
        WebExport::poll();
        if (Ui::isStatusModeActive())
        {
            const uint32_t now = millis();
            if (now - lastStatusRefreshMs >= STATUS_REFRESH_MS)
            {
                Ui::renderMenu();
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