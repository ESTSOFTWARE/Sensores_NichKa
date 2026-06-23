#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// --- Pines sensores ---
#define ONE_WIRE_BUS      4
#define PIN_CONDUCTIVIDAD 33
#define PIN_ALCOHOL       34
#define PIN_TURBIDEZ      32

// --- Pines relés (activos en LOW) ---
#define RELAY_TEMP          25
#define RELAY_CONDUCTIVIDAD 26
#define RELAY_ALCOHOL       27
#define RELAY_TURBIDEZ      14

// --- MQTT ---
const char* MQTT_HOST      = "shark.rmq.cloudamqp.com";
const int   MQTT_PORT      = 1883;
const char* MQTT_USER      = "kcugwwbq:kcugwwbq";
const char* MQTT_PASSWORD  = "LAG1cqP1fWpW-tXs2eQzyyq_fZMigTyj";
const char* MQTT_CLIENT_ID = "esp32-sensores-1";
const int   CIRCUIT_ID     = 1;

#define INTERVALO_ENVIO 30000

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
unsigned long ultimoEnvio = 0;

// --- Estado relés ---
bool relay_temp          = false;
bool relay_conductividad = false;
bool relay_alcohol       = false;
bool relay_turbidez      = false;

void setRelay(int pin, bool encendido) {
  digitalWrite(pin, encendido ? LOW : HIGH);
}

void aplicarComando(const char* dispositivo, const char* estado) {
  bool on = strcmp(estado, "encendido") == 0;
  if      (strcmp(dispositivo, "sensor_temperatura")   == 0) { relay_temp          = on; setRelay(RELAY_TEMP, on); }
  else if (strcmp(dispositivo, "sensor_conductividad") == 0) { relay_conductividad = on; setRelay(RELAY_CONDUCTIVIDAD, on); }
  else if (strcmp(dispositivo, "sensor_alcohol")       == 0) { relay_alcohol       = on; setRelay(RELAY_ALCOHOL, on); }
  else if (strcmp(dispositivo, "sensor_turbidez")      == 0) { relay_turbidez      = on; setRelay(RELAY_TURBIDEZ, on); }
  else if (strcmp(dispositivo, "sensores")             == 0) {
    relay_temp = relay_conductividad = relay_alcohol = relay_turbidez = on;
    setRelay(RELAY_TEMP, on);
    setRelay(RELAY_CONDUCTIVIDAD, on);
    setRelay(RELAY_ALCOHOL, on);
    setRelay(RELAY_TURBIDEZ, on);
  }
  Serial.printf("[RELAY] %s → %s\n", dispositivo, estado);
}

float calcularAlcohol(float voltaje) {
  voltaje = voltaje * 2.0;
  if      (voltaje < 0.4) return 0;
  else if (voltaje < 1.0) return (voltaje - 0.4) * 100;
  else if (voltaje < 2.0) return (voltaje - 0.4) * 150;
  else                    return (voltaje - 0.4) * 200;
}

float calcularTurbidez(float voltaje) {
  voltaje = voltaje * 2.0;
  return -123.38 * voltaje + 553.4;
}

float calcularTDS(int adcRaw) {
  float conductividad = 5.03 * (adcRaw * 8.0) - 2265.03;
  return max(conductividad * 0.64 / 1000.0, 0.0);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  Serial.printf("[MQTT CMD] %s → %s\n", topic, message.c_str());
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, message)) return;
  for (JsonPair kv : doc.as<JsonObject>()) {
    aplicarComando(kv.key().c_str(), kv.value().as<const char*>());
  }
}

void publicarSensor(const char* sensor_type, float valor) {
  if (!mqttClient.connected()) return;
  char topic[64];
  snprintf(topic, sizeof(topic), "sensors/%d/%s", CIRCUIT_ID, sensor_type);
  StaticJsonDocument<192> doc;
  doc["circuit_id"]  = CIRCUIT_ID;
  doc["sensor_type"] = sensor_type;
  doc["value"]       = valor;
  doc["session_id"]  = nullptr;
  doc["active"]      = true;
  char payload[192];
  serializeJson(doc, payload);
  mqttClient.publish(topic, payload);
  Serial.printf("[MQTT] %s → %s\n", topic, payload);
}

void conectarWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setTitle("Fermentador IoT - Sensores");
  if (!wm.autoConnect("Sensores-Setup")) {
    Serial.println("[WiFi] Timeout, reiniciando...");
    ESP.restart();
  }
  Serial.printf("[WiFi] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
}

void conectarMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  int intentos = 0;
  while (!mqttClient.connected() && intentos < 5) {
    Serial.print("[MQTT] Conectando...");
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
      Serial.println(" conectado");
      char topicCmd[64];
      snprintf(topicCmd, sizeof(topicCmd), "commands/%d/state", CIRCUIT_ID);
      mqttClient.subscribe(topicCmd);
      Serial.printf("[MQTT] Suscrito a %s\n", topicCmd);
    } else {
      Serial.printf(" fallo (rc=%d), reintentando...\n", mqttClient.state());
      delay(2000);
      intentos++;
    }
  }
  if (!mqttClient.connected())
    Serial.println("[MQTT] No se pudo conectar.");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  int relays[] = {RELAY_TEMP, RELAY_CONDUCTIVIDAD, RELAY_ALCOHOL, RELAY_TURBIDEZ};
  for (int pin : relays) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  tempSensor.begin();
  conectarWiFi();
  conectarMQTT();
  Serial.println("[SETUP] Listo");
}

void loop() {
  if (!mqttClient.connected()) {
    Serial.println("[MQTT] Reconectando...");
    conectarMQTT();
  }
  mqttClient.loop();

  unsigned long ahora = millis();
  if (ahora - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = ahora;

    float tempC = 0.0, tds_gL = 0.0, ppm = 0.0, turbidez = 0.0;

    if (relay_temp) {
      tempSensor.requestTemperatures();
      tempC = tempSensor.getTempCByIndex(0);
      if (tempC == DEVICE_DISCONNECTED_C) tempC = 0.0;
    }
    if (relay_conductividad)
      tds_gL = calcularTDS(analogRead(PIN_CONDUCTIVIDAD));
    if (relay_alcohol)
      ppm = calcularAlcohol(analogRead(PIN_ALCOHOL) * (3.3 / 4095.0));
    if (relay_turbidez)
      turbidez = calcularTurbidez(analogRead(PIN_TURBIDEZ) * (3.3 / 4095.0));

    publicarSensor("temperature",  tempC);
    publicarSensor("conductivity", tds_gL);
    publicarSensor("alcohol",      ppm);
    publicarSensor("turbidity",    turbidez);

    Serial.printf(
      "[MONITOR] Temp: %.2f°C | Cond: %.4f g/L | Alcohol: %.1f ppm | Turbidez: %.1f NTU\n",
      tempC, tds_gL, ppm, turbidez
    );
  }
}