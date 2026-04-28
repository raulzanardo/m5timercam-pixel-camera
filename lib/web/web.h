#pragma once

#include <Arduino.h>

namespace WebExport
{
  void stop();
  bool start(bool previewOnly = false);
  void poll();
  bool isActive();
  bool isWifiReady();
  bool isPreviewMode();
  String localIP();
} // namespace WebExport
