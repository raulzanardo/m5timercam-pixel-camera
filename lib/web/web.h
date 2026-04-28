#pragma once

#include <Arduino.h>

namespace WebExport
{
  void stop();
  bool start();
  void poll();
  bool isActive();
  bool isWifiReady();
  bool isPreviewMode();
  String localIP();
} // namespace WebExport
