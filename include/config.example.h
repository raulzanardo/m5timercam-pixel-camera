// Example configuration without secrets. Copy to config.h and fill real values.
#pragma once

#define SCREEN_WIDTH 64     // OLED display width in pixels
#define SCREEN_HEIGHT 32    // OLED display height in pixels
#define OLED_RESET -1       // Reset pin
#define SCREEN_ADDRESS 0x3C // I2C address
#define BUTTON_PIN 37       // External button pin

// Wi-Fi credentials for export server
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"

// UI/menu constants
#define MENU_LABEL_OFF "off"
#define MENU_LABEL_EXPORT "export"
#define MENU_LABEL_FILTER "filter"
#define MENU_LABEL_STATUS "status"
#define MENU_COUNT 4

// Returns a string literal for the selected menu index
#define MENU_LABEL(index) ((index) == 0 ? MENU_LABEL_OFF : (index) == 1 ? MENU_LABEL_EXPORT \
                                                       : (index) == 2   ? MENU_LABEL_FILTER \
                                                                        : MENU_LABEL_STATUS)

// Timing/power/camera constants
#define TOAST_DURATION_MS 800
#define STATUS_REFRESH_MS 5000
#define PHOTO_LED_BRIGHTNESS 80 // lower brightness to cut LED draw (10-bit duty)
#define PHOTO_LED_DURATION_MS 150
#define CPU_FREQ_LOW_MHZ 80
#define CPU_FREQ_HIGH_MHZ 240 // use full speed when exporting
#define XCLK_FREQ_HZ 10000000 // lower clock to cut active power
#define CAPTURE_XCLK_FREQ_HZ 20000000
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define EST_PHOTO_BYTES (100 * 1024)

#define LIVE_PIXEL_FORMAT PIXFORMAT_GRAYSCALE
#define LIVE_FRAME_SIZE FRAMESIZE_QQVGA
#define CAPTURE_PIXEL_FORMAT PIXFORMAT_RGB565
#define CAPTURE_FRAME_SIZE FRAMESIZE_QVGA
#define CAPTURE_FB_COUNT 2
#define CAPTURE_WARMUP_FRAMES 1
#define CAPTURE_WARMUP_DELAY_MS 12
#define JPG_ENCODE_QUALITY 80
