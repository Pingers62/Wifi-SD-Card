#define DISABLE_FS_FUNCTIONS

#include <freertos/semphr.h>
#include <SPI.h>  // Still used by USB stack, not for SD
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "html_page.h"
#include <ESP.h>
#include "esp_system.h"
#include <Preferences.h>

// SDMMC / FATFS
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include <dirent.h>
#include <sys/stat.h>

#include "usb_msc.h"

// ---------------- General Variables ----------------
String vers = "3D Printer File Upload v1.0";
int UploadStatus = 0;

// NVS storage
Preferences prefs;

static uint8_t uploadBuf[64 * 1024];  // buffer size for sd write (64Kb)
static size_t uploadBufPos = 0;


// ---------------- WiFi ----------------
// Defaults (used if Wifi.ini file is NOT found on SD card - or is blank)
String WIFI_SSID = "Your SSID";
String WIFI_PASSWORD = "Your Password";


// ---------------- OLED ----------------
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0,
  /* reset=*/U8X8_PIN_NONE,
  /* clock=*/9,
  /* data=*/8);

// ---------------- SDMMC ----------------
static const char* MOUNT_POINT = "/sdcard";
sdmmc_card_t* sdcard = nullptr;  // Shared with MSC (see usb_msc.cpp)

// File handle for upload
FILE* uploadFileFp = nullptr;

// Web server
WebServer server(80);

// ---------------- MSC / SD coordination ----------------
SemaphoreHandle_t sdMutex;

// ---------------- Mode ----------------
enum SdOwnerMode {
  MODE_USB,
  MODE_WIFI
};

SdOwnerMode currentMode = MODE_USB;

// ---------------- Button ----------------
#define MODE_BUTTON 10
unsigned long lastButtonPress = 0;

// ---------------- OLED Helpers ----------------
void OLED(int CL, int y, const char* label) {
  if (CL == 1) u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_fub14_tf);
  int16_t w = u8g2.getStrWidth(label);
  int posx = (128 - w) / 2;
  u8g2.drawStr(posx, y, label);
  u8g2.sendBuffer();
}

void IPaddr() {
  yield();
  IPAddress myIP = WiFi.localIP();
  String ipStr = String(myIP[0]) + "." + String(myIP[1]) + "." + String(myIP[2]) + "." + String(myIP[3]);
  OLED(1, 15, "Web UI on");
  OLED(0, 40, ipStr.c_str());
}

// ---------------- Button Mode Toggle ----------------
void checkModeButton() {
  if (digitalRead(MODE_BUTTON) == LOW) {
    unsigned long now = millis();
    if (now - lastButtonPress > 500) {
      lastButtonPress = now;

      prefs.begin("mode", false);

      if (currentMode == MODE_USB) {
        // USB → WiFi
        prefs.putBool("wifi", true);
      } else {
        // WiFi → USB
        prefs.putBool("wifi", false);
      }

      prefs.end();

      OLED(1, 15, "REBOOTING");
      OLED(0, 40, "MODE");
      OLED(0, 60, "CHANGE");

      esp_restart();
    }
  }
}

// ---------------- File Listing ----------------
void handleList() {
  String modeStr = (currentMode == MODE_USB) ? "usb" : "wifi";

  if (currentMode == MODE_USB) {
    server.send(200, "application/json", "{\"mode\":\"usb\",\"files\":[]}");
    return;
  }

  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    server.send(200, "application/json", "{\"mode\":\"" + modeStr + "\",\"files\":[]}");
    return;
  }

  DIR* dir = opendir(MOUNT_POINT);
  if (!dir) {
    xSemaphoreGive(sdMutex);
    server.send(200, "application/json", "{\"mode\":\"" + modeStr + "\",\"files\":[]}");
    return;
  }

  String json = "{\"mode\":\"" + modeStr + "\",\"files\":[";
  bool first = true;

  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    String name = String(ent->d_name);
    if (name == "." || name == "..") continue;

    String fullPath = String(MOUNT_POINT) + "/" + name;
    struct stat st;
    if (stat(fullPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"" + name + "\",\"size\":" + String((uint32_t)st.st_size) + ",\"date\":0,\"time\":0}";
    }
  }

  closedir(dir);
  json += "]}";
  xSemaphoreGive(sdMutex);
  server.send(200, "application/json", json);
}


void handleRoot() {
  String html = String(MAIN_PAGE);
  html.replace("%VERSION%", vers);

  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");

  server.send(200, "text/html", html);
}

void handleWebToggleMode() {
  prefs.begin("mode", false);

  if (currentMode == MODE_USB) {
    // USB → WiFi (same as pressing the physical button)
    prefs.putBool("wifi", true);
  } else {
    // WiFi → USB
    prefs.putBool("wifi", false);
  }

  prefs.end();

  server.send(200, "text/plain", "Rebooting...");
  delay(200);
  esp_restart();
}

void handleUpload() {
  if (currentMode != MODE_WIFI) return;
  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    OLED(1, 15, "FILE");
    OLED(0, 50, "UPLOADING");

    String fullPath = String(MOUNT_POINT) + "/" + up.filename;
    remove(fullPath.c_str());

    uploadFileFp = fopen(fullPath.c_str(), "wb");
    uploadBufPos = 0;

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFileFp) {
      // Copy into buffer
      memcpy(uploadBuf + uploadBufPos, up.buf, up.currentSize);
      uploadBufPos += up.currentSize;

      // If buffer full → flush to SD
      if (uploadBufPos >= sizeof(uploadBuf)) {
        fwrite(uploadBuf, 1, uploadBufPos, uploadFileFp);
        uploadBufPos = 0;
      }
    }

  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFileFp) {
      // Flush remaining bytes
      if (uploadBufPos > 0) {
        fwrite(uploadBuf, 1, uploadBufPos, uploadFileFp);
      }

      fclose(uploadFileFp);
      uploadFileFp = nullptr;

      OLED(1, 15, "UPLOAD");
      OLED(0, 50, "FINISHED");
      IPaddr();
      OLED(0, 60, "WIFI MODE");
      delay(1000);
    }

    uploadBufPos = 0;
    xSemaphoreGive(sdMutex);
  }
}


void handleDownload() {
  if (currentMode != MODE_WIFI) {
    server.send(403, "text/plain", "Switch to Wi-Fi mode");
    return;
  }
  OLED(1, 15, "FILE");
  OLED(0, 50, "DOWNLOAD");
  String filename = server.arg("file");
  String fullPath = String(MOUNT_POINT) + "/" + filename;


  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    server.send(500, "text/plain", "SD busy");
    return;
  }

  FILE* f = fopen(fullPath.c_str(), "rb");
  if (!f) {
    xSemaphoreGive(sdMutex);
    server.send(404, "text/plain", "File not found");
    return;
  }

  WiFiClient client = server.client();
  struct stat st;
  stat(fullPath.c_str(), &st);
  size_t fileSize = st.st_size;

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/octet-stream");
  client.println("Content-Disposition: attachment; filename=\"" + filename + "\"");
  client.println("Content-Length: " + String(fileSize));
  client.println("Connection: close");
  client.println();

  uint8_t buf[2048];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    client.write(buf, n);
    yield();
  }

  fclose(f);
  xSemaphoreGive(sdMutex);
  OLED(1, 15, "DOWNLOAD");
  OLED(0, 50, "FINISHED");
  IPaddr();
  OLED(0, 60, "WIFI MODE");
}

void handleDelete() {
  if (currentMode != MODE_WIFI) return;
  String path = "/" + server.arg("file");
  String fullPath = String(MOUNT_POINT) + path;

  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    remove(fullPath.c_str());
    xSemaphoreGive(sdMutex);
    server.send(200, "text/plain", "Deleted");
  }
}

// ---------------- SDMMC INIT ----------------
bool init_sdmmc() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_4BIT;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

  sdmmc_slot_config_t slot_config = {
    .clk = GPIO_NUM_39,
    .cmd = GPIO_NUM_40,
    .d0 = GPIO_NUM_41,
    .d1 = GPIO_NUM_42,
    .d2 = GPIO_NUM_37,
    .d3 = GPIO_NUM_38,
    .cd = SDMMC_SLOT_NO_CD,
    .wp = SDMMC_SLOT_NO_WP,
    .width = 4,
    .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP
  };

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 8,
    .allocation_unit_size = 64 * 1024  // speed up sd writes (64)
  };

  esp_err_t ret = esp_vfs_fat_sdmmc_mount(
    "/sdcard", &host, &slot_config, &mount_config, &sdcard);

  if (ret != ESP_OK) {
    Serial.printf("SDMMC mount failed: %s\n", esp_err_to_name(ret));
    return false;
  }

  sdmmc_card_print_info(stdout, sdcard);
  return true;
}

void loadWifiConfigFromSD() {
  String folder = String(MOUNT_POINT) + "/WIFI";
  String filePath = folder + "/Wifi.ini";

  // Check folder exists
  DIR* dir = opendir(folder.c_str());
  if (!dir) {
    return;
  }
  closedir(dir);

  // Check file exists
  FILE* f = fopen(filePath.c_str(), "r");
  if (!f) {
    return;
  }

  char line[128];
  while (fgets(line, sizeof(line), f)) {
    String s = String(line);
    s.trim();

    if (s.startsWith("SSID:")) {
      WIFI_SSID = s.substring(5);
      WIFI_SSID.trim();
    } else if (s.startsWith("PW:")) {
      WIFI_PASSWORD = s.substring(3);
      WIFI_PASSWORD.trim();
    }
  }

  fclose(f);
}



// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(400000);
  u8g2.begin();

  OLED(1, 40, "STARTING");
  delay(100);

  pinMode(MODE_BUTTON, INPUT_PULLUP);

  // SDMMC init
  if (!init_sdmmc()) {
    while (!init_sdmmc()) {
      OLED(1, 40, "SD FAILED");
      delay(500);
    }
  } else {
    OLED(1, 40, "SD INIT OK");
  }

  if (!init_sdmmc()) {
    while (!init_sdmmc()) {
      OLED(1, 40, "SD FAILED");
      delay(500);
    }
  } else {
    OLED(1, 40, "SD INIT OK");
  }

  loadWifiConfigFromSD();

  sdMutex = xSemaphoreCreateMutex();
  if (!sdMutex) {
    OLED(1, 40, "MUTEX FAIL");
    while (1) delay(1000);
  }

  // -------------------------
  // MODE SELECTION AT BOOT
  // -------------------------
  currentMode = MODE_USB;  // default

  prefs.begin("mode", false);
  bool wifiFlag = prefs.getBool("wifi", false);
  prefs.end();

  if (wifiFlag) {
    currentMode = MODE_WIFI;

    prefs.begin("mode", false);
    prefs.putBool("wifi", false);
    prefs.end();
  } else if (digitalRead(MODE_BUTTON) == LOW) {
    currentMode = MODE_WIFI;
  }

  // -------------------------
  // BOOT BEHAVIOUR
  // -------------------------
  if (currentMode == MODE_USB) {
    WiFi.mode(WIFI_OFF);
    OLED(1, 30, "USB MODE");
    OLED(0, 50, "READY");
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());




    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      OLED(1, 15, WIFI_SSID.c_str());
      OLED(0, 50, "TRYING");
    }

    OLED(1, 15, WIFI_SSID.c_str());
    OLED(0, 50, "CONNECTED");
    delay(100);
    IPaddr();
    OLED(0, 60, "WIFI MODE");
  }

  usbmsc_init();
  usb_msc_mediaPresent(currentMode == MODE_USB);

  if (currentMode == MODE_WIFI) {
    server.on("/", handleRoot);
    server.on("/list", handleList);
    server.on("/download", handleDownload);
    server.on("/delete", handleDelete);
    server.on("/toggle_mode", handleWebToggleMode);
    server.on(
      "/upload", HTTP_POST, []() {
        server.send(200, "text/plain", "OK");
      },
      handleUpload);

    const char* h[] = { "Content-Length" };
    server.collectHeaders(h, 1);

    server.begin();
  }
}

// ---------------- Loop ----------------
void loop() {
  checkModeButton();
  if (currentMode == MODE_WIFI) {
    server.handleClient();
  }
  delay(1);
}
