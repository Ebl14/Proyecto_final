/****************************************************************
 * ESP32 + Mosquitto + FreeRTOS + Flash
 * - Alarma T > Umbral
 * - PWM inicia en 255
 * - Envío de HUMEDAD agregado
 * - Sistema de CALIBRACIÓN (Offsets) persistente en Flash
 ****************************************************************/

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ------------------ CONFIG RED / MQTT ------------------
const char* WIFI_SSID     = "Camilo";       
const char* WIFI_PASSWORD = "maracaibo1"; 

const char* MQTT_SERVER    = "broker.emqx.io";
const int   MQTT_PORT      = 1883;
const char* MQTT_CLIENT_ID = "ESP32-Ambiente-Camilo-Client-05";

// --- Tópicos de Salida (Telemetría) ---
#define MQTT_TOPIC_TEMP           "esp32/ambiente/telemetria/temperatura"
#define MQTT_TOPIC_HUMIDITY       "esp32/ambiente/telemetria/humedad"      // <--- NUEVO
#define MQTT_TOPIC_GAS            "esp32/ambiente/telemetria/gas"
#define MQTT_TOPIC_ALARM_ST       "esp32/ambiente/telemetria/estado_alarma"
#define MQTT_TOPIC_CONFIG_RPT     "esp32/ambiente/telemetria/config_actual" 

// --- Tópicos de Entrada (Control) ---
#define MQTT_TOPIC_CONTROL_ALARM  "esp32/ambiente/control/alarma_manual"
#define MQTT_TOPIC_SET_THRESHOLDS "esp32/ambiente/control/limites"
#define MQTT_TOPIC_SET_CALIB      "esp32/ambiente/control/calibracion"     // <--- NUEVO

// ------------------ HARDWARE ------------------
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

#define MQ135_PIN   34    
#define FAN_PWM_PIN 18    
#define BUZZER_PIN  23
#define LED_R       14
#define LED_G       27
#define LED_B       26

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

Preferences preferences;

// ------------------ ESTRUCTURAS DE DATOS ------------------
typedef struct {
  // Umbrales de Alarma
  float tempThreshold;  
  int   gasThreshold;   
  int   fanSpeed;       
  
  // Variables de Calibración (Offsets)
  float tempOffset; // Ej: -1.5 (Resta 1.5 grados)
  float humOffset;  // Ej: +5.0 (Suma 5% humedad)
  int   gasOffset;  // Ej: -100 (Resta 100 puntos al gas)
} config_t;

typedef struct {
  float currentTemp;
  float currentHum; // <--- Agregado
  int   currentGas;
  bool  isTempAlarm;
  bool  isGasAlarm;
  bool  isRemoteAlarm;
} state_t;

// Valores globales
config_t sysConfig; 
state_t  sysState  = { 0.0, 0.0, 0, false, false, false };

SemaphoreHandle_t mutex;
bool requestConfigPublish = false; 

WiFiClient espClient;
PubSubClient client(espClient);

// ------------------ UTILS ------------------
void setColor(int r, int g, int b) {
  analogWrite(LED_R, r);
  analogWrite(LED_G, g);
  analogWrite(LED_B, b);
}

void saveConfigToFlash() {
  if(preferences.begin("env_sys", false)) {
    // Umbrales
    preferences.putFloat("t_lim", sysConfig.tempThreshold);
    preferences.putInt("g_lim",   sysConfig.gasThreshold);
    preferences.putInt("pwm",     sysConfig.fanSpeed);
    
    // Calibración (Nuevas llaves)
    preferences.putFloat("t_off", sysConfig.tempOffset);
    preferences.putFloat("h_off", sysConfig.humOffset);
    preferences.putInt("g_off",   sysConfig.gasOffset);
    
    preferences.end();
    Serial.println(">> Configuración y Calibración guardadas en Flash.");
  }
}

// ------------------ CALLBACK MQTT ------------------
void callback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  
  Serial.print("\n[MQTT RX] "); Serial.print(topic); Serial.print(": "); Serial.println(msg);

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, msg);
  
  if (!err && xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
    bool changed = false;

    // 1. Límites (Umbrales)
    if (String(topic) == MQTT_TOPIC_SET_THRESHOLDS) {
      if (doc.containsKey("temp")) { sysConfig.tempThreshold = doc["temp"]; changed = true; }
      if (doc.containsKey("gas"))  { sysConfig.gasThreshold  = doc["gas"];  changed = true; }
    }
    
    // 2. Calibración (Offsets) -> NUEVO
    if (String(topic) == MQTT_TOPIC_SET_CALIB) {
      if (doc.containsKey("temp")) { sysConfig.tempOffset = doc["temp"]; changed = true; }
      if (doc.containsKey("hum"))  { sysConfig.humOffset  = doc["hum"];  changed = true; }
      if (doc.containsKey("gas"))  { sysConfig.gasOffset  = doc["gas"];  changed = true; }
      Serial.println("Offsets de calibración actualizados.");
    }

    // 3. Control Manual
    if (String(topic) == MQTT_TOPIC_CONTROL_ALARM) {
      if (doc.containsKey("alarma")) sysState.isRemoteAlarm = doc["alarma"];
      if (doc.containsKey("pwm")) { 
        sysConfig.fanSpeed = constrain(doc["pwm"], 0, 255); 
        changed = true; 
      }
    }

    if (changed) {
      saveConfigToFlash();
      requestConfigPublish = true; 
    }
    xSemaphoreGive(mutex);
  }
}

// ------------------ TAREAS ------------------

// 1. Tarea Sensores (Aplica Calibración)
void sensorTask(void* pvParam) {
  for (;;) {
    // Lectura Cruda
    float rawT = dht.readTemperature();
    float rawH = dht.readHumidity();
    int   rawG = analogRead(MQ135_PIN);

    if (isnan(rawT)) rawT = 0.0;
    if (isnan(rawH)) rawH = 0.0;

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
      // --- APLICAR CALIBRACIÓN ---
      sysState.currentTemp = rawT + sysConfig.tempOffset;
      sysState.currentHum  = rawH + sysConfig.humOffset;
      sysState.currentGas  = rawG + sysConfig.gasOffset;

      // Asegurar que no den negativos ilógicos (opcional)
      if(sysState.currentGas < 0) sysState.currentGas = 0;
      if(sysState.currentHum < 0) sysState.currentHum = 0;
      if(sysState.currentHum > 100) sysState.currentHum = 100;
      
      // Lógica Alarma (Usando valor calibrado)
      sysState.isTempAlarm = (sysState.currentTemp >= sysConfig.tempThreshold && sysState.currentTemp != 0); 
      sysState.isGasAlarm  = (sysState.currentGas >= sysConfig.gasThreshold);
      
      xSemaphoreGive(mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// 2. Control (Fan, Buzzer, LED)
void controlTask(void* pvParam) {
  for (;;) {
    bool tAlarm, gAlarm, rAlarm;
    int targetPWM;

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50))) {
      tAlarm = sysState.isTempAlarm;
      gAlarm = sysState.isGasAlarm;
      rAlarm = sysState.isRemoteAlarm;
      targetPWM = sysConfig.fanSpeed;
      xSemaphoreGive(mutex);
    }

    bool anyAlarm = tAlarm || gAlarm || rAlarm;

    if (anyAlarm) {
      analogWrite(FAN_PWM_PIN, targetPWM); 
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      analogWrite(FAN_PWM_PIN, 0);         
      digitalWrite(BUZZER_PIN, LOW);
    }

    // LED RGB
    if (tAlarm && gAlarm)   setColor(255, 0, 0);     // Rojo
    else if (gAlarm)        setColor(255, 165, 0);   // Naranja
    else if (tAlarm)        setColor(255, 0, 255);   // Morado
    else if (rAlarm)        setColor(0, 0, 255);     // Azul
    else                    setColor(0, 255, 0);     // Verde

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// 3. Publicación MQTT
void mqttPubTask(void* pvParam) {
  char msg[128];
  for (;;) {
    if (client.connected()) {
      float t, h; int g; bool fanOn;
      
      if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
        t = sysState.currentTemp;
        h = sysState.currentHum;
        g = sysState.currentGas;
        fanOn = (sysState.isTempAlarm || sysState.isGasAlarm || sysState.isRemoteAlarm);
        xSemaphoreGive(mutex);
      }

      // Temp
      snprintf(msg, 128, "{\"valor\": %.1f}", t);
      client.publish(MQTT_TOPIC_TEMP, msg);
      
      // Humedad (NUEVO)
      snprintf(msg, 128, "{\"valor\": %.1f}", h);
      client.publish(MQTT_TOPIC_HUMIDITY, msg);
      
      // Gas
      snprintf(msg, 128, "{\"valor\": %d}", g);
      client.publish(MQTT_TOPIC_GAS, msg);

      // Estado
      snprintf(msg, 128, "{\"alarma\": %s, \"fan\": %s}", fanOn?"true":"false", fanOn?"ON":"OFF");
      client.publish(MQTT_TOPIC_ALARM_ST, msg);

      // Reporte Configuración (Incluye offsets)
      if (requestConfigPublish) {
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
          // JSON grande, cuidado con tamaño buffer
          DynamicJsonDocument docRep(300);
          docRep["temp_lim"] = sysConfig.tempThreshold;
          docRep["gas_lim"]  = sysConfig.gasThreshold;
          docRep["fan_pwm"]  = sysConfig.fanSpeed;
          // Agregamos offsets al reporte
          docRep["off_t"]    = sysConfig.tempOffset;
          docRep["off_h"]    = sysConfig.humOffset;
          docRep["off_g"]    = sysConfig.gasOffset;
          
          char buffer[300];
          serializeJson(docRep, buffer);
          client.publish(MQTT_TOPIC_CONFIG_RPT, buffer);
          
          requestConfigPublish = false; 
          xSemaphoreGive(mutex);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// 4. Conexión MQTT
void mqttConnTask(void* pvParam) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!client.connected()) {
        if (client.connect(MQTT_CLIENT_ID)) {
          Serial.println("MQTT Conectado");
          client.subscribe(MQTT_TOPIC_CONTROL_ALARM);
          client.subscribe(MQTT_TOPIC_SET_THRESHOLDS);
          client.subscribe(MQTT_TOPIC_SET_CALIB); // <-- Suscripción nueva
          requestConfigPublish = true; 
        } else {
          vTaskDelay(pdMS_TO_TICKS(2000));
        }
      }
      client.loop();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// 5. OLED
void oledTask(void* pvParam) {
  for (;;) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0,0);
    display.printf("W:%d M:%d", WiFi.status()==WL_CONNECTED, client.connected());

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
      display.setTextSize(2);
      display.setCursor(0, 15);
      display.printf("%.1fC %.0f%%", sysState.currentTemp, sysState.currentHum); // T y H
      
      display.setTextSize(1);
      display.setCursor(0, 38);
      display.printf("Gas:%d (Lim:%d)", sysState.currentGas, sysConfig.gasThreshold);
      
      // Mostrar si hay Offset aplicado con un pequeño asterisco o similar
      if (sysConfig.tempOffset != 0 || sysConfig.gasOffset != 0) {
        display.setCursor(120, 38); display.print("*");
      }

      bool alarm = (sysState.isTempAlarm || sysState.isGasAlarm || sysState.isRemoteAlarm);
      display.setCursor(0, 54);
      if(alarm) {
        display.print("ALERTA! PWM:"); display.print(sysConfig.fanSpeed);
      } else {
        display.print("SISTEMA NORMAL");
      }
      xSemaphoreGive(mutex);
    }
    display.display();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  pinMode(FAN_PWM_PIN, OUTPUT); pinMode(MQ135_PIN, INPUT);

  digitalWrite(BUZZER_PIN, LOW);
  analogWrite(FAN_PWM_PIN, 0);

  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  dht.begin();
  
  mutex = xSemaphoreCreateMutex();

  // --- CARGAR PREFERENCIAS (FLASH) ---
  preferences.begin("env_sys", false);
  
  // Umbrales por defecto
  sysConfig.tempThreshold = preferences.getFloat("t_lim", 30.0); 
  sysConfig.gasThreshold  = preferences.getInt("g_lim", 1800);   
  sysConfig.fanSpeed      = preferences.getInt("pwm", 255);      
  
  // Calibración por defecto (0.0 = sin cambios)
  sysConfig.tempOffset    = preferences.getFloat("t_off", 0.0);
  sysConfig.humOffset     = preferences.getFloat("h_off", 0.0);
  sysConfig.gasOffset     = preferences.getInt("g_off", 0);

  preferences.end();
  
  Serial.println("--- Config cargada ---");
  Serial.printf("Offset T: %.1f | H: %.1f | G: %d\n", sysConfig.tempOffset, sysConfig.humOffset, sysConfig.gasOffset);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK");

  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(callback);

  xTaskCreate(sensorTask,   "Sensors", 4096, NULL, 1, NULL);
  xTaskCreate(controlTask,  "Control", 2048, NULL, 1, NULL);
  xTaskCreate(mqttConnTask, "MqttCon", 4096, NULL, 1, NULL);
  xTaskCreate(mqttPubTask,  "MqttPub", 4096, NULL, 1, NULL);
  xTaskCreate(oledTask,     "OLED",    2048, NULL, 1, NULL);
}

void loop() { vTaskDelay(1000); }