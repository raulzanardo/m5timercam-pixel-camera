// Example configuration without secrets. Copy to config.h and fill real values.
#pragma once

#define SCREEN_WIDTH 64     // OLED display width in pixels
#define SCREEN_HEIGHT 32    // OLED display height in pixels
#define OLED_RESET -1       // Reset pin
#define SCREEN_ADDRESS 0x3C // I2C address
#define BUTTON_PIN 37       // External button pin

constexpr char WIFI_SSID[] = "YOUR_SSID";
constexpr char WIFI_PASSWORD[] = "YOUR_PASSWORD";
