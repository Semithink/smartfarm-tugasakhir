  /*
    sensor_environment_endpoint.ino

    Fungsi:
    1. Membaca sensor lingkungan:
      - DHT11: suhu dan kelembapan udara
      - Soil moisture YL-69: kelembapan tanah
      - BH1750: intensitas cahaya
    2. Menyediakan endpoint lokal ESP32:
      - GET /
      - GET /health
      - GET /status
      - GET /api/sensors/latest
      - GET /api/sensors/history
    3. Mengirim data sensor ke backend:
      - POST http://IP_LAPTOP:8000/api/sensors/ingest

    Catatan:
    - ESP32 dan laptop/backend harus berada pada WiFi yang sama.
    - Ubah WIFI_SSID, WIFI_PASSWORD, dan BACKEND_INGEST_URL sebelum upload.
  */

  #include <WiFi.h>
  #include <WebServer.h>
  #include <HTTPClient.h>
  #include <Wire.h>
  #include <BH1750.h>
  #include <DHT.h>

  // ========================
  // KONFIGURASI WIFI
  // ========================
  // const char* WIFI_SSID = "ESP32-CAM-TEST";
  // const char* WIFI_PASSWORD = "12345678";

  // const char* WIFI_SSID = "";
  // const char* WIFI_PASSWORD = "";

  const char* WIFI_SSID = "skripsi";
  const char* WIFI_PASSWORD = "skripsi876";

  // Ganti IP berikut dengan IPv4 laptop.
  // Contoh: http://192.168.1.10:8000/api/sensors/ingest
  // const char* BACKEND_INGEST_URL = "http://192.168.1.10:8000/api/sensors/ingest";
  // const char* BACKEND_INGEST_URL = "http://192.168.1.7:8000/api/sensors/ingest";
  const char* BACKEND_INGEST_URL = "http://192.168.2.179:8000/api/sensors/ingest";

  // ========================
  // IDENTITAS DEVICE
  // ========================
  const char* DEVICE_ID = "esp32_sensor_01";
  const char* FIRMWARE_VERSION = "sensor-endpoint-v1.0.0";

  // ========================
  // PIN CONFIG
  // ========================
  #define DHTPIN 4
  #define DHTTYPE DHT11

  #define SOIL_PIN 34
  #define I2C_SDA 21
  #define I2C_SCL 22

  // ========================
  // SENSOR OBJECT
  // ========================
  DHT dht(DHTPIN, DHTTYPE);
  BH1750 lightMeter;

  // ========================
  // WEB SERVER
  // ========================
  WebServer server(80);

  // ========================
  // SOIL CALIBRATION
  // ========================
  int SOIL_DRY = 3200;   // nilai ADC saat tanah kering
  int SOIL_WET = 1100;   // nilai ADC saat tanah basah

  // ========================
  // TIMER
  // ========================
  unsigned long lastRead = 0;
  const unsigned long interval = 2000;

  // ========================
  // BUFFER HISTORI SENSOR
  // ========================
  const int HISTORY_SIZE = 20;

  struct SensorReading {
    unsigned long sequence_no;
    unsigned long device_millis_ms;
    String device_timestamp_uptime;
    unsigned long interval_ms;

    float temperature_c;
    float humidity_percent;
    float light_lux;
    int soil_raw;
    int soil_percent;

    int wifi_rssi;
    uint32_t free_heap;
    unsigned long read_duration_ms;

    String status;
    String error_message;
  };

  SensorReading latestReading;
  SensorReading historyBuffer[HISTORY_SIZE];
  int historyIndex = 0;
  int historyCount = 0;

  // ========================
  // METRIK POST KE BACKEND
  // ========================
  unsigned long sequenceNo = 0;
  unsigned long totalReadCount = 0;
  unsigned long totalPostSuccess = 0;
  unsigned long totalPostFailed = 0;

  int lastBackendHttpStatus = 0;
  unsigned long lastBackendPostLatencyMs = 0;
  int lastPayloadBytes = 0;
  String lastBackendError = "";

  // ========================
  // HELPER FUNCTION
  // ========================
  int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
  }

  String getTimestamp() {
    unsigned long seconds = millis() / 1000;

    int hrs = seconds / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;

    char buffer[20];
    sprintf(buffer, "%02d:%02d:%02d", hrs, mins, secs);

    return String(buffer);
  }

  String jsonFloat(float value, int decimals) {
    if (isnan(value)) {
      return "null";
    }

    char buffer[32];
    dtostrf(value, 0, decimals, buffer);
    return String(buffer);
  }

  void addCorsHeaders() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  }

  String readingToJson(const SensorReading& r) {
    String json = "{";
    json += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
    json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\",";
    json += "\"sequence_no\":" + String(r.sequence_no) + ",";
    json += "\"device_millis_ms\":" + String(r.device_millis_ms) + ",";
    json += "\"device_timestamp_uptime\":\"" + r.device_timestamp_uptime + "\",";
    json += "\"interval_ms\":" + String(r.interval_ms) + ",";

    json += "\"temperature_c\":" + jsonFloat(r.temperature_c, 1) + ",";
    json += "\"humidity_percent\":" + jsonFloat(r.humidity_percent, 1) + ",";
    json += "\"light_lux\":" + jsonFloat(r.light_lux, 1) + ",";
    json += "\"soil_raw\":" + String(r.soil_raw) + ",";
    json += "\"soil_percent\":" + String(r.soil_percent) + ",";

    json += "\"wifi_rssi\":" + String(r.wifi_rssi) + ",";
    json += "\"free_heap\":" + String(r.free_heap) + ",";
    json += "\"read_duration_ms\":" + String(r.read_duration_ms) + ",";

    json += "\"last_backend_http_status\":" + String(lastBackendHttpStatus) + ",";
    json += "\"last_backend_post_latency_ms\":" + String(lastBackendPostLatencyMs) + ",";
    json += "\"last_payload_bytes\":" + String(lastPayloadBytes) + ",";

    json += "\"status\":\"" + r.status + "\",";
    json += "\"error_message\":\"" + r.error_message + "\"";
    json += "}";

    return json;
  }

  void saveToHistory(const SensorReading& r) {
    historyBuffer[historyIndex] = r;
    historyIndex = (historyIndex + 1) % HISTORY_SIZE;

    if (historyCount < HISTORY_SIZE) {
      historyCount++;
    }
  }

  SensorReading readSensors() {
    unsigned long startMs = millis();

    SensorReading r;
    r.sequence_no = ++sequenceNo;
    r.device_millis_ms = millis();
    r.device_timestamp_uptime = getTimestamp();
    r.interval_ms = interval;

    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();
    float lux = lightMeter.readLightLevel();

    int soilRaw = analogRead(SOIL_PIN);
    int soilPercent = map(soilRaw, SOIL_DRY, SOIL_WET, 0, 100);
    soilPercent = clampInt(soilPercent, 0, 100);

    r.temperature_c = temperature;
    r.humidity_percent = humidity;
    r.light_lux = lux;
    r.soil_raw = soilRaw;
    r.soil_percent = soilPercent;

    r.wifi_rssi = WiFi.RSSI();
    r.free_heap = ESP.getFreeHeap();
    r.read_duration_ms = millis() - startMs;

    r.status = "OK";
    r.error_message = "";

    if (isnan(humidity) || isnan(temperature)) {
      r.status = "ERROR";
      r.error_message = "DHT11 gagal membaca suhu atau kelembapan";
    }

    if (lux < 0) {
      r.status = "ERROR";
      if (r.error_message.length() > 0) {
        r.error_message += "; ";
      }
      r.error_message += "BH1750 gagal membaca intensitas cahaya";
    }

    return r;
  }

  void postSensorToBackend(const String& payload) {
    if (WiFi.status() != WL_CONNECTED) {
      totalPostFailed++;
      lastBackendHttpStatus = -1;
      lastBackendError = "WiFi tidak terhubung";
      return;
    }

    HTTPClient http;
    unsigned long postStartMs = millis();

    http.begin(BACKEND_INGEST_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Id", DEVICE_ID);
    http.addHeader("X-Firmware-Version", FIRMWARE_VERSION);

    lastPayloadBytes = payload.length();
    int httpCode = http.POST(payload);
    lastBackendPostLatencyMs = millis() - postStartMs;
    lastBackendHttpStatus = httpCode;

    if (httpCode >= 200 && httpCode < 300) {
      totalPostSuccess++;
      lastBackendError = "";
    } else {
      totalPostFailed++;
      lastBackendError = "POST gagal, HTTP status: " + String(httpCode);
    }

    http.end();
  }

  void performSensorCycle() {
    latestReading = readSensors();
    totalReadCount++;

    saveToHistory(latestReading);

    String payload = readingToJson(latestReading);
    postSensorToBackend(payload);

    Serial.printf(
      "[%s] Seq: %lu | Temp: %.1f C | Hum: %.1f %% | Lux: %.1f | Soil: %d %% raw:%d | Payload: %d bytes | POST: %d | Latency: %lu ms | Status: %s\n",
      latestReading.device_timestamp_uptime.c_str(),
      latestReading.sequence_no,
      latestReading.temperature_c,
      latestReading.humidity_percent,
      latestReading.light_lux,
      latestReading.soil_percent,
      latestReading.soil_raw,
      lastPayloadBytes,
      lastBackendHttpStatus,
      lastBackendPostLatencyMs,
      latestReading.status.c_str()
    );
  }

  // ========================
  // ENDPOINT HANDLER
  // ========================
  void handleRoot() {
    addCorsHeaders();

    String html = "";
    html += "<html><head><title>ESP32 Sensor</title></head><body>";
    html += "<h1>ESP32 Sensor Lingkungan</h1>";
    html += "<p>Device ID: " + String(DEVICE_ID) + "</p>";
    html += "<ul>";
    html += "<li><a href='/health'>/health</a></li>";
    html += "<li><a href='/status'>/status</a></li>";
    html += "<li><a href='/api/sensors/latest'>/api/sensors/latest</a></li>";
    html += "<li><a href='/api/sensors/history'>/api/sensors/history</a></li>";
    html += "</ul>";
    html += "</body></html>";

    server.send(200, "text/html", html);
  }

  void handleHealth() {
    addCorsHeaders();

    String json = "{";
    json += "\"status\":\"ok\",";
    json += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
    json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"uptime_ms\":" + String(millis()) + ",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap());
    json += "}";

    server.send(200, "application/json", json);
  }

  void handleStatus() {
    addCorsHeaders();

    String json = "{";
    json += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
    json += "\"device_type\":\"environment_sensor\",";
    json += "\"status\":\"online\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"last_read_status\":\"" + latestReading.status + "\",";
    json += "\"last_backend_http_status\":" + String(lastBackendHttpStatus) + ",";
    json += "\"last_backend_post_latency_ms\":" + String(lastBackendPostLatencyMs) + ",";
    json += "\"last_payload_bytes\":" + String(lastPayloadBytes) + ",";
    json += "\"total_read_count\":" + String(totalReadCount) + ",";
    json += "\"total_post_success\":" + String(totalPostSuccess) + ",";
    json += "\"total_post_failed\":" + String(totalPostFailed) + ",";
    json += "\"last_backend_error\":\"" + lastBackendError + "\"";
    json += "}";

    server.send(200, "application/json", json);
  }

  void handleLatestSensor() {
    addCorsHeaders();
    server.send(200, "application/json", readingToJson(latestReading));
  }

  void handleHistorySensor() {
    addCorsHeaders();

    String json = "[";

    for (int i = 0; i < historyCount; i++) {
      int idx = (historyIndex - historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;

      if (i > 0) {
        json += ",";
      }

      json += readingToJson(historyBuffer[idx]);
    }

    json += "]";
    server.send(200, "application/json", json);
  }

  void handleOptions() {
    addCorsHeaders();
    server.send(204);
  }

  void setupServerRoutes() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/health", HTTP_GET, handleHealth);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/api/sensors/latest", HTTP_GET, handleLatestSensor);
    server.on("/api/sensors/history", HTTP_GET, handleHistorySensor);

    server.on("/health", HTTP_OPTIONS, handleOptions);
    server.on("/status", HTTP_OPTIONS, handleOptions);
    server.on("/api/sensors/latest", HTTP_OPTIONS, handleOptions);
    server.on("/api/sensors/history", HTTP_OPTIONS, handleOptions);

    server.begin();
  }

  // ========================
  // SETUP
  // ========================
  void setup() {
    Serial.begin(115200);
    delay(1000);

    dht.begin();

    Wire.begin(I2C_SDA, I2C_SCL);
    lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);

    analogReadResolution(12);

    Serial.println();
    Serial.println("===== ESP32 SENSOR MONITOR STARTED =====");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Menghubungkan ke WiFi");
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi connected");
    Serial.print("IP ESP32: ");
    Serial.println(WiFi.localIP());

    setupServerRoutes();

    // Baca pertama kali agar endpoint latest langsung berisi data.
    performSensorCycle();
  }

  // ========================
  // LOOP
  // ========================
  void loop() {
    server.handleClient();

    if (millis() - lastRead >= interval) {
      lastRead = millis();
      performSensorCycle();
    }
  }
