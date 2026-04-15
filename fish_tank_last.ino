#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>

// ============================================================
//  CẤU HÌNH PHẦN CỨNG
// ============================================================
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

#define ONE_WIRE_BUS  4
#define TDS_PIN       34
#define TRIG_PIN      26
#define ECHO_PIN      25
#define TANK_HEIGHT   20
#define RELAY_IN      27
#define RELAY_OUT     14
#define RELAY_OXY     13
#define RELAY_FILTER  12
#define SERVO_PIN     16

// ============================================================
//  CẤU HÌNH MẠNG & SERVER  (lưu trong NVS — xem setup())
// ============================================================
static char s_ssid[64]        = "P2701";
static char s_pass[64]        = "12345678";
static char s_serverBase[128] = "https://fish.kolla.click";
static char s_tgToken[128]    = "";
static char s_tgChatId[32]    = "";

// ============================================================
//  NGƯỠNG
// ============================================================
#define TDS_THRESHOLD_ON   200
#define TDS_THRESHOLD_OFF  100

#define LEVEL_LOW        12.0f
#define LEVEL_LOW_STOP   12.5f
#define LEVEL_HIGH_STOP  14.5f
#define LEVEL_HIGH       15.0f

#define CHANGE_DRAIN_TO   5.0f

#define ALERT_COOLDOWN_MS (5UL * 60UL * 1000UL)

// ============================================================
//  OBJECTS
// ============================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
OneWire           oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
Servo             feederServo;
Preferences       prefs;

// ============================================================
//  SHARED DATA (bảo vệ bởi mutex)
// ============================================================
struct SensorData {
  float temp       = 0;
  float tds        = 0;
  float waterLevel = 0;
};
static SensorData        shared;
static SemaphoreHandle_t sensorMutex;

// ============================================================
//  STATE MACHINES
// ============================================================
enum ChangeModeState : uint8_t { CM_IDLE, CM_DRAINING };
static volatile ChangeModeState changeModeState = CM_IDLE;

static portMUX_TYPE  ctrlMux            = portMUX_INITIALIZER_UNLOCKED;
static volatile bool oxyStateTarget     = false;
static volatile bool oxyStateApplied    = false;
static volatile bool filterStateApplied = false;

// ── Feeding ──────────────────────────────────────────────────
static volatile bool          feedingNow    = false;
static volatile unsigned long feedStartTime = 0;

// ── Telegram message queue ───────────────────────────────────
static QueueHandle_t tgQueue;  // queue of String*

// ── Alert cooldown ───────────────────────────────────────────
enum AlertType : uint8_t {
  ALERT_WATER_HIGH = 0, ALERT_WATER_LOW,
  ALERT_QUALITY_BAD, ALERT_TEMP_HIGH, ALERT_TEMP_LOW,
  ALERT_COUNT
};
static unsigned long lastAlertTime[ALERT_COUNT] = {0};

// ============================================================
//  HÀM PHỤ TRỢ
// ============================================================
static const char* getWaterQuality(float ppm) {
  if (ppm < 100) return "Tot";
  if (ppm < 300) return "Trung binh";
  if (ppm < 600) return "Kem";
  return "Ban";
}

static void enqueueTelegram(const String& msg) {
  String* p = new String(msg);
  if (!xQueueSend(tgQueue, &p, 0)) delete p;
}

static void sendAlert(AlertType type, const String& msg) {
  unsigned long now = millis();
  if (now - lastAlertTime[type] >= ALERT_COOLDOWN_MS) {
    lastAlertTime[type] = now;
    enqueueTelegram(msg);
  }
}

static void checkAndAlert(float temp, float tds, float level) {
  if (level > 17)
    sendAlert(ALERT_WATER_HIGH, "⚠️ <b>Cảnh báo bể cá!</b>\nMực nước cao: " + String(level, 1) + " cm");
  if (level < 5 && level > 0)
    sendAlert(ALERT_WATER_LOW,  "⚠️ <b>Cảnh báo bể cá!</b>\nMực nước thấp: " + String(level, 1) + " cm");
  if (tds > 600)
    sendAlert(ALERT_QUALITY_BAD,"🚨 <b>Nước xấu!</b>\nTDS: " + String(tds, 0) + " ppm");
  if (temp > 32)
    sendAlert(ALERT_TEMP_HIGH,  "🌡️ <b>Nhiệt độ cao!</b>\n" + String(temp, 1) + "°C");
  if (temp < 20 && temp > -10)
    sendAlert(ALERT_TEMP_LOW,   "🌡️ <b>Nhiệt độ thấp!</b>\n" + String(temp, 1) + "°C");
}

static void postJsonValue(const char* path, const String& value) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(s_serverBase) + path);
  http.addHeader("Content-Type", "application/json");
  http.POST(String("{\"value\":") + value + "}");
  http.end();
}

static void postSensorData(float t, float q, float w) {
  postJsonValue("/api/v1/temperature",   String(t, 2));
  postJsonValue("/api/v1/water-quality", String(q, 2));
  postJsonValue("/api/v1/water-level",   String(w, 2));
}

static bool getIsOnFromServer(const char* path, bool& isOn) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(String(s_serverBase) + path);
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, http.getString()) && doc.containsKey("isOn")) {
      isOn = doc["isOn"];
      http.end();
      return true;
    }
  }
  http.end();
  return false;
}

static bool getShouldFeed(bool& shouldFeed) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(String(s_serverBase) + "/api/v1/feeding/should-feed");
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, http.getString()) && doc.containsKey("shouldFeed")) {
      shouldFeed = doc["shouldFeed"];
      http.end();
      return true;
    }
  }
  http.end();
  return false;
}

// ============================================================
//  TASK 1: ĐỌC HC-SR04 (FreeRTOS)
// ============================================================
static void taskReadUltrasonic(void* pv) {
  const int   NUM_SAMPLES   = 9;
  const float OUTLIER_LIMIT = 3.0f;
  const float EMA_ALPHA     = 0.08f;
  float smoothedLevel = -1.0f;

  for (;;) {
    float raw[NUM_SAMPLES];
    int   validCount = 0;

    for (int i = 0; i < NUM_SAMPLES; i++) {
      digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
      digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
      digitalWrite(TRIG_PIN, LOW);
      long dur = pulseIn(ECHO_PIN, HIGH, 30000UL);
      if (dur > 0) raw[validCount++] = dur * 0.01715f;
      vTaskDelay(pdMS_TO_TICKS(15));
    }

    if (validCount == 0) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

    for (int i = 0; i < validCount - 1; i++)
      for (int j = i + 1; j < validCount; j++)
        if (raw[j] < raw[i]) { float t = raw[i]; raw[i] = raw[j]; raw[j] = t; }
    float medianDist = raw[validCount / 2];

    if (smoothedLevel >= 0 && fabsf(medianDist - (TANK_HEIGHT - smoothedLevel)) > OUTLIER_LIMIT) {
      vTaskDelay(pdMS_TO_TICKS(200)); continue;
    }

    float rawLevel = max(0.0f, (float)TANK_HEIGHT - medianDist);
    smoothedLevel  = (smoothedLevel < 0.0f) ? rawLevel
                   : smoothedLevel * (1.0f - EMA_ALPHA) + rawLevel * EMA_ALPHA;

    if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(50))) {
      shared.waterLevel = smoothedLevel;
      xSemaphoreGive(sensorMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(300));
  }
}

// ============================================================
//  TASK 2: ĐỌC TDS (FreeRTOS — không block loop)
// ============================================================
static void taskReadTDS(void* pv) {
  const int SAMPLES = 30;
  for (;;) {
    float adcSum = 0;
    for (int i = 0; i < SAMPLES; i++) {
      adcSum += analogRead(TDS_PIN);
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    float voltage = (adcSum / SAMPLES) * (3.3f / 4095.0f);
    float tempC;
    if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(20))) {
      tempC = shared.temp;
      xSemaphoreGive(sensorMutex);
    } else { tempC = 25.0f; }

    float comp = 1.0f + 0.02f * (tempC - 25.0f);
    float ec   = (133.42f * powf(voltage, 3) - 255.86f * powf(voltage, 2) + 857.39f * voltage) * 0.5f / comp;
    float tds  = ec * 0.67f;

    if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(20))) {
      shared.tds = tds;
      xSemaphoreGive(sensorMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ============================================================
//  TASK 3: MẠNG (gộp request, xử lý Telegram queue)
// ============================================================
static void taskNetwork(void* pv) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    String* tgMsg = nullptr;
    while (xQueueReceive(tgQueue, &tgMsg, 0) == pdTRUE && tgMsg) {
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(String("https://api.telegram.org/bot") + s_tgToken + "/sendMessage");
        http.addHeader("Content-Type", "application/json");
        StaticJsonDocument<512> doc;
        doc["chat_id"] = s_tgChatId; doc["text"] = *tgMsg; doc["parse_mode"] = "HTML";
        String body; serializeJson(doc, body);
        http.POST(body); http.end();
      }
      delete tgMsg; tgMsg = nullptr;
    }

    if (WiFi.status() != WL_CONNECTED) continue;

    float t, q, w;
    if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100))) {
      t = shared.temp; q = shared.tds; w = shared.waterLevel;
      xSemaphoreGive(sensorMutex);
    } else { continue; }

    postSensorData(t, q, w);
    checkAndAlert(t, q, w);

    bool sOxy;
    if (getIsOnFromServer("/api/v1/system-control/oxygen", sOxy)) {
      portENTER_CRITICAL(&ctrlMux);
      oxyStateTarget = sOxy;
      portEXIT_CRITICAL(&ctrlMux);
    }

    bool sChg;
    if (getIsOnFromServer("/api/v1/system-control/water-drainage", sChg)) {
      if (sChg && changeModeState == CM_IDLE) {
        changeModeState = CM_DRAINING;
        enqueueTelegram("🔄 <b>Bắt đầu thay nước</b>\nĐang xả đến " + String(CHANGE_DRAIN_TO, 0) + " cm...");
      } else if (!sChg && changeModeState == CM_DRAINING) {
        changeModeState = CM_IDLE;
        enqueueTelegram("✅ <b>Thay nước hoàn tất!</b>\nChế độ tự động đã được khôi phục.");
      }
    }

    bool sFeed;
    if (getShouldFeed(sFeed) && sFeed && !feedingNow) {
      feederServo.write(55);
      feedingNow    = true;
      feedStartTime = millis();
      postJsonValue("/api/v1/feeding/ack", "true");
    }
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  prefs.begin("fishtank", true);
  prefs.getString("ssid",     s_ssid,       sizeof(s_ssid));
  prefs.getString("pass",     s_pass,       sizeof(s_pass));
  prefs.getString("server",   s_serverBase, sizeof(s_serverBase));
  prefs.getString("tgToken",  s_tgToken,    sizeof(s_tgToken));
  prefs.getString("tgChatId", s_tgChatId,   sizeof(s_tgChatId));
  prefs.end();

  display.begin();
  display.setFont(u8g2_font_6x12_tr);
  display.clearBuffer();
  display.drawStr(0, 12, "WiFi...");
  display.sendBuffer();

  sensors.begin();
  sensors.setWaitForConversion(false);
  analogReadResolution(12);

  pinMode(TRIG_PIN,     OUTPUT);
  pinMode(ECHO_PIN,     INPUT);
  pinMode(RELAY_IN,     OUTPUT);
  pinMode(RELAY_OUT,    OUTPUT);
  pinMode(RELAY_OXY,    OUTPUT);
  pinMode(RELAY_FILTER, OUTPUT);

  digitalWrite(RELAY_IN,     RELAY_OFF);
  digitalWrite(RELAY_OUT,    RELAY_OFF);
  digitalWrite(RELAY_OXY,    RELAY_OFF);
  digitalWrite(RELAY_FILTER, RELAY_OFF);

  feederServo.attach(SERVO_PIN);
  feederServo.write(5);

  sensorMutex = xSemaphoreCreateMutex();
  tgQueue     = xQueueCreate(8, sizeof(String*));

  WiFi.begin(s_ssid, s_pass);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);

  display.clearBuffer();
  display.drawStr(0, 12, WiFi.status() == WL_CONNECTED ? "WiFi OK" : "WiFi FAIL");
  display.sendBuffer();
  delay(800);

  xTaskCreate(taskReadUltrasonic, "Ultrasonic", 4096,  NULL, 3, NULL);
  xTaskCreate(taskReadTDS,        "TDS",        4096,  NULL, 2, NULL);
  xTaskCreate(taskNetwork,        "Network",    16384, NULL, 1, NULL);
}

// ============================================================
//  LOOP — nhiệt độ, relay, servo, OLED
// ============================================================
void loop() {
  static bool          tempRequested = false;
  static unsigned long lastTempReq   = 0;
  static float         latestTemp    = 0;

  if (!tempRequested) {
    sensors.requestTemperatures();
    tempRequested = true;
    lastTempReq   = millis();
  }
  if (tempRequested && millis() - lastTempReq >= 750) {
    latestTemp    = sensors.getTempCByIndex(0);
    tempRequested = false;
  }

  float tempC, tdsValue, level;
  if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(10))) {
    shared.temp = latestTemp;
    tempC    = shared.temp;
    tdsValue = shared.tds;
    level    = shared.waterLevel;
    xSemaphoreGive(sensorMutex);
  } else {
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }

  // ── Logic bơm vào / bơm ra ──────────────────────────────
  bool cmdPumpIn = false, cmdPumpOut = false;

  switch (changeModeState) {
    case CM_DRAINING:
      if (level > CHANGE_DRAIN_TO) cmdPumpOut = true;
      break;

    case CM_IDLE:
    default: {
      static bool pumpInActive = false, pumpOutActive = false;
      if (!pumpInActive  && level < LEVEL_LOW)        pumpInActive  = true;
      if ( pumpInActive  && level >= LEVEL_LOW_STOP)  pumpInActive  = false;
      if (!pumpOutActive && level > LEVEL_HIGH)       pumpOutActive = true;
      if ( pumpOutActive && level <= LEVEL_HIGH_STOP) pumpOutActive = false;
      if (pumpInActive && pumpOutActive) pumpInActive = pumpOutActive = false;
      cmdPumpIn  = pumpInActive;
      cmdPumpOut = pumpOutActive;
      break;
    }
  }

  digitalWrite(RELAY_IN,  cmdPumpIn  ? RELAY_ON : RELAY_OFF);
  digitalWrite(RELAY_OUT, cmdPumpOut ? RELAY_ON : RELAY_OFF);

  // ── Relay Filter (tắt khi đang thay nước) ───────────────
  if (changeModeState == CM_DRAINING) {
    if (filterStateApplied) {
      filterStateApplied = false;
      digitalWrite(RELAY_FILTER, RELAY_OFF);
    }
  } else if (!filterStateApplied && tdsValue > TDS_THRESHOLD_ON) {
    filterStateApplied = true;
    digitalWrite(RELAY_FILTER, RELAY_ON);
  } else if (filterStateApplied && tdsValue < TDS_THRESHOLD_OFF) {
    filterStateApplied = false;
    digitalWrite(RELAY_FILTER, RELAY_OFF);
  }

  // ── Relay OXY (ghi GPIO chỉ khi thay đổi) ───────────────
  portENTER_CRITICAL(&ctrlMux);
  bool oxyChanged = (oxyStateTarget != oxyStateApplied);
  bool oxyVal     = oxyStateTarget;
  if (oxyChanged) oxyStateApplied = oxyVal;
  portEXIT_CRITICAL(&ctrlMux);
  if (oxyChanged) digitalWrite(RELAY_OXY, oxyVal ? RELAY_ON : RELAY_OFF);

  // ── Servo cho ăn ─────────────────────────────────────────
  if (feedingNow && millis() - feedStartTime > 1000) {
    feederServo.write(5);
    feedingNow = false;
  }

  // ── OLED (500ms) ──────────────────────────────────────────
  static unsigned long lastOled = 0;
  if (millis() - lastOled >= 500) {
    lastOled = millis();
    display.clearBuffer();
    display.setCursor(0, 14); display.print("Nhiet do: "); display.print(tempC, 1);    display.print(" C");
    display.setCursor(0, 30); display.print("TDS: ");      display.print(tdsValue, 0); display.print(" ppm");
    display.setCursor(0, 46);
    switch (changeModeState) {
      case CM_DRAINING: display.print("Dang xa nuoc..."); break;
      default:          display.print(getWaterQuality(tdsValue)); break;
    }
    display.setCursor(0, 62); display.print("Nuoc: "); display.print(level, 1); display.print(" cm");
    display.sendBuffer();
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
