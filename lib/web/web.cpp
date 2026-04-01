#include "web.h"

#include <WiFi.h>
#include <LittleFS.h>
#include <vector>
#include <algorithm>

#include "../../include/config.h"

namespace
{
  WiFiServer exportServer(80);
  bool exportServerActive = false;
  bool exportWifiReady = false;

  String formatBytes(size_t bytes)
  {
    if (bytes < 1024)
      return String(bytes) + " B";
    if (bytes < 1024 * 1024)
      return String(bytes / 1024.0, 1) + " KB";
    return String(bytes / 1024.0 / 1024.0, 2) + " MB";
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
} // namespace

namespace WebExport
{
  void stop()
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

  bool start()
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
    const uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_CONNECT_TIMEOUT_MS)
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

  void poll()
  {
    if (!exportServerActive)
      return;

    WiFiClient client = exportServer.available();
    if (client)
    {
      handleExportClient(client);
    }
  }

  bool isActive()
  {
    return exportServerActive;
  }

  bool isWifiReady()
  {
    return exportWifiReady;
  }

  String localIP()
  {
    return WiFi.localIP().toString();
  }
} // namespace WebExport
