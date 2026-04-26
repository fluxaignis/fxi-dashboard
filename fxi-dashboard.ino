#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>
#include <LittleFS.h>
#include <FS.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>

// ==================== BUFFER DE LOGS ====================
#define MAX_LOGS 50
struct LogEntry {
  unsigned long timestamp;
  String level;
  String message;
};
LogEntry logBuffer[MAX_LOGS];
int logIndex = 0;
int logCount = 0;

void addLog(String level, String message) {
  logBuffer[logIndex].timestamp = millis();
  logBuffer[logIndex].level = level;
  logBuffer[logIndex].message = message;
  logIndex = (logIndex + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS) logCount++;
  Serial.printf("[%s] %s\n", level.c_str(), message.c_str());
}

// ==================== CONFIGURACIÓN DE GITHUB OTA ====================
String FIRMWARE_VERSION = "AUTO_VERSION";
const char* urlVersion = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/version.txt";
const char* urlFirmware = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/firmware.bin";
const char* urlHTML = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/esp32/index.html";

unsigned long ultimoChequeoOTA = 0;
const unsigned long INTERVALO_OTA = 60000;      // 1 minuto (pruebas). Cambiar a 3600000 en producción

// ==================== MAPEO DE PINES ====================
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

// ==================== ESTADOS Y TIEMPOS DE LA RUTINA ====================
enum EstadoSistema { REPOSO, ESPERANDO_AGUA, GIRANDO_IZQ, GIRANDO_DER, VOLVIENDO_CENTRO };
EstadoSistema estadoActual = REPOSO;
const int SERVO_H_NEUTRAL = 94;
unsigned long TIEMPO_90_IZQ = 750;
unsigned long TIEMPO_90_DER = 780;
unsigned long TIEMPO_RETORNO = 500;
unsigned long cronometroRutina = 0;
const unsigned long WATER_DELAY_MS = 1000;

// ==================== VARIABLES DE SEGURIDAD Y SENSORES ====================
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

// ==================== CREDENCIALES ====================
const char* ssid = "NauticaNet";
const char* password = "PromoXXX.2026";
const char* mqtt_server = "df734b8fbeed43978f29869442892dcf.s1.eu.hivemq.cloud";
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -14400;
const int daylightOffset_sec = 0;

// ==================== CONFIGURACIÓN DEL PUNTO DE ACCESO ====================
const char* ap_ssid = "FLUXA IGNIS";
const char* ap_password = "";
IPAddress apIP(192, 168, 1, 1);
IPAddress apGateway(192, 168, 1, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ==================== FUNCIONES AUXILIARES ====================
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
    addLog("alert", "¡EMERGENCIA! Iniciando maniobra de extinción... Motivo: " + motivo);
    digitalWrite(PIN_BOMBA, HIGH);
    cronometroRutina = millis();
    estadoActual = ESPERANDO_AGUA;
  }
}

void detenerRutina() {
  servoHorizontal.write(SERVO_H_NEUTRAL);
  digitalWrite(PIN_BOMBA, LOW);
  estadoActual = REPOSO;
  addLog("info", "Rutina detenida.");
}

// ==================== CALLBACK MQTT ====================
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
      addLog("warn", "Simulación ACTIVADA");
    } else if (msg == "FUEGO_OFF") {
      simularFuego = false;
      addLog("info", "Simulación DESACTIVADA");
    }
  }
}

// ==================== MANEJADORES WEB ====================
void handleRoot() {
  // Cabeceras anti-caché
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
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
  server.send(200, "text/html", "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='0;url=http://192.168.1.1/'></head><body></body></html>");
}

// ==================== ENDPOINTS PARA ADMIN ====================
void handleInfo() {
  DynamicJsonDocument doc(512);
  doc["firmware"] = FIRMWARE_VERSION;
  doc["uptime"] = millis() / 1000;

  size_t heapTotal = ESP.getHeapSize();
  size_t heapFree = ESP.getFreeHeap();
  doc["heap_percent"] = (heapTotal > 0) ? (heapFree * 100) / heapTotal : 0;

  uint8_t littlefsPercent = 30; // fallback
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  if (total > 0) littlefsPercent = (used * 100) / total;
  doc["littlefs_percent"] = littlefsPercent;

  doc["ip_ap"] = WiFi.softAPIP().toString();
  doc["ip_sta"] = WiFi.localIP().toString();
  doc["mqtt_status"] = client.connected() ? "conectado" : "desconectado";
  doc["mdns"] = "fluxaignis.local";

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleLogs() {
  DynamicJsonDocument doc(2048);
  JsonArray logs = doc.createNestedArray("logs");
  int start = (logIndex - logCount + MAX_LOGS) % MAX_LOGS;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % MAX_LOGS;
    JsonObject entry = logs.createNestedObject();
    entry["timestamp"] = logBuffer[idx].timestamp;
    entry["level"] = logBuffer[idx].level;
    entry["message"] = logBuffer[idx].message;
  }
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleCmd() {
  if (!server.hasArg("action")) {
    server.send(400, "text/plain", "Missing action");
    return;
  }
  String action = server.arg("action");
  addLog("info", "Comando recibido: " + action);

  if (action == "restart") {
    server.send(200, "text/plain", "OK");
    delay(100);
    ESP.restart();
  } 
  else if (action == "check_ota") {
    ultimoChequeoOTA = 0;
    server.send(200, "text/plain", "OK");
  } 
  else if (action == "toggle_sim") {
    simularFuego = !simularFuego;
    addLog("warn", simularFuego ? "Simulación de fuego ACTIVADA" : "Simulación DESACTIVADA");
    server.send(200, "text/plain", "OK");
  } 
  else if (action == "clear_logs") {
    logIndex = 0;
    logCount = 0;
    addLog("info", "Logs limpiados por comando");
    server.send(200, "text/plain", "OK");
  } 
  else if (action == "update_fw") {
    server.send(200, "text/plain", "OK");
    chequearActualizacionGitHub();   // fuerza OTA
  } 
  else if (action == "update_html") {
    server.send(200, "text/plain", "OK");
    actualizarHTML();
  } 
  else {
    server.send(400, "text/plain", "Unknown action");
  }
}

// ==================== ACTUALIZACIONES (OTA y HTML) ====================
void guardarVersion(String version) {
  File f = LittleFS.open("/version.txt", "w");
  if (f) {
    f.print(version);
    f.close();
    addLog("info", "Versión guardada en LittleFS: " + version);
  } else {
    addLog("error", "Error al guardar versión");
  }
}

String leerVersion() {
  File f = LittleFS.open("/version.txt", "r");
  if (!f) {
    addLog("warn", "No se encontró version.txt, se usará AUTO_VERSION");
    return "AUTO_VERSION";
  }
  String v = f.readString();
  v.trim();
  f.close();
  addLog("info", "Versión leída de LittleFS: " + v);
  return v;
}

bool actualizarHTML() {
  if (WiFi.status() != WL_CONNECTED) return false;

  addLog("info", "Descargando nueva versión del HTML...");
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(espClient, urlHTML);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    addLog("error", "Error al descargar HTML: " + String(httpCode));
    http.end();
    return false;
  }

  String nuevoHTML = http.getString();
  http.end();

  if (nuevoHTML.length() < 100) {
    addLog("error", "HTML descargado demasiado pequeño, ignorando");
    return false;
  }

  File f = LittleFS.open("/index.html", "r");
  if (f) {
    String actualHTML = f.readString();
    f.close();
    if (actualHTML == nuevoHTML) {
      addLog("info", "HTML ya está actualizado");
      return false;
    }
  }

  f = LittleFS.open("/index.html", "w");
  if (!f) {
    addLog("error", "Error al guardar HTML");
    return false;
  }
  f.print(nuevoHTML);
  f.close();
  addLog("info", "HTML actualizado correctamente");
  return true;
}

void chequearActualizacionGitHub() {
  if (WiFi.status() != WL_CONNECTED) {
    addLog("warn", "Sin conexión WiFi externa. Se omite chequeo OTA de GitHub.");
    return;
  }

  addLog("info", "Chequeando actualizaciones en GitHub...");

  HTTPClient http;
  http.setTimeout(10000);
  http.begin(espClient, urlVersion);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String versionGitHub = http.getString();
    versionGitHub.trim();

    String currentVersion = leerVersion();
    addLog("info", "Versión actual: " + currentVersion + " | GitHub: " + versionGitHub);

    if (versionGitHub != currentVersion && versionGitHub.length() > 0) {
      addLog("info", "¡Nueva versión detectada! Iniciando OTA...");
      httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      t_httpUpdate_return ret = httpUpdate.update(espClient, urlFirmware);

      switch (ret) {
        case HTTP_UPDATE_FAILED:
          addLog("error", "Error en OTA: " + String(httpUpdate.getLastError()));
          break;
        case HTTP_UPDATE_NO_UPDATES:
          addLog("info", "No hay actualizaciones disponibles");
          break;
        case HTTP_UPDATE_OK:
          guardarVersion(versionGitHub);
          actualizarHTML();
          addLog("success", "¡Actualización completada! Reiniciando...");
          ESP.restart();
          break;
      }
    } else {
      addLog("info", "El firmware ya está en la última versión.");
    }
  } else {
    addLog("error", "Error al consultar versión en GitHub. Código: " + String(httpCode));
    if (httpCode == -1) addLog("error", "Posible fallo de conexión o DNS. Verifica la URL.");
  }
  http.end();
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  addLog("info", "Sistema iniciando...");

  dht.begin();
  pinMode(PIN_BOMBA, OUTPUT); digitalWrite(PIN_BOMBA, LOW);
  pinMode(PIN_ROJO, OUTPUT); pinMode(PIN_VERDE, OUTPUT); pinMode(PIN_AZUL, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT); noTone(PIN_BUZZER);

  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2); ESP32PWM::allocateTimer(3);
  servoHorizontal.setPeriodHertz(50); servoHorizontal.attach(PIN_SERVO_H, 500, 2400);
  servoVertical.setPeriodHertz(50); servoVertical.attach(PIN_SERVO_V, 500, 2400);
  servoHorizontal.write(SERVO_H_NEUTRAL); servoVertical.write(20);

  if (!LittleFS.begin()) {
    Serial.println("Error al montar LittleFS");
    addLog("error", "Error al montar LittleFS");
    return;
  }

  FIRMWARE_VERSION = leerVersion();
  addLog("info", "Firmware version: " + FIRMWARE_VERSION);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(ap_ssid, ap_password);
  addLog("info", "Hotspot creado: " + String(ap_ssid) + " - IP: " + WiFi.softAPIP().toString());

  dnsServer.start(53, "*", apIP);
  addLog("info", "DNS Captive Portal activado");

  if (MDNS.begin("fluxaignis")) {
    addLog("info", "mDNS iniciado: fluxaignis.local");
    MDNS.addService("http", "tcp", 80);
  } else {
    addLog("warn", "Error al iniciar mDNS");
  }

  WiFi.begin(ssid, password);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    addLog("info", "WiFi externo conectado - IP: " + WiFi.localIP().toString());
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    espClient.setInsecure();
    client.setServer(mqtt_server, 8883);
    client.setCallback(callback);
  } else {
    addLog("warn", "No se pudo conectar a red externa. Modo offline + hotspot.");
  }

  ArduinoOTA.setHostname("fluxaignis_ota");
  ArduinoOTA.setPassword("12345678");
  ArduinoOTA.begin();
  addLog("info", "OTA clásico iniciado.");

  server.on("/", handleRoot);
  server.on("/estado", handleEstado);
  server.on("/toggleServo", handleToggle);
  server.on("/generate_204", []() { server.send(204); });
  server.on("/info", handleInfo);
  server.on("/logs", handleLogs);
  server.on("/cmd", handleCmd);
  server.onNotFound(handleNotFound);
  server.begin();

  addLog("info", "Servidor web listo. Accede a http://192.168.1.1");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();

  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      if (client.connect("ESP32_FXI", "Admin", "FluxaIgnis2026")) {
        client.subscribe("fxi/comandos");
        client.subscribe("fxi/simular");
        addLog("info", "MQTT conectado");
      }
    } else {
      client.loop();
    }
  }

  unsigned long ahora = millis();

  if (ahora - ultimoTiempoDHT >= 2000) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (t > 0 && t < 80) tempGuardada = t;
    if (!isnan(h)) humGuardada = h;
    ultimoTiempoDHT = ahora;
  }

  rssiGuardado = WiFi.RSSI();

  int lecturaLlama = (simularFuego) ? 300 : analogRead(PIN_LLAMA);
  if (lecturaLlama < 50) lecturaLlama = 4095;
  if (estadoActual == REPOSO && lecturaLlama < UMBRAL_FUEGO) {
    String motivo = simularFuego ? "FUEGO SIMULADO (MQTT)" : "FUEGO DETECTADO (KY-026)";
    iniciarRutina(motivo);
    if (!emergenciaEnviada) {
      enviarNotificacionMQTT(motivo, tempGuardada);
      emergenciaEnviada = true;
    }
  }

  if (tempGuardada >= TEMP_CRITICA) {
    if (!emergenciaEnviada) {
      enviarNotificacionMQTT("CALOR CRITICO", tempGuardada);
      emergenciaEnviada = true;
      iniciarRutina("Temperatura crítica superada");
    }
  } else if (tempGuardada < TEMP_CRITICA - 2.0) {
    emergenciaEnviada = false;
  }

  if (estadoActual != REPOSO) {
    setColor(255, 0, 0);
    if ((ahora / 300) % 2 == 0) tone(PIN_BUZZER, 2000);
    else noTone(PIN_BUZZER);
  } else {
    noTone(PIN_BUZZER);
    int r = (tempGuardada >= 35) ? 255 : (tempGuardada >= 30 ? 255 : 0);
    int g = (tempGuardada >= 35) ? 0 : (tempGuardada >= 30 ? 100 : 255);
    int b = (tempGuardada < 30) ? 255 : 0;
    setColor(r, g, b);
  }

  switch (estadoActual) {
    case REPOSO: break;
    case ESPERANDO_AGUA:
      if (ahora - cronometroRutina >= WATER_DELAY_MS) {
        addLog("info", "Agua lista, iniciando barrido");
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

  if (ahora - ultimoChequeoOTA >= INTERVALO_OTA) {
    ultimoChequeoOTA = ahora;
    chequearActualizacionGitHub();
  }

  static unsigned long ultimaActualizacionHTML = 0;
  if (ahora - ultimaActualizacionHTML >= 86400000UL) {
    if (WiFi.status() == WL_CONNECTED) {
      actualizarHTML();
    }
    ultimaActualizacionHTML = ahora;
  }
}
