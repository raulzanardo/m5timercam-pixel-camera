#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <M5TimerCAM.h>
#include <U8g2lib.h>
#include <esp_camera.h>
#include <OneButton.h>
#include <LittleFS.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include <esp_system.h>
#include <cstdlib>
#include <vector>
#include <algorithm>

#include "driver/gpio.h"
#include "filter.h"
#include "config.h"

U8G2_SSD1306_64X32_1F_F_HW_I2C display(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
OneButton button(BUTTON_PIN, true, false);

enum class MenuItem
{
    Off,
    Export,
    ToggleFilter,
    Status,
};

const char *menuLabels[] = {"off", "export", "filter", "status"};
constexpr size_t MENU_COUNT = sizeof(menuLabels) / sizeof(menuLabels[0]);

bool inMenu = false;
size_t menuIndex = 0;
bool isOff = false;
bool filterEnabled = false;
bool littlefsReady = false;
bool showToast = false;
uint32_t toastUntilMs = 0;
constexpr uint32_t TOAST_DURATION_MS = 800;
uint32_t lastStatusRefreshMs = 0;
constexpr uint32_t STATUS_REFRESH_MS = 5000;
bool photoBlinkActive = false;
uint32_t photoBlinkUntilMs = 0;
constexpr uint8_t PHOTO_LED_BRIGHTNESS = 80; // lower brightness to cut LED draw (10-bit duty)
constexpr uint32_t PHOTO_LED_DURATION_MS = 150;
constexpr uint8_t CPU_FREQ_LOW_MHZ = 80;
constexpr uint8_t CPU_FREQ_HIGH_MHZ = 240;  // use full speed when exporting
constexpr uint32_t XCLK_FREQ_HZ = 10000000; // lower clock to cut active power
constexpr pixformat_t LIVE_PIXEL_FORMAT = PIXFORMAT_GRAYSCALE;
constexpr framesize_t LIVE_FRAME_SIZE = FRAMESIZE_QQVGA;
constexpr pixformat_t CAPTURE_PIXEL_FORMAT = PIXFORMAT_RGB565;
constexpr framesize_t CAPTURE_FRAME_SIZE = FRAMESIZE_QVGA;
constexpr uint8_t CAPTURE_FB_COUNT = 2;
Preferences preferences;
bool preferencesReady = false;
uint32_t photoCounter = 0;
WiFiServer exportServer(80);
bool exportServerActive = false;
bool exportWifiReady = false;
bool timerPowerReady = false; // tracks TimerCAM.begin success for power features
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;

static int16_t ditherBuffer[SCREEN_HEIGHT][SCREEN_WIDTH];

String formatBytes(size_t bytes)
{
    if (bytes < 1024)
        return String(bytes) + " B";
    if (bytes < 1024 * 1024)
        return String(bytes / 1024.0, 1) + " KB";
    return String(bytes / 1024.0 / 1024.0, 2) + " MB";
}

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
        s->set_vflip(s, 1); // flip vertically
        s->set_aec2(s, enableAec2 ? 1 : 0);
        // Enable AWB/AGC when capturing for more stable frames
        s->set_whitebal(s, enableAec2 ? 1 : 0);  // AWB
        s->set_gain_ctrl(s, enableAec2 ? 1 : 0); // AGC
    }
    return true;
}

bool saveJpgToSpiffs(camera_fb_t *fb)
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

    // Encode JPEG using esp_camera helper (quality 12)
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

void capturePhotoToJpg()
{
    // Must reinitialize camera for different frame buffer allocation
    // Trying to change resolution without reinit causes memory issues
    esp_camera_deinit();

    // Use higher XCLK during capture to reduce rolling-shutter wobble
    if (!initCamera(CAPTURE_PIXEL_FORMAT, CAPTURE_FRAME_SIZE, true, 20000000))
    {
        Serial.println("Failed to init capture mode");
        initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE, false, XCLK_FREQ_HZ);
        return;
    }

    // Discard a few frames to let exposure/gain settle
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
        return;
    }

    if (filterEnabled)
    {
        applyAutoAdjust(fb);
        applyPicoPalette(fb);
    }

    saveJpgToSpiffs(fb);

    esp_camera_fb_return(fb);

    // Return to live preview mode
    esp_camera_deinit();
    initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE, false, XCLK_FREQ_HZ);
}

void stopExportServer()
{
    if (exportServerActive)
    {
        exportServer.close();
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        setCpuFrequencyMhz(CPU_FREQ_LOW_MHZ);
        exportServerActive = false;
        exportWifiReady = false;
    }
}

bool startExportServer()
{
    if (exportServerActive)
        return true;

    if (!LittleFS.exists("/photos"))
    {
        Serial.println("Export: /photos missing, creating");
        LittleFS.mkdir("/photos");
    }

    WiFi.mode(WIFI_STA);
    // Use full speed while exporting to keep Wi-Fi stable and responsive
    setCpuFrequencyMhz(CPU_FREQ_HIGH_MHZ);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Export Wi-Fi: connecting to %s...\n", WIFI_SSID);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS)
    {
        delay(200);
    }
    exportWifiReady = WiFi.status() == WL_CONNECTED;
    if (!exportWifiReady)
    {
        Serial.println("Export Wi-Fi: failed to connect");
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        setCpuFrequencyMhz(CPU_FREQ_LOW_MHZ);
        return false;
    }
    Serial.printf("Export Wi-Fi: connected, IP=%s\n", WiFi.localIP().toString().c_str());
    exportServer.begin();
    exportServerActive = true;
    Serial.println("Export server started on port 80");
    return true;
}

void handleExportClient(WiFiClient &client)
{
    String req = client.readStringUntil('\n');
    if (req.length() == 0)
        return;

    Serial.printf("Export request: %s\n", req.c_str());
    int firstSpace = req.indexOf(' ');
    int secondSpace = req.indexOf(' ', firstSpace + 1);
    String path = req.substring(firstSpace + 1, secondSpace);

    if (path.startsWith("/file?name="))
    {
        String name = path.substring(strlen("/file?name="));
        if (!name.startsWith("/"))
        {
            name = String("/photos/") + name;
        }
        if (!LittleFS.exists("/photos"))
        {
            Serial.println("Export: /photos missing, creating");
            LittleFS.mkdir("/photos");
        }
        File f = LittleFS.open(name, FILE_READ);
        if (!f)
        {
            Serial.printf("Export: file not found %s\n", name.c_str());
            client.println("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n\r\nNot found");
            client.stop();
            return;
        }
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: image/jpeg");
        client.println("Connection: close");
        client.println();
        while (f.available())
        {
            uint8_t buf[512];
            size_t n = f.read(buf, sizeof(buf));
            client.write(buf, n);
        }
        f.close();
        client.stop();
        return;
    }

    if (path.startsWith("/delete?name="))
    {
        String name = path.substring(strlen("/delete?name="));
        if (!name.startsWith("/"))
        {
            name = String("/photos/") + name;
        }
        if (LittleFS.exists(name))
        {
            LittleFS.remove(name);
            Serial.printf("Export: deleted %s\n", name.c_str());
        }
        client.println("HTTP/1.1 302 Found\r\nLocation: /\r\nConnection: close\r\n\r\n");
        client.stop();
        return;
    }

    if (path.equalsIgnoreCase("/delete-all"))
    {
        File root = LittleFS.open("/photos");
        if (root)
        {
            File f = root.openNextFile();
            while (f)
            {
                String pathToDelete = String(f.name());
                f.close();
                if (!pathToDelete.startsWith("/"))
                {
                    pathToDelete = String("/photos/") + pathToDelete;
                }
                if (LittleFS.exists(pathToDelete))
                {
                    LittleFS.remove(pathToDelete);
                    Serial.printf("Export: deleted %s\n", pathToDelete.c_str());
                }
                f = root.openNextFile();
            }
            root.close();
        }
        client.println("HTTP/1.1 302 Found\r\nLocation: /\r\nConnection: close\r\n\r\n");
        client.stop();
        return;
    }

    if (path.equals("/favicon.ico"))
    {
        client.println("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
        client.stop();
        return;
    }

    // default: list files
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;

    if (!LittleFS.exists("/photos"))
    {
        Serial.println("Export: /photos missing, creating");
        LittleFS.mkdir("/photos");
    }

    struct PhotoEntry
    {
        String name;
        size_t size;
        uint32_t idx;
    };

    std::vector<PhotoEntry> photos;
    {
        File rootCount = LittleFS.open("/photos");
        if (rootCount)
        {
            File fCount = rootCount.openNextFile();
            while (fCount)
            {
                PhotoEntry entry;
                entry.name = String(fCount.name());
                entry.size = fCount.size();
                fCount.close();

                int us = entry.name.lastIndexOf('_');
                int dot = entry.name.lastIndexOf('.');
                if (us >= 0 && dot > us)
                {
                    entry.idx = static_cast<uint32_t>(entry.name.substring(us + 1, dot).toInt());
                }
                else
                {
                    entry.idx = 0;
                }

                photos.push_back(entry);
                fCount = rootCount.openNextFile();
            }
            rootCount.close();
        }
    }

    std::sort(photos.begin(), photos.end(), [](const PhotoEntry &a, const PhotoEntry &b)
              {
        if (a.idx != b.idx)
            return a.idx < b.idx;
        return a.name < b.name; });

    const uint32_t photoCount = static_cast<uint32_t>(photos.size());

    constexpr size_t EST_PHOTO_BYTES = 100 * 1024;
    const uint32_t remainingPhotos = (EST_PHOTO_BYTES > 0) ? static_cast<uint32_t>(freeBytes / EST_PHOTO_BYTES) : 0;

    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n");
    client.println();
    client.println("<!doctype html><html><head><meta charset='utf-8'>"
                   "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                   "<script src='https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js'></script>"
                   "<script src='https://cdnjs.cloudflare.com/ajax/libs/FileSaver.js/2.0.5/FileSaver.min.js'></script>"
                   "<script>"
                   "async function downloadAllAsZip(){"
                   "const btn=event.target;btn.disabled=true;btn.textContent='Creating ZIP...';"
                   "try{"
                   "const zip=new JSZip();"
                   "const cards=document.querySelectorAll('.card');"
                   "let loaded=0;"
                   "for(const card of cards){"
                   "const name=card.querySelector('.name').textContent;"
                   "const url='/file?name='+encodeURIComponent(name);"
                   "const resp=await fetch(url);"
                   "const blob=await resp.blob();"
                   "zip.file(name,blob);"
                   "loaded++;"
                   "btn.textContent='Adding '+loaded+'/'+cards.length+'...';"
                   "}"
                   "btn.textContent='Generating ZIP...';"
                   "const content=await zip.generateAsync({type:'blob'});"
                   "saveAs(content,'photos.zip');"
                   "btn.textContent='Download all as ZIP';btn.disabled=false;"
                   "}catch(e){alert('Error: '+e.message);btn.textContent='Download all as ZIP';btn.disabled=false;}"
                   "}"
                   "</script>"
                   "<style>body{font-family:Arial, sans-serif;background:#0e1726;color:#e8ecf1;margin:0;padding:16px;}h3{margin-top:0;}"
                   ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:12px;margin-top:12px;}"
                   ".card{background:#162235;border:1px solid #24344f;border-radius:10px;padding:12px;box-shadow:0 8px 18px rgba(0,0,0,0.25);display:flex;justify-content:space-between;align-items:center;gap:10px;}"
                   ".name{font-size:13px;word-break:break-all;}"
                   "a.btn{display:inline-block;padding:6px 10px;border-radius:6px;text-decoration:none;font-size:12px;"
                   "background:#ff6b6b;color:#0e1726;font-weight:700;}a.btn:hover{background:#ffa8a8;}"
                   "a.btn2,button.btn2{display:inline-block;padding:8px 12px;border-radius:8px;border:none;cursor:pointer;text-decoration:none;font-size:13px;font-weight:700;"
                   "background:#4ade80;color:#0e1726;}a.btn2:hover,button.btn2:hover:not(:disabled){background:#7ee6a9;}button.btn2:disabled{opacity:0.6;cursor:not-allowed;}"
                   ".topbar{display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap;}"
                   ".meta{font-size:12px;color:#cdd5df;}"
                   ".size{font-size:12px;color:#9fb3c8;}"
                   "</style></head><body>");
    client.print("<div class='topbar'><h3>Photos</h3>"
                 "<div class='meta'>Count: ");
    client.print(photoCount);
    client.print(" | Est remaining: ");
    client.print(remainingPhotos);
    client.print("<br>Free: ");
    client.print(formatBytes(freeBytes));
    client.print(" / ");
    client.print(formatBytes(totalBytes));
    client.println("</div>"
                   "<div style='display:flex;gap:8px;'>"
                   "<button class='btn2' onclick='downloadAllAsZip()'>Download all as ZIP</button>"
                   "<a class='btn' href='/delete-all' onclick='return confirm(\"Delete all photos?\");'>Delete all photos</a>"
                   "</div>"
                   "</div><div class='grid'>");
    for (const auto &entry : photos)
    {
        const String &name = entry.name;
        const size_t fileSize = entry.size;

        Serial.printf("Export: listing %s\n", name.c_str());
        client.print("<div class='card'>");
        client.print("<div class='name'>");
        client.print(name);
        client.print("</div>");
        client.print("<div class='size'>");
        client.print(formatBytes(fileSize));
        client.print("</div>");
        client.print("<div>");
        client.print("<a class='btn' href=\"/delete?name=");
        client.print(name);
        client.print("\" onclick='return confirm(\"Delete this photo?\");'>Delete</a> ");
        client.print("<a class='btn2' href=\"/file?name=");
        client.print(name);
        client.print("\">Download</a>");
        client.print("</div>");
        client.println("</div>");
    }
    client.println("</div></body></html>");
    client.stop();
}

void enterDeepSleep()
{
    // Mirror the sleep flow from teste.cpp_: hold power, configure wake, wait for button release, then sleep
    gpio_hold_en((gpio_num_t)POWER_HOLD_PIN);
    gpio_deep_sleep_hold_en();
    // Tear down peripherals to cut current
    if (exportServerActive)
    {
        stopExportServer();
    }
    WiFi.mode(WIFI_OFF);
    btStop();

    sensor_t *s = esp_camera_sensor_get();

    s->set_reg(s, 0x3008, 0xff, 0x01); // banksel --- ? Is this needed for ov3660? Is there only 1 bank
    delay(100);
    s->set_reg(s, 0x3008, 0xff, 0x80); // reset (we do this to clear the sensor registries, it seems to get more consistent images this way)
    delay(100);
    s->set_reg(s, 0x3008, 0xff, 0x40); // stand by  (sensor, register, mask, value)
    delay(100);

    esp_camera_deinit();
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
    display.drawStr(3, 11, menuLabels[menuIndex]);
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
        if (exportWifiReady)
        {
            String ip = WiFi.localIP().toString();
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

        startExportServer();
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
    capturePhotoToJpg();

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
    display.setPowerSave(0); // wake OLED after init
    display.setDisplayRotation(U8G2_R2);
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_mf);
    display.drawFrame(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    display.drawStr(10, 12, "Start");

    display.sendBuffer(); // transfer internal memory to the display

    if (!initCamera(LIVE_PIXEL_FORMAT, LIVE_FRAME_SIZE))
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
    if (!inMenu && exportServerActive)
    {
        stopExportServer();
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
        if (exportServerActive)
        {
            WiFiClient client = exportServer.available();
            if (client)
            {
                handleExportClient(client);
            }
        }
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

    // Capture a frame
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
        Serial.println("Camera capture failed");
        return;
    }

    display.clearBuffer(); // clear the internal memory

    // Create a temporary buffer for Floyd-Steinberg dithering
    // We need signed int16_t to handle error diffusion (can be negative)
    int16_t (*buffer)[SCREEN_WIDTH] = ditherBuffer;

    // Aspect-correct downsample with center crop
    const float scale_x = static_cast<float>(fb->width) / SCREEN_WIDTH;
    const float scale_y = static_cast<float>(fb->height) / SCREEN_HEIGHT;
    const float scale = (scale_x < scale_y) ? scale_x : scale_y; // use same scale to avoid squish

    // Crop the dimension with extra room (centered)
    const float crop_x = (fb->width - (SCREEN_WIDTH * scale)) * 0.5f;
    const float crop_y = (fb->height - (SCREEN_HEIGHT * scale)) * 0.5f;

    // Downsample the camera image into our buffer
    for (int y = 0; y < SCREEN_HEIGHT; y++)
    {
        for (int x = 0; x < SCREEN_WIDTH; x++)
        {
            int cam_x = static_cast<int>(crop_x + x * scale);
            int cam_y = static_cast<int>(crop_y + y * scale);
            int pixel_index = cam_y * fb->width + cam_x;

            // Get pixel value (assuming grayscale)
            buffer[y][x] = fb->buf[pixel_index];
        }
    }

    // Apply Floyd-Steinberg dithering
    for (int y = 0; y < SCREEN_HEIGHT; y++)
    {
        for (int x = 0; x < SCREEN_WIDTH; x++)
        {
            int16_t oldPixel = buffer[y][x];

            // Threshold at 128 to determine if pixel should be black or white
            int16_t newPixel = (oldPixel > 128) ? 255 : 0;

            // Draw pixel on display if it should be white
            if (newPixel > 128)
            {
                display.drawPixel(x, y); // sensor already v-flipped; draw as-is
            }

            // Calculate quantization error
            int16_t error = oldPixel - newPixel;

            // Distribute error to neighboring pixels using Floyd-Steinberg weights:
            //         current    7/16
            //   3/16    5/16    1/16

            if (x + 1 < SCREEN_WIDTH)
            {
                buffer[y][x + 1] += (error * 7) / 16; // Right pixel
            }
            if (y + 1 < SCREEN_HEIGHT)
            {
                if (x > 0)
                {
                    buffer[y + 1][x - 1] += (error * 3) / 16; // Bottom-left pixel
                }
                buffer[y + 1][x] += (error * 5) / 16; // Bottom pixel
                if (x + 1 < SCREEN_WIDTH)
                {
                    buffer[y + 1][x + 1] += (error * 1) / 16; // Bottom-right pixel
                }
            }
        }
    }

    // Release the frame
    esp_camera_fb_return(fb);
    display.sendBuffer();
}