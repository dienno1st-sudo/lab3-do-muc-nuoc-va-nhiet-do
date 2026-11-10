#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

// ================= CHÂN KẾT NỐI =================
constexpr int TRIG  = 18;
constexpr int ECHO  = 19;
constexpr int TEMP  = 4;
constexpr int PUMP  = 25;
constexpr int VALVE = 26;

// Relay npn trong sơ đồ Wokwi kích ở mức HIGH
constexpr int VALVE_ON_LEVEL = HIGH;

// ================= MQTT =================
const char *BROKER = "broker.hivemq.com";
const char *DATA_TOPIC = "nhungblt/de4/ea9c235d/data";
const char *CMD_TOPIC  = "nhungblt/de4/ea9c235d/cmd";

WiFiClient net;
PubSubClient mqtt(net);

// ================= CẢM BIẾN NHIỆT ĐỘ =================
OneWire oneWire(TEMP);
DallasTemperature waterTemp(&oneWire);

// ================= KIỂU DỮ LIỆU =================
struct SensorData {
  float distance_m;
  float temperature_c;
  uint32_t sample_ms;
};

struct OutputState {
  int pump_percent;
  bool valve_open;
  bool manual;
};

enum Command {
  PUMP_ON,
  PUMP_OFF,
  VALVE_ON,
  VALVE_OFF,
  AUTO_MODE
};

// ================= HÀNG ĐỢI =================
QueueHandle_t dataQueue;
QueueHandle_t dataAlert;
QueueHandle_t commandQueue;
QueueHandle_t stateQueue;

// ================= NHẬN LỆNH MQTT =================
void mqttCallback(char *topic, byte *payload,
                  unsigned int length) {
  String text;

  for (unsigned int i = 0; i < length; i++) {
    text += (char)payload[i];
  }

  text.trim();
  text.toUpperCase();

  Command cmd;

  if (text == "PUMP_ON")
    cmd = PUMP_ON;
  else if (text == "PUMP_OFF")
    cmd = PUMP_OFF;
  else if (text == "VALVE_ON")
    cmd = VALVE_ON;
  else if (text == "VALVE_OFF")
    cmd = VALVE_OFF;
  else if (text == "AUTO")
    cmd = AUTO_MODE;
  else
    return;

  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    Serial.println("Command queue full");
  }
}

// ================= CẬP NHẬT BƠM VÀ VAN =================
void applyOutputs(const OutputState &s) {
  // Tắt cả hai trước khi cập nhật trạng thái
  analogWrite(PUMP, 0);
  digitalWrite(VALVE, !VALVE_ON_LEVEL);

  if (s.pump_percent > 0) {
    int duty = s.pump_percent * 255 / 100;
    analogWrite(PUMP, duty);
  }
  else if (s.valve_open) {
    digitalWrite(VALVE, VALVE_ON_LEVEL);
  }

  xQueueOverwrite(stateQueue, &s);
}

// ================= TASK ĐỌC DATA — CORE 1 =================
void taskRead(void *) {
  waterTemp.begin();
  waterTemp.setResolution(12);

  for (;;) {
    // Phát xung kích HC-SR04
    digitalWrite(TRIG, LOW);
    delayMicroseconds(2);

    digitalWrite(TRIG, HIGH);
    delayMicroseconds(10);

    digitalWrite(TRIG, LOW);

    // Timeout 30 ms
    unsigned long us = pulseIn(ECHO, HIGH, 30000);

    // Khoảng cách từ cảm biến đến mặt nước, đơn vị mét
    float distance = us ? us / 58.0f / 100.0f : NAN;

    // Đọc nhiệt độ nước
    waterTemp.requestTemperatures();
    float t = waterTemp.getTempCByIndex(0);

    if (t == DEVICE_DISCONNECTED_C) {
      t = NAN;
    }

    SensorData data = {
      distance,
      t,
      millis()
    };

    // Gửi đầy đủ dữ liệu cho task truyền
    xQueueOverwrite(dataQueue, &data);

    // Gửi riêng khoảng cách cho task cảnh báo
    xQueueOverwrite(dataAlert, &distance);

    Serial.printf(
      "[READ core %d] d=%.2f m | T=%.2f C\n",
      xPortGetCoreID(), distance, t
    );

    // Chuyển đổi nhiệt độ 12 bit mất khoảng 750 ms
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ================= PHÂN CHIA VÙNG KHOẢNG CÁCH =================
int distanceZone(float d) {
  if (!isfinite(d))
    return -1;

  if (d < 0.5f)
    return 1;  // Mở van xả 10 giây

  if (d >= 1.0f && d < 1.5f)
    return 2;  // Bơm PWM 50% trong 10 giây

  if (d >= 1.5f && d <= 2.0f)
    return 3;  // Bơm PWM 100% trong 20 giây

  return 0;
}

// ================= TASK CẢNH BÁO — CORE 1 =================
void taskAlert(void *) {
  OutputState s = {0, false, false};

  float distance = NAN;

  uint32_t lastSample = 0;
  uint32_t started = 0;
  uint32_t duration = 0;

  int servedZone = -1;
  bool running = false;

  applyOutputs(s);

  for (;;) {
    // ---------- Nhận lệnh điều khiển ----------
    Command cmd;

    while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE) {
      running = false;

      if (cmd == AUTO_MODE) {
        s = {0, false, false};
        servedZone = -1;
      }
      else {
        s.manual = true;

        if (cmd == PUMP_ON) {
          s.pump_percent = 100;
          s.valve_open = false;
        }

        if (cmd == PUMP_OFF) {
          s.pump_percent = 0;
        }

        if (cmd == VALVE_ON) {
          s.pump_percent = 0;
          s.valve_open = true;
        }

        if (cmd == VALVE_OFF) {
          s.valve_open = false;
        }
      }

      applyOutputs(s);
    }

    // ---------- Nhận khoảng cách mới ----------
    if (xQueueReceive(dataAlert, &distance, 0) == pdTRUE) {
      lastSample = millis();
    }

    uint32_t now = millis();
    int zone = distanceZone(distance);

    // ---------- Chế độ tự động ----------
    if (!s.manual) {
      // Lỗi khoảng cách hoặc quá 3 giây không có dữ liệu
      if (zone < 0 ||
          (uint32_t)(now - lastSample) > 3000) {

        if (running || s.pump_percent || s.valve_open) {
          s.pump_percent = 0;
          s.valve_open = false;
          applyOutputs(s);
        }

        running = false;
        servedZone = -1;
      }
      else {
        // Hết thời gian thì dừng bơm/van
        if (running &&
            (uint32_t)(now - started) >= duration) {

          s.pump_percent = 0;
          s.valve_open = false;
          running = false;

          applyOutputs(s);

          Serial.println(
            "[AUTO] Het thoi gian, dung bom/van"
          );
        }

        // Mỗi vùng kích hoạt một chu kỳ
        if (!running && zone != servedZone) {
          servedZone = zone;

          if (zone == 2)
            s.pump_percent = 50;
          else if (zone == 3)
            s.pump_percent = 100;
          else
            s.pump_percent = 0;

          s.valve_open = (zone == 1);

          duration = (zone == 3) ? 20000 : 10000;
          running = (zone != 0);
          started = now;

          applyOutputs(s);

          Serial.printf(
            "[AUTO] zone=%d pump=%d%% valve=%d "
            "duration=%lu ms\n",
            zone,
            s.pump_percent,
            s.valve_open,
            (unsigned long)(running ? duration : 0)
          );
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ================= TASK TRUYỀN DATA — CORE 0 =================
void taskTransmit(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.begin("Wokwi-GUEST", "", 6);

  mqtt.setServer(BROKER, 1883);
  mqtt.setCallback(mqttCallback);
  mqtt.setSocketTimeout(2);

  uint32_t retry = 0;
  uint32_t sent = 0;
  bool firstAttempt = true;

  for (;;) {
    // ---------- Lệnh Serial để thử mạch ----------
    if (Serial.available()) {
      char c = Serial.read();

      Command cmd;
      bool valid = true;

      switch (c) {
        case 'p': cmd = PUMP_ON;   break;
        case 's': cmd = PUMP_OFF;  break;
        case 'v': cmd = VALVE_ON;  break;
        case 'x': cmd = VALVE_OFF; break;
        case 'a': cmd = AUTO_MODE; break;
        default: valid = false;
      }

      if (valid) {
        xQueueSend(commandQueue, &cmd, 0);
      }
    }

    uint32_t now = millis();

    // ---------- Kết nối lại MQTT ----------
    if (WiFi.status() == WL_CONNECTED &&
        !mqtt.connected() &&
        (firstAttempt ||
         (uint32_t)(now - retry) >= 5000)) {

      firstAttempt = false;
      retry = now;

      String id =
        "de4-" +
        String((uint32_t)ESP.getEfuseMac(), HEX) +
        "-" +
        String(esp_random(), HEX);

      if (mqtt.connect(id.c_str())) {
        mqtt.subscribe(CMD_TOPIC);

        Serial.printf(
          "[MQTT core %d] Connected. data: %s\n",
          xPortGetCoreID(), DATA_TOPIC
        );
      }
    }

    // ---------- Gửi dữ liệu mỗi giây ----------
    if (mqtt.connected()) {
      mqtt.loop();

      if ((uint32_t)(now - sent) >= 1000) {
        SensorData d;
        OutputState s;

        if (xQueuePeek(dataQueue, &d, 0) == pdTRUE &&
            xQueuePeek(stateQueue, &s, 0) == pdTRUE) {

          char ds[24];
          char ts[24];
          char json[256];

          if (isfinite(d.distance_m)) {
            snprintf(
              ds, sizeof(ds), "%.3f", d.distance_m
            );
          }
          else {
            strcpy(ds, "null");
          }

          if (isfinite(d.temperature_c)) {
            snprintf(
              ts, sizeof(ts), "%.2f", d.temperature_c
            );
          }
          else {
            strcpy(ts, "null");
          }

          snprintf(
            json,
            sizeof(json),
            "{\"distance_m\":%s,"
            "\"temperature_c\":%s,"
            "\"pump_percent\":%d,"
            "\"valve_open\":%s,"
            "\"mode\":\"%s\","
            "\"sample_ms\":%lu}",
            ds,
            ts,
            s.pump_percent,
            s.valve_open ? "true" : "false",
            s.manual ? "manual" : "auto",
            (unsigned long)d.sample_ms
          );

          mqtt.publish(DATA_TOPIC, json);
        }

        sent = now;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ================= KHỞI TẠO =================
void setup() {
  Serial.begin(115200);

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(PUMP, OUTPUT);
  pinMode(VALVE, OUTPUT);

  digitalWrite(VALVE, !VALVE_ON_LEVEL);
  analogWrite(PUMP, 0);

  dataQueue = xQueueCreate(1, sizeof(SensorData));
  dataAlert = xQueueCreate(1, sizeof(float));

  commandQueue = xQueueCreate(8, sizeof(Command));
  stateQueue = xQueueCreate(1, sizeof(OutputState));

  if (!dataQueue || !dataAlert ||
      !commandQueue || !stateQueue) {

    Serial.println("Khong tao duoc queue");

    while (true) {
      delay(1000);
    }
  }

  Serial.println(
    "Serial: p=bom ON, s=bom OFF, "
    "v=van ON, x=van OFF, a=AUTO"
  );

  BaseType_t a = xTaskCreatePinnedToCore(
    taskRead, "Read",
    4096, nullptr, 1, nullptr, 1
  );

  BaseType_t b = xTaskCreatePinnedToCore(
    taskAlert, "Alert",
    4096, nullptr, 2, nullptr, 1
  );

  BaseType_t c = xTaskCreatePinnedToCore(
    taskTransmit, "Transmit",
    6144, nullptr, 1, nullptr, 0
  );

  if (a != pdPASS || b != pdPASS || c != pdPASS) {
    Serial.println("Task creation failed");
    ESP.restart();
  }
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}