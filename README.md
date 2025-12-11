# Proyecto_final
#  Sistema de Monitoreo Ambiental con ESP32 + FreeRTOS + MQTT

Este proyecto implementa un sistema de monitoreo ambiental con **ESP32**, usando:

- Sensor **DHT22** (temperatura y humedad)  
- Sensor de gas **MQ135** (calidad del aire)  
- Broker **MQTT** (probado con Mosquitto/EMQX)  
- Pantalla **OLED SSD1306**  
- Control **PWM** para ventilación  
- **Buzzer** para alarma acústica  
- **LED RGB** para señalización de estados  

Además, incluye un sistema completo de **calibración**, cuyos parámetros se almacenan en la **Flash del ESP32 usando `Preferences`**, garantizando persistencia tras reinicios.  
Todo el sistema corre sobre **FreeRTOS**, distribuyendo las funciones en **tareas (hilos) independientes**.

---

##  Características principales

- Lectura periódica de:
  - Temperatura (°C)
  - Humedad relativa (%)
  - Nivel de gas (valor ADC del MQ135)
- Publicación de datos vía **MQTT**:
  - Temperatura
  - Humedad
  - Gas
  - Estado de alarma y ventilador
  - Configuración actual (umbrales y offsets)
- Control:
  - Ventilador por **PWM**
  - Buzzer ON/OFF según alarmas
  - LED RGB con colores según estado
- Sistema de **alarmas** por:
  - Temperatura sobre umbral
  - Gas sobre umbral
  - Alarma remota por MQTT
- **Calibración** con offsets:
  - `tempOffset` (°C)
  - `humOffset` (%)
  - `gasOffset` (unidades ADC)
- Configuración y calibración **persistentes en Flash** usando `Preferences`.
- Toda la lógica separada en tareas de **FreeRTOS**:
  - Lectura de sensores
  - Control de actuadores
  - Publicación MQTT
  - Mantenimiento de conexión MQTT
  - Actualización de pantalla OLED

---

##  Hardware utilizado

- ESP32
- DHT22 (sensor de temperatura y humedad) – conectado a GPIO 4
- MQ135 (sensor de gas) – conectado a GPIO 34 (entrada analógica)
- Pantalla OLED SSD1306 por I2C – SDA GPIO 21, SCL GPIO 22
- Ventilador controlado por PWM – GPIO 18
- Buzzer – GPIO 23
- LED RGB:
  - Rojo: GPIO 14  
  - Verde: GPIO 27  
  - Azul: GPIO 26  

> Los pines pueden ajustarse en el código según la necesidad del montaje.

---

##  Librerías y dependencias

En el código se utilizan las siguientes librerías:

- `WiFi.h` – conexión WiFi del ESP32  
- `PubSubClient.h` – cliente MQTT  
- `Wire.h` – comunicación I2C  
- `Adafruit_GFX.h` y `Adafruit_SSD1306.h` – manejo de la pantalla OLED  
- `DHT.h` – lectura del sensor DHT22  
- `ArduinoJson.h` – parseo y generación de JSON para mensajes MQTT  
- `Preferences.h` – almacenamiento de configuración en la Flash del ESP32  
- `FreeRTOS` – incluido en el core del ESP32

Instálalas desde el **Library Manager** del Arduino IDE o PlatformIO según corresponda.

---

##  Configuración WiFi y MQTT

En el código se definen estas constantes:

```cpp
const char* WIFI_SSID     = "TU_SSID";
const char* WIFI_PASSWORD = "TU_PASSWORD";

const char* MQTT_SERVER    = "broker.emqx.io";
const int   MQTT_PORT      = 1883;
const char* MQTT_CLIENT_ID = "ESP32-Ambiente-Camilo-Client-05";
