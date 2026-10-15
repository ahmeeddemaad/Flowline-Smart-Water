/*
  Smart Water Distribution - Residential node
  Wokwi ESP32 Arduino sketch

  MQTT host/port match the HiveMQ broker used by the sensor/valve nodes
  in the supplied Node-RED flow. Put your HiveMQ Cloud access credentials below.

  Credentials are intentionally placeholders because the exported Node-RED
  JSON does not contain the broker username/password.
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// ---------- Wokwi Wi-Fi ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// ---------- HiveMQ Cloud ----------
const char* MQTT_HOST = "e0b93cccb3ea43df8a98877ae1af77ac.s1.eu.hivemq.cloud";
constexpr uint16_t MQTT_PORT = 8883;  // MQTT over TLS
const char* MQTT_USERNAME = "WaterLogic";  // REQUIRED: replace this
const char* MQTT_PASSWORD = "12345678";  // REQUIRED: replace this
const char* MQTT_CLIENT_ID = "smartwater-wokwi-residential-esp32";  // unique per zone

// ---------- Zone and MQTT topics ----------
const char* ZONE_NAME = "residential";
const char* SENSOR_TOPIC = "smartwater/residential/sensors";
const char* VALVE_TOPIC = "smartwater/residential/valve";

// ---------- Wokwi wiring: matches diagram.json ----------
constexpr int DHT_PIN = 4;
constexpr int DHT_TYPE = DHT22; // diagram uses Wokwi DHT22 model for all zones
constexpr int WATER_LEVEL_PIN = 34;
constexpr int FLOW_PIN = 33;

constexpr int SERVO_PIN = 26;
constexpr int SERVO_MIN_ANGLE = 0;
constexpr int SERVO_MAX_ANGLE = 90;
constexpr int DEFAULT_SERVO_ANGLE = 20;
constexpr unsigned long PUBLISH_INTERVAL_MS = 5000;
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;

WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);
DHT dht(DHT_PIN, DHT_TYPE);
Servo valveServo;

unsigned long lastPublishMs = 0;
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastMqttAttemptMs = 0;
bool credentialsWarningPrinted = false;
int currentServoAngle = DEFAULT_SERVO_ANGLE;

bool mqttCredentialsConfigured() {
  return MQTT_USERNAME[0] != '\0' && MQTT_PASSWORD[0] != '\0' &&
         String(MQTT_USERNAME) != "YOUR_HIVEMQ_USERNAME" &&
         String(MQTT_PASSWORD) != "YOUR_HIVEMQ_PASSWORD";
}

int analogPercent(int pin) {
  int raw = analogRead(pin);  // ESP32 ADC: 0..4095
  return constrain((int)lroundf((raw / 4095.0f) * 100.0f), 0, 100);
}

void connectWiFiOnce() {
  Serial.println("[WiFi] Connecting to Wokwi-GUEST...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);
  lastWiFiAttemptMs = millis();
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != VALVE_TOPIC) return;

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.printf("[MQTT] Invalid valve JSON: %s\n", error.c_str());
    return;
  }

  if (!doc["angle"].is<int>() && !doc["angle"].is<float>() && !doc["angle"].is<double>()) {
    Serial.println("[MQTT] Valve command has no numeric 'angle'; ignored.");
    return;
  }

  const char* mode = doc["mode"] | "auto";
  int requestedAngle = (int)lroundf(doc["angle"].as<float>());
  if (String(mode) == "closed") requestedAngle = 0;
  requestedAngle = constrain(requestedAngle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);

  currentServoAngle = requestedAngle;
  valveServo.write(currentServoAngle);
  Serial.printf("[VALVE] zone=%s mode=%s angle=%d deg\n", ZONE_NAME, mode, currentServoAngle);
}

bool connectMQTT() {
  if (mqtt.connected()) return true;
  if (!mqttCredentialsConfigured()) {
    if (!credentialsWarningPrinted) {
      Serial.println("[MQTT] Add your HiveMQ Cloud username/password near the top of sketch.ino.");
      credentialsWarningPrinted = true;
    }
    return false;
  }

  if (millis() - lastMqttAttemptMs < MQTT_RETRY_INTERVAL_MS) return false;
  lastMqttAttemptMs = millis();
  Serial.printf("[MQTT] Connecting to %s:%u as %s...\n", MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID);

  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("[MQTT] Connected.");
    if (mqtt.subscribe(VALVE_TOPIC, 1)) {
      Serial.printf("[MQTT] Subscribed: %s\n", VALVE_TOPIC);
    } else {
      Serial.printf("[MQTT] Subscribe failed: %s\n", VALVE_TOPIC);
    }
    return true;
  }

  Serial.printf("[MQTT] Connection failed, state=%d. Will retry.\n", mqtt.state());
  return false;
}

void publishSensorData() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("[SENSORS] DHT read failed; skipped this MQTT publish.");
    return;
  }

  StaticJsonDocument<256> doc;
  doc["temp"] = roundf(temperature * 10.0f) / 10.0f;
  doc["humidity"] = roundf(humidity * 10.0f) / 10.0f;
  doc["level"] = analogPercent(WATER_LEVEL_PIN);
  doc["flow"] = analogPercent(FLOW_PIN);

  char buffer[256];
  size_t used = serializeJson(doc, buffer, sizeof(buffer));
  if (used == 0 || used >= sizeof(buffer)) {
    Serial.println("[SENSORS] Failed to serialize JSON payload.");
    return;
  }

  if (mqtt.publish(SENSOR_TOPIC, buffer, false)) {
    Serial.printf("[SENSORS] Published %s -> %s\n", SENSOR_TOPIC, buffer);
  } else {
    Serial.println("[SENSORS] MQTT publish failed.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.printf("=== Smart Water %s ESP32 ===\n", ZONE_NAME);

  analogReadResolution(12);
  dht.begin();
  valveServo.setPeriodHertz(50);
  valveServo.attach(SERVO_PIN, 500, 2400);
  valveServo.write(DEFAULT_SERVO_ANGLE);
  Serial.printf("[VALVE] Servo attached to GPIO%d; initial angle=%d deg\n", SERVO_PIN, DEFAULT_SERVO_ANGLE);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(8);

  // Simplifies TLS setup for this Wokwi-only prototype. For real hardware,
  // replace setInsecure() with CA certificate verification.
  secureClient.setInsecure();
  connectWiFiOnce();
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWiFiAttemptMs >= WIFI_RETRY_INTERVAL_MS) connectWiFiOnce();
    delay(10);
    return;
  }

  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  if (mqtt.connected() && now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;
    publishSensorData();
  }
  delay(2);
}
