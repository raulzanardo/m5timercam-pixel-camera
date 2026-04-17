#ifndef FILTER_H
#define FILTER_H

#include <Arduino.h>
#include <esp_camera.h>

// Generic palette application with optional ordered dithering (Bayer)
void applyColorPalette(uint16_t *imageBuffer, int width, int height, const uint32_t *palette, int paletteSize, int bayerSize = 16);

// Auto adjust (gamma/contrast/brightness) on RGB565 framebuffer
void applyAutoAdjust(camera_fb_t *cameraFb);

// Convenience: apply Pico-8 style palette with Bayer 2x2 dithering
void applyPicoPalette(camera_fb_t *cameraFb);

// Convenience: apply Elevate palette with Bayer 2x2 dithering
void applyElevatePalette(camera_fb_t *cameraFb);

#endif // FILTER_H