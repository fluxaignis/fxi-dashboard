#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

// --- CONFIGURACIÓN DE GITHUB OTA ---
// La palabra mágica "AUTO_VERSION" será reemplazada por el robot durante la compilación
String FIRMWARE_VERSION = "AUTO_VERSION";
const char* urlVersion = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/main/version.txt";
const char* urlFirmware = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/main/firmware.bin";

unsigned long ultimoChequeoOTA = 0;
const unsigned long INTERVALO_OTA = 300000; // 5 minutos (para pruebas; cámbialo a 3600000 para 1 hora)

// --- MAPEO DE PINES ---
#define DHTPIN 27
#define DHTTYPE DHT11
#define PIN_SERVO_H 25
#define PIN_SERVO_V 26
#define PIN_BOMBA 32
#define PIN_ROJO 13
#define PIN_VERDE 12
#define PIN_AZUL 14
#define PIN_LLAMA 35
#define PIN_BUZZER 33

DHT dht(DHTPIN, DHTTYPE);
Servo servoHorizontal;
Servo servoVertical;
WiFiClientSecure espClient;
PubSubClient client(espClient);
WebServer server(80);
DNSServer dnsServer;

// --- ESTADOS Y TIEMPOS DE LA RUTINA ---
enum EstadoSistema { REPOSO, ESPERANDO_AGUA, GIRANDO_IZQ, GIRANDO_DER, VOLVIENDO_CENTRO };
EstadoSistema estadoActual = REPOSO;
const int SERVO_H_NEUTRAL = 94;
unsigned long TIEMPO_90_IZQ = 750;
unsigned long TIEMPO_90_DER = 780;
unsigned long TIEMPO_RETORNO = 500;
unsigned long cronometroRutina = 0;
const unsigned long WATER_DELAY_MS = 1000;

// --- VARIABLES DE SEGURIDAD Y SENSORES ---
const int UMBRAL_FUEGO = 500;
const float TEMP_CRITICA = 40.0;
bool emergenciaEnviada = false;
float tempGuardada = 25.0;
float humGuardada = 50.0;
int rssiGuardado = -99;
unsigned long cronometroDatos = 0;
unsigned long cronometroRSSI = 0;
unsigned long ultimoTiempoDHT = 0;
bool simularFuego = false;

// --- CREDENCIALES (para WiFi externo) ---
const char* ssid = "NauticaNet";
const char* password = "PromoXXX.2026";
const char* mqtt_server = "df734b8fbeed43978f29869442892dcf.s1.eu.hivemq.cloud";
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -14400;
const int daylightOffset_sec = 0;

// --- Configuración del punto de acceso (hotspot) ---
const char* ap_ssid = "FluxaIgnis";
const char* ap_password = "";          // Red abierta
IPAddress apIP(192, 168, 1, 1);
IPAddress apGateway(192, 168, 1, 1);
IPAddress apSubnet(255, 255, 255, 0);

// --- FUNCIONES AUXILIARES ---
void setColor(int r, int g, int b) {
  analogWrite(PIN_ROJO, r);
  analogWrite(PIN_VERDE, g);
  analogWrite(PIN_AZUL, b);
}

void enviarNotificacionMQTT(String motivo, float temp) {
  if (!client.connected()) return;
  struct tm timeinfo;
  char fechaHora[30];
  if (!getLocalTime(&timeinfo)) strcpy(fechaHora, "Error de tiempo");
  else strftime(fechaHora, 30, "%I:%M%p %d/%m/%Y", &timeinfo);
  String payload = "{\"motivo\":\"" + motivo + "\",\"temperatura\":" + String(temp) + ",\"timestamp\":\"" + String(fechaHora) + "\"}";
  client.publish("fxi/emergencia", payload.c_str());
}

void iniciarRutina(String motivo) {
  if (estadoActual == REPOSO) {
    Serial.println("¡EMERGENCIA! Iniciando maniobra de extinción... Motivo: " + motivo);
    digitalWrite(PIN_BOMBA, HIGH);
    cronometroRutina = millis();
    estadoActual = ESPERANDO_AGUA;
  }
}

void detenerRutina() {
  servoHorizontal.write(SERVO_H_NEUTRAL);
  digitalWrite(PIN_BOMBA, LOW);
  estadoActual = REPOSO;
  Serial.println("Rutina detenida.");
}

// --- CALLBACK MQTT ---
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  if (String(topic) == "fxi/comandos") {
    if (msg == "TOGGLE") {
      if (estadoActual == REPOSO) iniciarRutina("Comando Manual MQTT");
      else detenerRutina();
    }
  } else if (String(topic) == "fxi/simular") {
    if (msg == "FUEGO_ON") {
      simularFuego = true;
      Serial.println("Simulación ACTIVADA");
    } else if (msg == "FUEGO_OFF") {
      simularFuego = false;
      Serial.println("Simulación DESACTIVADA");
    }
  }
}

// --- MANEJADORES WEB ---
void handleRoot() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    server.send(500, "text/plain", "Error al cargar la página");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void handleEstado() {
  String json = "{";
  json += "\"temp\":" + String(tempGuardada) + ",";
  json += "\"hum\":" + String(humGuardada) + ",";
  json += "\"rssi\":" + String(rssiGuardado);
  json += "}";
  server.send(200, "application/json", json);
}

void handleToggle() {
  if (estadoActual == REPOSO) iniciarRutina("Comando Web");
  else detenerRutina();
  server.send(200, "text/plain", (estadoActual != REPOSO) ? "ON" : "OFF");
}

void handleNotFound() {
  // Redirige cualquier ruta no encontrada a la raíz (portal cautivo)
  server.send(200, "text/html", "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='0;url=http://192.168.1.1/'></head><body></body></html>");
}

void chequearActualizacionGitHub() {
  // Solo intentamos actualizar si hay conexión a la red externa
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sin conexión WiFi externa. Se omite chequeo OTA de GitHub.");
    return;
  }

  Serial.println("Chequeando actualizaciones en GitHub...");

  HTTPClient http;
  // Usamos el espClient que ya tienes configurado como Inseguro (setInsecure) en tu setup
  http.begin(espClient, urlVersion); 
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String versionGitHub = http.getString();
    versionGitHub.trim(); // Limpiamos espacios o saltos de línea

    Serial.println("Versión actual en ESP32: " + FIRMWARE_VERSION);
    Serial.println("Versión detectada en GitHub: " + versionGitHub);

    // Si las versiones no coinciden, iniciamos la descarga
    if (versionGitHub != FIRMWARE_VERSION && versionGitHub.length() > 0) {
      Serial.println("¡Nueva versión detectada! Iniciando descarga e instalación OTA...");

      // GitHub usa redirecciones en sus links RAW, hay que habilitarlas
      httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      
      // Iniciamos el flasheo
      t_httpUpdate_return ret = httpUpdate.update(espClient, urlFirmware);

      switch (ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("Error en OTA (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
          break;
        case HTTP_UPDATE_NO_UPDATES:
          Serial.println("No hay actualizaciones disponibles (HTTP_UPDATE_NO_UPDATES).");
          break;
        case HTTP_UPDATE_OK:
          Serial.println("¡Actualización completada! El ESP32 se reiniciará ahora.");
          break;
      }
    } else {
      Serial.println("El firmware ya está en la última versión.");
    }
  } else {
    Serial.printf("Error al consultar versión en GitHub. Código HTTP: %d\n", httpCode);
  }
  http.end();
}

// --- CONFIGURACIÓN INICIAL ---
void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(PIN_BOMBA, OUTPUT); digitalWrite(PIN_BOMBA, LOW);
  pinMode(PIN_ROJO, OUTPUT); pinMode(PIN_VERDE, OUTPUT); pinMode(PIN_AZUL, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT); noTone(PIN_BUZZER);

  // Servos
  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2); ESP32PWM::allocateTimer(3);
  servoHorizontal.setPeriodHertz(50); servoHorizontal.attach(PIN_SERVO_H, 500, 2400);
  servoVertical.setPeriodHertz(50); servoVertical.attach(PIN_SERVO_V, 500, 2400);
  servoHorizontal.write(SERVO_H_NEUTRAL); servoVertical.write(20);

  // Inicializar LittleFS
  if (!LittleFS.begin()) {
    Serial.println("Error al montar LittleFS");
    return;
  }

Serial.println("Listando archivos en LittleFS:");
File root = LittleFS.open("/");
File file = root.openNextFile();
while (file) {
    Serial.println(file.name());
    file = root.openNextFile();
}

  // ========== 1. Configurar punto de acceso (AP) ==========
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.println("Hotspot creado: " + String(ap_ssid));
  Serial.print("IP del ESP32 en el hotspot: ");
  Serial.println(WiFi.softAPIP());

  // ========== 2. Iniciar servidor DNS (portal cautivo) ==========
  dnsServer.start(53, "*", apIP);
  Serial.println("DNS Captive Portal activado");

  // ========== 3. Iniciar mDNS ==========
  if (MDNS.begin("fluxaignis")) {
    Serial.println("mDNS iniciado: fluxaignis.local");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Error al iniciar mDNS");
  }

  // ========== 4. Intentar conectar a red WiFi externa ==========
  WiFi.begin(ssid, password);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi externo conectado");
    Serial.print("IP en red externa: ");
    Serial.println(WiFi.localIP());
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    espClient.setInsecure();
    client.setServer(mqtt_server, 8883);
    client.setCallback(callback);
  } else {
    Serial.println("\nNo se pudo conectar a red externa. Modo offline + hotspot.");
  }

  // ========== 5. Configurar OTA ==========
  ArduinoOTA.setHostname("fluxaignis_ota");
  ArduinoOTA.setPassword("12345678");   // Opcional
  ArduinoOTA.begin();
  Serial.println("OTA iniciado. Usa fluxaignis_ota.local o la IP para actualizar.");

  // ========== 6. Configurar servidor web ==========
  server.on("/", handleRoot);
  server.on("/estado", handleEstado);
  server.on("/toggleServo", handleToggle);
  server.on("/generate_204", []() { server.send(204); });
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("Servidor web listo. Portal cautivo activo.");
  Serial.println("Accede a:");
  Serial.println(" - http://192.168.1.1");
  Serial.println(" - http://fluxaignis.local");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(" - http://");
    Serial.println(WiFi.localIP());
  }
  Serial.println("Sistema híbrido (AP+STA) con MQTT, LittleFS y OTA.");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();

  // Mantener MQTT solo si hay WiFi externo
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      if (client.connect("ESP32_FXI", "Admin", "FluxaIgnis2026")) {
        client.subscribe("fxi/comandos");
        client.subscribe("fxi/simular");
        Serial.println("MQTT conectado");
      }
    } else {
      client.loop();
    }
  }

  unsigned long ahora = millis();

  // --- LECTURA DE TEMPERATURA Y HUMEDAD ---
  if (ahora - ultimoTiempoDHT >= 2000) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (t > 0 && t < 80) tempGuardada = t;
    if (!isnan(h)) humGuardada = h;
    ultimoTiempoDHT = ahora;
  }

  // --- LECTURA DE RSSI (actualizar siempre, incluso en modo AP, aunque no sea muy útil) ---
  rssiGuardado = WiFi.RSSI();

  // ========== 1. DETECCIÓN DE FUEGO ==========
  int lecturaLlama;
  if (simularFuego) {
    lecturaLlama = 300;
  } else {
    lecturaLlama = analogRead(PIN_LLAMA);
    if (lecturaLlama < 50) lecturaLlama = 4095;
  }
  if (estadoActual == REPOSO && lecturaLlama < UMBRAL_FUEGO) {
    String motivo = simularFuego ? "FUEGO SIMULADO (MQTT)" : "FUEGO DETECTADO (KY-026)";
    iniciarRutina(motivo);
    if (!emergenciaEnviada) {
      enviarNotificacionMQTT(motivo, tempGuardada);
      emergenciaEnviada = true;
    }
  }

  // ========== 2. SEGURIDAD TÉRMICA ==========
  if (tempGuardada >= TEMP_CRITICA) {
    if (!emergenciaEnviada) {
      enviarNotificacionMQTT("CALOR CRITICO", tempGuardada);
      emergenciaEnviada = true;
      iniciarRutina("Temperatura crítica superada");
    }
  } else if (tempGuardada < TEMP_CRITICA - 2.0) {
    emergenciaEnviada = false;
  }

  // ========== 3. INDICADORES ==========
  if (estadoActual != REPOSO) {
    setColor(255, 0, 0);
    if ((ahora / 300) % 2 == 0) tone(PIN_BUZZER, 2000);
    else noTone(PIN_BUZZER);
  } else {
    noTone(PIN_BUZZER);
    int r = (tempGuardada >= 35) ? 255 : (tempGuardada >= 30 ? 255 : 0);
    int g = (tempGuardada >= 35) ? 0 : (tempGuardada >= 30 ? 100 : 255);
    setColor(r, g, 0);
  }

  // ========== 4. MÁQUINA DE ESTADOS DEL SERVO ==========
  switch (estadoActual) {
    case REPOSO: break;
    case ESPERANDO_AGUA:
      if (ahora - cronometroRutina >= WATER_DELAY_MS) {
        Serial.println("Agua lista, iniciando barrido");
        servoHorizontal.write(SERVO_H_NEUTRAL);
        delay(50);
        cronometroRutina = ahora;
        estadoActual = GIRANDO_IZQ;
      }
      break;
    case GIRANDO_IZQ:
      servoHorizontal.write(70);
      if (ahora - cronometroRutina >= TIEMPO_90_IZQ) {
        cronometroRutina = ahora;
        estadoActual = GIRANDO_DER;
      }
      break;
    case GIRANDO_DER:
      servoHorizontal.write(110);
      if (ahora - cronometroRutina >= (TIEMPO_90_DER * 2)) {
        cronometroRutina = ahora;
        estadoActual = VOLVIENDO_CENTRO;
      }
      break;
    case VOLVIENDO_CENTRO:
      servoHorizontal.write(70);
      if (ahora - cronometroRutina >= TIEMPO_RETORNO) {
        detenerRutina();
      }
      break;
  }

  // ========== 5. ENVÍO DE DATOS MQTT (solo si hay conexión) ==========
  if (WiFi.status() == WL_CONNECTED && client.connected()) {
    if (ahora - cronometroDatos > 2000) {
      cronometroDatos = ahora;
      String hStr = isnan(humGuardada) ? "0" : String(humGuardada);
      String payload = "{\"temp\":" + String(tempGuardada) + ", \"hum\":" + hStr + "}";
      client.publish("fxi/datos", payload.c_str());
    }
    if (ahora - cronometroRSSI > 5000) {
      cronometroRSSI = ahora;
      String rssiPayload = "{\"rssi\":" + String(rssiGuardado) + "}";
      client.publish("fxi/rssi", rssiPayload.c_str());
    }
  }

// ========== CHEQUEO AUTOMÁTICO DE OTA DESDE GITHUB ==========
  // (Ya usamos la variable 'ahora' que definiste al principio del loop)
  if (ahora - ultimoChequeoOTA >= INTERVALO_OTA) {
    ultimoChequeoOTA = ahora;
    chequearActualizacionGitHub();
  }
}
