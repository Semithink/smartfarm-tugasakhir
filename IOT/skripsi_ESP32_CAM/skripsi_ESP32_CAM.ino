/*
  esp32_cam_dashboard_capture.ino

  Fungsi utama:
  1. Menjalankan ESP32-CAM AI Thinker sebagai capture server HTTP.
  2. Mode ringan untuk dashboard:
     - ESP32-CAM masuk ke WiFi lokal sebagai STA.
     - Laptop/backend mengambil citra dari IP WiFi lokal ESP32-CAM.
  3. Access Point bawaan hanya fallback bila WiFi lokal gagal:
     - SSID: ESP32-CAM-TEST
     - IP AP: 192.168.4.1
  4. Endpoint yang tetap tersedia:
     - GET /
     - GET /health
     - GET /status
     - GET /capture
     - GET /sd/list
     - GET /sd/download?file=nama.jpg
     - GET /sd/ack?file=nama.jpg
  5. Capture tetap dibackup ke microSD folder /pending bila microSD siap.

  Cara pakai dashboard:
  - Upload sketch ini ke ESP32-CAM.
  - Buka Serial Monitor.
  - Lihat STA IP jika berhasil connect ke WiFi lokal.
  - Set capture_dataset.py / capture_reporter.py agar mengambil gambar dari:
      http://STA_IP_ESP32_CAM/capture
    contoh:
      http://192.168.1.20/capture
  - Jika WiFi lokal gagal, AP fallback aktif dan laptop bisa connect ke AP ESP32-CAM lalu pakai:
      http://192.168.4.1/capture
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <time.h>
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"

// ===========================
// AI Thinker ESP32-CAM pins
// ===========================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define LED_GPIO_NUM       4

// ===========================
// WiFi STA + AP fallback config
// ===========================
const char* AP_SSID = "ESP32-CAM-TEST";
const char* AP_PASS = "12345678";

// Set true agar ESP32-CAM ikut WiFi lokal yang sama dengan laptop/backend.
const bool ENABLE_STA = true;

// Sistem utama memakai WiFi lokal sebagai pusat. AP tidak dipakai untuk alur dashboard.
const bool ENABLE_AP_FALLBACK = false;

// Ganti sesuai WiFi laptop/backend.
// const char* STA_SSID = "";
// const char* STA_PASS = "";

const char* STA_SSID = "skripsi";
const char* STA_PASS = "skripsi876";

const unsigned long STA_CONNECT_TIMEOUT_MS = 15000;
const char* BACKEND_CAPTURE_INGEST_URL = "http://192.168.2.179:8000/api/captures/ingest";
const unsigned long AUTO_CAPTURE_INTERVAL_MS = 15000;
const bool ENABLE_AUTO_CAPTURE_POST = false;
const long GMT_OFFSET_SEC = 7 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

WebServer server(80);

bool cameraOK = false;
String cameraMsg = "belum dicek";

bool sdOK = false;
String sdMsg = "belum dicek";

bool staOK = false;
String staMsg = "belum dicek";

bool apOK = false;
String apMsg = "belum dicek";

unsigned long lastPrint = 0;
uint32_t captureSeq = 0;
unsigned long totalCaptureCount = 0;
unsigned long totalSdBackupSuccess = 0;
unsigned long totalSdBackupFailed = 0;
unsigned long totalBackendPostSuccess = 0;
unsigned long totalBackendPostFailed = 0;
unsigned long lastCaptureMillis = 0;
unsigned long lastBackendPostLatencyMs = 0;
int lastBackendHttpStatus = 0;
size_t lastCaptureBytes = 0;
String lastSdFileName = "";
String lastSdBackupStatus = "belum ada capture";
String lastBackendError = "";
unsigned long lastAutoCaptureMs = 0;

const char* SD_PENDING_DIR = "/pending";

// ===========================
// Utility
// ===========================
String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "\\r");
  return s;
}

bool isSafeFileName(String name) {
  if (name.length() == 0) return false;
  if (name.indexOf("..") >= 0) return false;
  if (name.indexOf("/") >= 0) return false;
  if (name.indexOf("\\") >= 0) return false;
  if (!name.endsWith(".jpg")) return false;
  return true;
}

String makePendingFileName() {
  captureSeq++;
  struct tm timeInfo;
  char buf[80];
  if (getLocalTime(&timeInfo, 1000)) {
    strftime(buf, sizeof(buf), "cabai_%Y%m%d_%H%M%S.jpg", &timeInfo);
  } else {
    snprintf(buf, sizeof(buf), "cabai_%05lu_%lu.jpg", (unsigned long)captureSeq, (unsigned long)millis());
  }
  return String(buf);
}

String ipToString(IPAddress ip) {
  return ip.toString();
}

String primaryIpString() {
  if (staOK && WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  if (apOK) {
    return WiFi.softAPIP().toString();
  }
  return "0.0.0.0";
}

String primaryBaseUrl() {
  return String("http://") + primaryIpString();
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Expose-Headers", "X-SD-Backup, X-SD-Filename");
}

void sendOptions() {
  addCorsHeaders();
  server.send(204);
}

// ===========================
// Start STA, AP fallback only if needed
// ===========================
bool startNetwork() {
  Serial.println("[NET] Menyalakan WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  delay(500);

  staOK = false;
  apOK = false;

  if (ENABLE_STA) {
    Serial.println("[STA] Menghubungkan ke WiFi lokal...");
    Serial.print("[STA] SSID: ");
    Serial.println(STA_SSID);

    WiFi.begin(STA_SSID, STA_PASS);
    unsigned long startMs = millis();

    while (WiFi.status() != WL_CONNECTED && millis() - startMs < STA_CONNECT_TIMEOUT_MS) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    staOK = true;
    staMsg = "WiFi lokal terhubung";
    Serial.println("[STA] " + staMsg);
      Serial.print("[STA] IP STA  : ");
      Serial.println(WiFi.localIP());
      Serial.print("[STA] Capture : http://");
      Serial.print(WiFi.localIP());
      Serial.println("/capture");
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.google.com");
      apMsg = "AP fallback tidak aktif karena STA berhasil";
      return true;
    } else {
      staMsg = "gagal connect WiFi lokal";
      Serial.println("[STA] " + staMsg);
    }
  } else {
    staMsg = "STA dinonaktifkan";
    Serial.println("[STA] " + staMsg);
  }

  if (!ENABLE_AP_FALLBACK) {
    apMsg = "AP fallback dinonaktifkan";
    Serial.println("[AP] " + apMsg);
    return false;
  }

  Serial.println("[AP] Menyalakan AP fallback...");
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  delay(300);

  apOK = WiFi.softAP(AP_SSID, AP_PASS);
  if (!apOK) {
    apMsg = "GAGAL membuat Access Point fallback";
    Serial.println("[AP] " + apMsg);
  } else {
    apMsg = "AP fallback aktif";
    Serial.println("[AP] " + apMsg);
    Serial.print("[AP] SSID    : ");
    Serial.println(AP_SSID);
    Serial.print("[AP] Password: ");
    Serial.println(AP_PASS);
    Serial.print("[AP] IP AP   : ");
    Serial.println(WiFi.softAPIP());
  }

  return apOK || staOK;
}

// ===========================
// MicroSD init
// ===========================
bool initMicroSD() {
  Serial.println("[SD] Mount microSD...");

  // true = 1-bit mode, lebih aman untuk ESP32-CAM AI Thinker.
  if (!SD_MMC.begin("/sdcard", true)) {
    sdMsg = "gagal mount microSD";
    Serial.println("[SD] " + sdMsg);
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    sdMsg = "microSD tidak terdeteksi";
    Serial.println("[SD] " + sdMsg);
    return false;
  }

  if (!SD_MMC.exists(SD_PENDING_DIR)) {
    if (!SD_MMC.mkdir(SD_PENDING_DIR)) {
      sdMsg = "gagal membuat folder /pending";
      Serial.println("[SD] " + sdMsg);
      return false;
    }
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  sdMsg = "microSD siap, size=" + String((unsigned long)cardSize) + "MB";

  Serial.println("[SD] " + sdMsg);
  return true;
}

size_t countPendingFiles() {
  if (!sdOK) return 0;

  File root = SD_MMC.open(SD_PENDING_DIR);
  if (!root || !root.isDirectory()) return 0;

  size_t count = 0;
  File file = root.openNextFile();

  while (file) {
    if (!file.isDirectory()) {
      count++;
    }
    file.close();
    file = root.openNextFile();
  }

  root.close();
  return count;
}

String pendingFilesJson() {
  String json = "[";

  if (!sdOK) {
    json += "]";
    return json;
  }

  File root = SD_MMC.open(SD_PENDING_DIR);
  if (!root || !root.isDirectory()) {
    json += "]";
    return json;
  }

  bool first = true;
  File file = root.openNextFile();

  while (file) {
    if (!file.isDirectory()) {
      String fullName = String(file.name());
      String name = fullName;

      int slashIndex = name.lastIndexOf('/');
      if (slashIndex >= 0) {
        name = name.substring(slashIndex + 1);
      }

      if (!first) json += ",";
      first = false;

      json += "{";
      json += "\"name\":\"" + jsonEscape(name) + "\",";
      json += "\"size\":" + String((unsigned long)file.size());
      json += "}";
    }

    file.close();
    file = root.openNextFile();
  }

  root.close();

  json += "]";
  return json;
}

bool saveFrameToSD(camera_fb_t *fb, String fileName) {
  if (!sdOK || !fb || fileName.length() == 0) return false;

  String path = String(SD_PENDING_DIR) + "/" + fileName;
  File file = SD_MMC.open(path, FILE_WRITE);

  if (!file) {
    Serial.println("[SD] Gagal membuka file untuk tulis: " + path);
    return false;
  }

  size_t written = file.write(fb->buf, fb->len);
  file.close();

  if (written != fb->len) {
    Serial.println("[SD] Ukuran tulis tidak sesuai, hapus file: " + path);
    SD_MMC.remove(path);
    return false;
  }

  Serial.println("[SD] Backup tersimpan: " + path);
  return true;
}

bool postFrameToBackend(camera_fb_t *fb, const String& sdFileName, bool sdBackupOK) {
  if (!fb) return false;

  if (WiFi.status() != WL_CONNECTED) {
    totalBackendPostFailed++;
    lastBackendHttpStatus = -1;
    lastBackendPostLatencyMs = 0;
    lastBackendError = "WiFi lokal tidak terhubung";
    Serial.println("[POST] Gagal: " + lastBackendError);
    return false;
  }

  String filename = sdFileName.length() ? sdFileName : makePendingFileName();

  HTTPClient http;
  unsigned long postStartMs = millis();
  http.begin(BACKEND_CAPTURE_INGEST_URL);
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Device-Id", "esp32_cam_01");
  http.addHeader("X-Filename", filename);
  http.addHeader("X-SD-Backup", sdBackupOK ? "true" : "false");
  http.addHeader("X-SD-Filename", sdBackupOK ? sdFileName : "");

  int httpCode = http.POST(fb->buf, fb->len);
  lastBackendPostLatencyMs = millis() - postStartMs;
  lastBackendHttpStatus = httpCode;

  if (httpCode >= 200 && httpCode < 300) {
    totalBackendPostSuccess++;
    lastBackendError = "";
    Serial.print("[POST] OK HTTP ");
    Serial.print(httpCode);
    Serial.print(" | bytes: ");
    Serial.println((unsigned long)fb->len);
    http.end();
    return true;
  }

  totalBackendPostFailed++;
  lastBackendError = "POST gagal, HTTP status: " + String(httpCode);
  Serial.println("[POST] " + lastBackendError);
  http.end();
  return false;
}

void captureBackupAndPostToBackend() {
  if (!cameraOK) {
    Serial.println("[AUTO] Kamera tidak siap: " + cameraMsg);
    return;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[AUTO] Gagal ambil frame dari kamera");
    return;
  }

  totalCaptureCount++;
  lastCaptureMillis = millis();
  lastCaptureBytes = fb->len;

  String sdFileName = "";
  bool sdBackupOK = false;

  if (sdOK) {
    sdFileName = makePendingFileName();
    sdBackupOK = saveFrameToSD(fb, sdFileName);
    if (sdBackupOK) {
      totalSdBackupSuccess++;
      lastSdFileName = sdFileName;
      lastSdBackupStatus = "backup tersimpan";
    } else {
      totalSdBackupFailed++;
      lastSdBackupStatus = "gagal backup ke microSD";
      sdFileName = "";
    }
  } else {
    totalSdBackupFailed++;
    lastSdBackupStatus = "microSD tidak siap";
  }

  postFrameToBackend(fb, sdFileName, sdBackupOK);
  esp_camera_fb_return(fb);
}

// ===========================
// Camera init
// ===========================
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;

  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_QVGA;   // 320x240
    config.jpeg_quality = 10;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size   = FRAMESIZE_QQVGA;  // 160x120
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    cameraMsg = "gagal init, err=0x" + String((uint32_t)err, HEX);
    Serial.println("[CAM] Camera init FAILED");
    Serial.println("[CAM] " + cameraMsg);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();

  if (s) {
    if (psramFound()) {
      s->set_framesize(s, FRAMESIZE_QVGA);
      s->set_quality(s, 10);
    } else {
      s->set_framesize(s, FRAMESIZE_QQVGA);
      s->set_quality(s, 12);
    }

    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
  }

  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);

  cameraMsg = "init sukses";
  Serial.println("[CAM] Camera init OK");
  return true;
}

// ===========================
// Handlers
// ===========================
void handleRoot() {
  addCorsHeaders();
  String html;

  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32-CAM DATA CAPTURE</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;max-width:900px;margin:20px auto;padding:0 12px;line-height:1.5;}";
  html += "a{display:inline-block;margin:6px 8px 6px 0;padding:10px 14px;border:1px solid #333;border-radius:8px;text-decoration:none;color:#111;background:#f3f3f3;}";
  html += "code{background:#eee;padding:2px 6px;border-radius:4px;}";
  html += ".box{padding:12px;background:#f7f7f7;border-radius:8px;margin:12px 0;}";
  html += "</style></head><body>";

  html += "<h2>ESP32-CAM DATA CAPTURE SERVER</h2>";
  html += "<div class='box'>";
  html += "<p><b>AP fallback:</b> " + apMsg + "</p>";
  html += "<p><b>AP SSID:</b> " + String(apOK ? AP_SSID : "-") + "</p>";
  html += String("<p><b>AP IP:</b> ") + (apOK ? WiFi.softAPIP().toString() : String("-")) + "</p>";
  html += "<p><b>STA WiFi:</b> " + staMsg + "</p>";
  html += String("<p><b>STA IP:</b> ") + (staOK ? WiFi.localIP().toString() : String("-")) + "</p>";
  html += "<p><b>Primary URL:</b> <code>" + primaryBaseUrl() + "</code></p>";
  html += "<p><b>Client count:</b> " + String(WiFi.softAPgetStationNum()) + "</p>";
  html += "<p><b>Camera:</b> " + cameraMsg + "</p>";
  html += "<p><b>microSD:</b> " + sdMsg + "</p>";
  html += "<p><b>Pending SD files:</b> " + String(countPendingFiles()) + "</p>";
  html += "<p><b>Total capture:</b> " + String(totalCaptureCount) + "</p>";
  html += "<p><b>Backup SD sukses:</b> " + String(totalSdBackupSuccess) + "</p>";
  html += "<p><b>Backup SD gagal:</b> " + String(totalSdBackupFailed) + "</p>";
  html += "<p><b>Backup SD terakhir:</b> " + lastSdBackupStatus + "</p>";
  html += "<p><b>File SD terakhir:</b> " + (lastSdFileName.length() ? lastSdFileName : String("-")) + "</p>";
  html += "</div>";

  html += "<p>";
  html += "<a href='/health' target='_blank'>/health</a>";
  html += "<a href='/status' target='_blank'>/status</a>";
  html += "<a href='/capture' target='_blank'>/capture</a>";
  html += "<a href='/sd/list' target='_blank'>/sd/list</a>";
  html += "</p>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleHealth() {
  addCorsHeaders();
  String status = cameraOK ? "ok" : "error";

  String json = "{";
  json += "\"status\":\"" + status + "\",";
  json += "\"camera_ok\":" + String(cameraOK ? "true" : "false") + ",";
  json += "\"sd_ok\":" + String(sdOK ? "true" : "false") + ",";
  json += "\"sta_ok\":" + String(staOK ? "true" : "false") + ",";
  json += "\"ap_ok\":" + String(apOK ? "true" : "false") + ",";
  json += String("\"ap_ip\":\"") + (apOK ? WiFi.softAPIP().toString() : String("")) + "\",";
  json += String("\"sta_ip\":\"") + (staOK ? WiFi.localIP().toString() : String("")) + "\",";
  json += "\"capture_url\":\"" + primaryBaseUrl() + "/capture\",";
  json += "\"total_capture_count\":" + String(totalCaptureCount) + ",";
  json += "\"total_sd_backup_success\":" + String(totalSdBackupSuccess) + ",";
  json += "\"total_sd_backup_failed\":" + String(totalSdBackupFailed) + ",";
  json += "\"total_backend_post_success\":" + String(totalBackendPostSuccess) + ",";
  json += "\"total_backend_post_failed\":" + String(totalBackendPostFailed) + ",";
  json += "\"last_capture_millis\":" + String(lastCaptureMillis) + ",";
  json += "\"last_capture_bytes\":" + String((unsigned long)lastCaptureBytes) + ",";
  json += "\"last_backend_http_status\":" + String(lastBackendHttpStatus) + ",";
  json += "\"last_backend_post_latency_ms\":" + String(lastBackendPostLatencyMs) + ",";
  json += "\"last_backend_error\":\"" + jsonEscape(lastBackendError) + "\",";
  json += "\"last_sd_file\":\"" + jsonEscape(lastSdFileName) + "\",";
  json += "\"last_sd_backup_status\":\"" + jsonEscape(lastSdBackupStatus) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleStatus() {
  addCorsHeaders();
  String json = "{";
  json += "\"camera_ok\":" + String(cameraOK ? "true" : "false") + ",";
  json += "\"sd_ok\":" + String(sdOK ? "true" : "false") + ",";
  json += "\"sta_ok\":" + String(staOK ? "true" : "false") + ",";
  json += "\"ap_ok\":" + String(apOK ? "true" : "false") + ",";
  json += String("\"ap_ip\":\"") + (apOK ? WiFi.softAPIP().toString() : String("")) + "\",";
  json += String("\"sta_ip\":\"") + (staOK ? WiFi.localIP().toString() : String("")) + "\",";
  json += "\"primary_ip\":\"" + primaryIpString() + "\",";
  json += "\"capture_url\":\"" + primaryBaseUrl() + "/capture\",";
  json += String("\"ap_capture_url\":\"") + (apOK ? (String("http://") + WiFi.softAPIP().toString() + "/capture") : String("")) + "\",";
  json += String("\"sta_capture_url\":\"") + (staOK ? (String("http://") + WiFi.localIP().toString() + "/capture") : String("")) + "\",";
  json += "\"clients\":" + String(apOK ? WiFi.softAPgetStationNum() : 0) + ",";
  json += "\"pending_sd_files\":" + String(countPendingFiles()) + ",";
  json += "\"total_capture_count\":" + String(totalCaptureCount) + ",";
  json += "\"total_sd_backup_success\":" + String(totalSdBackupSuccess) + ",";
  json += "\"total_sd_backup_failed\":" + String(totalSdBackupFailed) + ",";
  json += "\"total_backend_post_success\":" + String(totalBackendPostSuccess) + ",";
  json += "\"total_backend_post_failed\":" + String(totalBackendPostFailed) + ",";
  json += "\"last_capture_millis\":" + String(lastCaptureMillis) + ",";
  json += "\"last_capture_bytes\":" + String((unsigned long)lastCaptureBytes) + ",";
  json += "\"last_backend_http_status\":" + String(lastBackendHttpStatus) + ",";
  json += "\"last_backend_post_latency_ms\":" + String(lastBackendPostLatencyMs) + ",";
  json += "\"last_backend_error\":\"" + jsonEscape(lastBackendError) + "\",";
  json += "\"last_sd_file\":\"" + jsonEscape(lastSdFileName) + "\",";
  json += "\"last_sd_backup_status\":\"" + jsonEscape(lastSdBackupStatus) + "\",";
  json += "\"camera_msg\":\"" + jsonEscape(cameraMsg) + "\",";
  json += "\"sd_msg\":\"" + jsonEscape(sdMsg) + "\",";
  json += "\"sta_msg\":\"" + jsonEscape(staMsg) + "\",";
  json += "\"ap_msg\":\"" + jsonEscape(apMsg) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleCapture() {
  addCorsHeaders();
  if (!cameraOK) {
    server.send(503, "text/plain", "Camera tidak siap: " + cameraMsg);
    return;
  }

  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {
    server.send(500, "text/plain", "Gagal ambil frame dari kamera");
    return;
  }

  totalCaptureCount++;
  lastCaptureMillis = millis();
  lastCaptureBytes = fb->len;

  String sdFileName = "";
  bool sdBackupOK = false;

  if (sdOK) {
    sdFileName = makePendingFileName();
    sdBackupOK = saveFrameToSD(fb, sdFileName);

    if (!sdBackupOK) {
      sdFileName = "";
      totalSdBackupFailed++;
      lastSdBackupStatus = "gagal backup ke microSD";
    } else {
      totalSdBackupSuccess++;
      lastSdFileName = sdFileName;
      lastSdBackupStatus = "backup tersimpan";
    }
  } else {
    totalSdBackupFailed++;
    lastSdBackupStatus = "microSD tidak siap";
    Serial.println("[SD] Backup dilewati: microSD tidak siap");
  }

  server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
  server.sendHeader("X-SD-Backup", sdBackupOK ? "true" : "false");
  server.sendHeader("X-SD-Filename", sdBackupOK ? sdFileName : "");
  server.sendHeader("X-Capture-Bytes", String((unsigned long)fb->len));
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");

  WiFiClient client = server.client();
  size_t sent = client.write(fb->buf, fb->len);

  if (sent != fb->len) {
    Serial.println("[HTTP] Warning: data terkirim tidak penuh. Backup tetap ada di microSD.");
  }

  esp_camera_fb_return(fb);
}

void handleSDList() {
  addCorsHeaders();
  if (!sdOK) {
    server.send(503, "application/json", "{\"sd_ok\":false,\"error\":\"microSD tidak siap\"}");
    return;
  }

  String json = "{";
  json += "\"sd_ok\":true,";
  json += "\"count\":" + String(countPendingFiles()) + ",";
  json += "\"files\":" + pendingFilesJson();
  json += "}";

  server.send(200, "application/json", json);
}

void handleSDDownload() {
  addCorsHeaders();
  if (!sdOK) {
    server.send(503, "text/plain", "microSD tidak siap");
    return;
  }

  String fileName = server.arg("file");

  if (!isSafeFileName(fileName)) {
    server.send(400, "text/plain", "Nama file tidak valid");
    return;
  }

  String path = String(SD_PENDING_DIR) + "/" + fileName;

  if (!SD_MMC.exists(path)) {
    server.send(404, "text/plain", "File tidak ditemukan");
    return;
  }

  File file = SD_MMC.open(path, FILE_READ);

  if (!file) {
    server.send(500, "text/plain", "Gagal membuka file");
    return;
  }

  server.sendHeader("Content-Disposition", "attachment; filename=" + fileName);
  server.streamFile(file, "image/jpeg");
  file.close();
}

void handleSDAck() {
  addCorsHeaders();
  if (!sdOK) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"microSD tidak siap\"}");
    return;
  }

  String fileName = server.arg("file");

  if (!isSafeFileName(fileName)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Nama file tidak valid\"}");
    return;
  }

  String path = String(SD_PENDING_DIR) + "/" + fileName;

  if (!SD_MMC.exists(path)) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"File tidak ditemukan\"}");
    return;
  }

  bool removed = SD_MMC.remove(path);

  String json = "{";
  json += "\"ok\":" + String(removed ? "true" : "false") + ",";
  json += "\"file\":\"" + jsonEscape(fileName) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleNotFound() {
  addCorsHeaders();
  server.send(404, "text/plain", "Not found");
}

// ===========================
// Setup
// ===========================
void setup() {
  delay(1000);
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=================================");
  Serial.println("ESP32-CAM STA-first + HTTP + CAPTURE + microSD");
  Serial.println("=================================");

  startNetwork();

  sdOK = initMicroSD();
  cameraOK = initCamera();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/capture", HTTP_GET, handleCapture);

  server.on("/sd/list", HTTP_GET, handleSDList);
  server.on("/sd/download", HTTP_GET, handleSDDownload);
  server.on("/sd/ack", HTTP_GET, handleSDAck);

  server.on("/", HTTP_OPTIONS, sendOptions);
  server.on("/health", HTTP_OPTIONS, sendOptions);
  server.on("/status", HTTP_OPTIONS, sendOptions);
  server.on("/capture", HTTP_OPTIONS, sendOptions);
  server.on("/sd/list", HTTP_OPTIONS, sendOptions);
  server.on("/sd/download", HTTP_OPTIONS, sendOptions);
  server.on("/sd/ack", HTTP_OPTIONS, sendOptions);

  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("[HTTP] Server started");
  if (staOK) {
    Serial.println("[HTTP] URL WiFi lokal untuk dashboard/backend:");
    Serial.println(String("http://") + WiFi.localIP().toString() + "/");
    Serial.println(String("http://") + WiFi.localIP().toString() + "/status");
    Serial.println(String("http://") + WiFi.localIP().toString() + "/capture");
    Serial.println(String("http://") + WiFi.localIP().toString() + "/sd/list");
    Serial.print("[POST] Backend ingest citra: ");
    Serial.println(BACKEND_CAPTURE_INGEST_URL);
  }

  if (apOK) {
    Serial.println("[HTTP] URL AP fallback:");
    Serial.println("http://192.168.4.1/");
    Serial.println("http://192.168.4.1/status");
    Serial.println("http://192.168.4.1/capture");
    Serial.println("http://192.168.4.1/sd/list");
  }
}

// ===========================
// Loop
// ===========================
void loop() {
  server.handleClient();

  if (ENABLE_STA && !apOK && !staOK && WiFi.status() == WL_CONNECTED) {
    staOK = true;
    staMsg = "WiFi lokal terhubung kembali";
  }

  if (ENABLE_STA && !apOK && staOK && WiFi.status() != WL_CONNECTED) {
    staOK = false;
    staMsg = "WiFi lokal terputus";
  }

  if (ENABLE_AUTO_CAPTURE_POST && cameraOK && WiFi.status() == WL_CONNECTED && millis() - lastAutoCaptureMs >= AUTO_CAPTURE_INTERVAL_MS) {
    lastAutoCaptureMs = millis();
    captureBackupAndPostToBackend();
  }

  if (millis() - lastPrint > 5000) {
    lastPrint = millis();

    Serial.print("[INFO] Client AP: ");
    Serial.print(apOK ? WiFi.softAPgetStationNum() : 0);
    Serial.print(" | AP IP: ");
    Serial.print(apOK ? WiFi.softAPIP().toString() : String("-"));
    Serial.print(" | STA: ");
    Serial.print(staOK ? WiFi.localIP().toString() : String("-"));
    Serial.print(" | Camera: ");
    Serial.print(cameraMsg);
    Serial.print(" | SD: ");
    Serial.print(sdMsg);
    Serial.print(" | Pending: ");
    Serial.print(countPendingFiles());
    Serial.print(" | POST: ");
    Serial.print(lastBackendHttpStatus);
    Serial.print(" | POST OK/Fail: ");
    Serial.print(totalBackendPostSuccess);
    Serial.print("/");
    Serial.println(totalBackendPostFailed);
  }
}
