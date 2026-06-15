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
#define MAX_LOGS 30
struct LogEntry {
  unsigned long timestamp;
  String level;
  String message;
};
LogEntry logBuffer[MAX_LOGS];
int logIndex = 0;
int logCount = 0;

void addLog(String level, String message) {
  if (message.length() > 100) message = message.substring(0, 97) + "...";
  logBuffer[logIndex].timestamp = millis();
  logBuffer[logIndex].level = level;
  logBuffer[logIndex].message = message;
  logIndex = (logIndex + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS) logCount++;
  Serial.printf("[%s] %s\n", level.c_str(), message.c_str());
}

// ==================== GITHUB OTA ====================
String FIRMWARE_VERSION = "1.0.43";
const char* urlVersion = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/version.txt";
const char* urlFirmware = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/firmware.bin";
const char* urlHTML = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/main/index.html";
unsigned long ultimoChequeoOTA = 0;
const unsigned long INTERVALO_OTA = 60001;

// ==================== PINES ====================
#define DHTPIN 27
#define DHTTYPE DHT11
#define PIN_SERVO_H 15          // Servo horizontal (360°)
#define PIN_SERVO_V 26
#define PIN_BOMBA 32
#define PIN_ROJO 13
#define PIN_VERDE 12
#define PIN_AZUL 14
#define PIN_LLAMA_IZQ 35        // KY-026 izquierdo (analógico)
#define PIN_LLAMA_DER 36        // KY-026 derecho (analógico)
#define PIN_MQ2 34              // Sensor MQ-2 (analógico)
#define PIN_BUZZER 33

DHT dht(DHTPIN, DHTTYPE);
Servo servoHorizontal;
Servo servoVertical;
WiFiClientSecure espClient;
PubSubClient client(espClient);
WebServer server(80);
DNSServer dnsServer;

// ==================== ESTADOS Y TIEMPOS DE LA RUTINA ====================
enum EstadoSistema { REPOSO, ESPERANDO_AGUA, APUNTANDO, EXTINGUIENDO };
EstadoSistema estadoActual = REPOSO;

// Ángulos para servo 360: 90 = parado, 0 = máximo un sentido, 180 = máximo opuesto.
// Usaremos posiciones para apuntar (simulando ángulo fijo durante unos segundos).
const int SERVO_IZQ = 0;      // Giro continuo hacia la izquierda
const int SERVO_DER = 180;    // Giro continuo hacia la derecha
const int SERVO_CENTRO = 90;   // Parado

unsigned long TIEMPO_APUNTAR = 2000;   // Tiempo que se mantiene girando hacia el lado (ms)
unsigned long cronometroRutina = 0;
const unsigned long WATER_DELAY_MS = 1000; // Bomba encendida 1s antes de mover servo

int ladoEmergencia = 0; // 0 = centro, 1 = izquierda, 2 = derecha, 3 = ambos (barrido)
bool emergenciaActiva = false;

// ==================== VARIABLES DE SEGURIDAD ====================
const int UMBRAL_FUEGO = 500;        // Para KY-026 (cuanto más bajo, más cerca)
const int UMBRAL_GAS = 500;          // Para MQ-2 (ajustar)
const float TEMP_CRITICA = 40.0;
bool emergenciaEnviada = false;
float tempGuardada = 25.0;
float humGuardada = 50.0;
int rssiGuardado = -99;
int gasValue = 0;
unsigned long cronometroDatos = 0;
unsigned long cronometroRSSI = 0;
unsigned long ultimoTiempoDHT = 0;
bool simularFuego = false;

// Lectura de sensores de llama
int llamaIzq = 4095;
int llamaDer = 4095;

// ==================== CREDENCIALES ====================
const char* ssid = "NauticaNet";
const char* password = "PromoXXX.2026";
const char* mqtt_server = "df734b8fbeed43978f29869442892dcf.s1.eu.hivemq.cloud";
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -14400;
const int daylightOffset_sec = 0;

// ==================== AP ====================
const char* ap_ssid = "FluxaIgnis TECH";
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

void enviarNotificacionMQTT(String motivo, float temp, int gas = -1) {
  if (!client.connected()) return;
  struct tm timeinfo;
  char fechaHora[30];
  if (!getLocalTime(&timeinfo)) strcpy(fechaHora, "Error de tiempo");
  else strftime(fechaHora, 30, "%I:%M%p %d/%m/%Y", &timeinfo);
  String payload = "{\"motivo\":\"" + motivo + "\",\"temperatura\":" + String(temp);
  if (gas >= 0) payload += ",\"gas\":" + String(gas);
  payload += ",\"timestamp\":\"" + String(fechaHora) + "\"}";
  client.publish("fxi/emergencia", payload.c_str());
}

void iniciarRutina(int lado, String motivo) {
  if (estadoActual == REPOSO) {
    addLog("alert", "¡EMERGENCIA! " + motivo + " - Lado: " + (lado == 1 ? "IZQUIERDO" : (lado == 2 ? "DERECHO" : "CENTRO")));
    digitalWrite(PIN_BOMBA, HIGH);
    ladoEmergencia = lado;
    cronometroRutina = millis();
    estadoActual = ESPERANDO_AGUA;
    emergenciaActiva = true;
  }
}

void detenerRutina() {
  servoHorizontal.write(SERVO_CENTRO);
  digitalWrite(PIN_BOMBA, LOW);
  estadoActual = REPOSO;
  ladoEmergencia = 0;
  emergenciaActiva = false;
  addLog("info", "Rutina detenida.");
}

// ==================== MQTT ADMIN (sin cambios, solo se copia) ====================
void respondAdminCommand(int id, bool success, JsonDocument &data, const String &errorMsg = "") {
  if (!client.connected()) return;
  DynamicJsonDocument resp(1024);
  resp["id"] = id;
  resp["success"] = success;
  if (success) resp["data"] = data;
  else resp["error"] = errorMsg;
  String out;
  serializeJson(resp, out);
  client.publish("fxi/admin/resp", out.c_str());
}

void processAdminCommand(int id, const String &action) {
  addLog("info", "Comando MQTT admin: " + action + " (id=" + String(id) + ")");
  if (action == "get_stats") {
    DynamicJsonDocument data(512);
    data["firmware"] = FIRMWARE_VERSION;
    data["uptime"] = millis() / 1000;
    size_t heapTotal = ESP.getHeapSize();
    size_t heapFree = ESP.getFreeHeap();
    data["heap_percent"] = (heapTotal > 0) ? (heapFree * 100) / heapTotal : 0;
    data["littlefs_percent"] = 30;
    size_t sketchSize = ESP.getSketchSize();
    size_t sketchTotal = 1992294;
    data["sketch_percent"] = (sketchSize * 100) / sketchTotal;
    data["ip_ap"] = WiFi.softAPIP().toString();
    data["ip_sta"] = WiFi.localIP().toString();
    data["mqtt_status"] = client.connected() ? "conectado" : "desconectado";
    data["mdns"] = "fluxaignis.local";
    respondAdminCommand(id, true, data);
  }
  else if (action == "get_logs") {
    DynamicJsonDocument data(6144);
    JsonArray logs = data.createNestedArray("logs");
    int start = (logIndex - logCount + MAX_LOGS) % MAX_LOGS;
    for (int i = 0; i < logCount; i++) {
      int idx = (start + i) % MAX_LOGS;
      JsonObject entry = logs.createNestedObject();
      entry["timestamp"] = logBuffer[idx].timestamp;
      entry["level"] = logBuffer[idx].level;
      entry["message"] = logBuffer[idx].message;
    }
    respondAdminCommand(id, true, data);
  }
  else if (action == "restart") {
    DynamicJsonDocument empty(64);
    respondAdminCommand(id, true, empty);
    delay(100);
    ESP.restart();
  }
  else if (action == "update_fw") {
    DynamicJsonDocument empty(64);
    respondAdminCommand(id, true, empty);
    chequearActualizacionGitHub();
  }
  else if (action == "update_html") {
    DynamicJsonDocument empty(64);
    respondAdminCommand(id, true, empty);
    actualizarHTML();
  }
  else if (action == "toggle_sim") {
    simularFuego = !simularFuego;
    DynamicJsonDocument data(32);
    data["simulating"] = simularFuego;
    respondAdminCommand(id, true, data);
    addLog("warn", simularFuego ? "Simulación ACTIVADA" : "Simulación DESACTIVADA");
  }
  else if (action == "clear_logs") {
    logIndex = 0; logCount = 0;
    DynamicJsonDocument empty(64);
    respondAdminCommand(id, true, empty);
    addLog("info", "Logs limpiados");
  }
  else if (action == "ping") {
    DynamicJsonDocument data(32);
    data["pong"] = true;
    respondAdminCommand(id, true, data);
  }
  else {
    DynamicJsonDocument empty(64);
    respondAdminCommand(id, false, empty, "Unknown action: " + action);
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  String topicStr = String(topic);
  if (topicStr == "fxi/comandos") {
    if (msg == "TOGGLE") {
      if (estadoActual == REPOSO) iniciarRutina(0, "Comando Manual MQTT (centro)");
      else detenerRutina();
    }
  }
  else if (topicStr == "fxi/simular") {
    if (msg == "FUEGO_ON") simularFuego = true;
    else if (msg == "FUEGO_OFF") simularFuego = false;
  }
  else if (topicStr == "fxi/admin/cmd") {
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, msg);
    if (error) return;
    int id = doc["id"] | 0;
    String action = doc["action"] | "";
    if (action.length() == 0) return;
    processAdminCommand(id, action);
  }
}

// ==================== MANEJADORES WEB ====================
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
  json += "\"gas\":" + String(gasValue) + ",";
  json += "\"llama_izq\":" + String(llamaIzq) + ",";
  json += "\"llama_der\":" + String(llamaDer) + ",";
  json += "\"rssi\":" + String(rssiGuardado) + "}";
  server.send(200, "application/json", json);
}

void handleToggle() {
  if (estadoActual == REPOSO) iniciarRutina(0, "Comando Web (centro)");
  else detenerRutina();
  server.send(200, "text/plain", (estadoActual != REPOSO) ? "ON" : "OFF");
}

void handleNotFound() {
  server.send(200, "text/html", "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='0;url=http://192.168.1.1/'></head><body></body></html>");
}

void handleInfo() {
  DynamicJsonDocument doc(512);
  doc["firmware"] = FIRMWARE_VERSION;
  doc["uptime"] = millis() / 1000;
  size_t heapTotal = ESP.getHeapSize();
  size_t heapFree = ESP.getFreeHeap();
  doc["heap_percent"] = (heapTotal > 0) ? (heapFree * 100) / heapTotal : 0;
  doc["littlefs_percent"] = 30;
  size_t sketchSize = ESP.getSketchSize();
  size_t sketchTotal = 1992294;
  doc["sketch_percent"] = (sketchSize * 100) / sketchTotal;
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
  addLog("info", "Comando HTTP: " + action);
  if (action == "restart") {
    server.send(200, "text/plain", "OK");
    delay(100);
    ESP.restart();
  } else if (action == "check_ota") {
    ultimoChequeoOTA = 0;
    server.send(200, "text/plain", "OK");
  } else if (action == "toggle_sim") {
    simularFuego = !simularFuego;
    server.send(200, "text/plain", "OK");
  } else if (action == "clear_logs") {
    logIndex = 0; logCount = 0;
    server.send(200, "text/plain", "OK");
  } else if (action == "update_fw") {
    server.send(200, "text/plain", "OK");
    chequearActualizacionGitHub();
  } else if (action == "update_html") {
    server.send(200, "text/plain", "OK");
    actualizarHTML();
  } else {
    server.send(400, "text/plain", "Unknown action");
  }
}

// ==================== ACTUALIZACIONES (OTA y HTML) ====================
void guardarVersion(String version) {
  File f = LittleFS.open("/version.txt", "w");
  if (f) {
    f.print(version);
    f.close();
    addLog("info", "Versión guardada: " + version);
  }
}

String leerVersion() {
  File f = LittleFS.open("/version.txt", "r");
  if (!f) return "1.0.43";
  String v = f.readString();
  v.trim();
  f.close();
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
  if (nuevoHTML.length() < 100) return false;
  File f = LittleFS.open("/index.html", "w");
  if (!f) return false;
  f.print(nuevoHTML);
  f.close();
  addLog("info", "HTML actualizado");
  return true;
}

bool isNewerVersion(String remote, String current) {
  remote.replace("v", ""); current.replace("v", "");
  int rMaj, rMin, rPat, cMaj, cMin, cPat;
  if (sscanf(remote.c_str(), "%d.%d.%d", &rMaj, &rMin, &rPat) != 3) return false;
  if (sscanf(current.c_str(), "%d.%d.%d", &cMaj, &cMin, &cPat) != 3) return true;
  if (rMaj > cMaj) return true;
  if (rMaj < cMaj) return false;
  if (rMin > cMin) return true;
  if (rMin < cMin) return false;
  return (rPat > cPat);
}

void chequearActualizacionGitHub() {
  if (WiFi.status() != WL_CONNECTED) return;
  addLog("info", "Chequeando actualizaciones...");
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(espClient, urlVersion);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String versionGitHub = http.getString();
    versionGitHub.trim();
    String currentVersion = leerVersion();
    if (isNewerVersion(versionGitHub, currentVersion) && versionGitHub.length() > 0) {
      addLog("info", "Nueva versión detectada. Iniciando OTA...");
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
          addLog("success", "Actualización completada. Reiniciando...");
          ESP.restart();
          break;
      }
    } else {
      addLog("info", "Firmware actualizado.");
    }
  } else {
    addLog("error", "Error al consultar versión: " + String(httpCode));
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
  pinMode(PIN_MQ2, INPUT);
  pinMode(PIN_LLAMA_IZQ, INPUT);
  pinMode(PIN_LLAMA_DER, INPUT);

  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2); ESP32PWM::allocateTimer(3);
  servoHorizontal.setPeriodHertz(50); servoHorizontal.attach(PIN_SERVO_H, 500, 2400);
  servoVertical.setPeriodHertz(50); servoVertical.attach(PIN_SERVO_V, 500, 2400);
  servoHorizontal.write(SERVO_CENTRO); servoVertical.write(20);

  if (!LittleFS.begin()) {
    Serial.println("Error al montar LittleFS");
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
    MDNS.addService("http", "tcp", 80);
    addLog("info", "mDNS iniciado como fluxaignis.local");
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
    MDNS.end();
    if (MDNS.begin("fluxaignis")) {
      MDNS.addService("http", "tcp", 80);
      addLog("info", "mDNS re-registrado en STA");
    }
    chequearActualizacionGitHub();
  } else {
    addLog("warn", "Modo offline + hotspot.");
  }

  ArduinoOTA.setHostname("fluxaignis_ota");
  ArduinoOTA.setPassword("12345678");
  ArduinoOTA.begin();
  addLog("info", "OTA clásico iniciado");

  server.on("/", handleRoot);
  server.on("/estado", handleEstado);
  server.on("/toggleServo", handleToggle);
  server.on("/generate_204", []() { server.send(204); });
  server.on("/info", handleInfo);
  server.on("/logs", handleLogs);
  server.on("/cmd", handleCmd);
  server.onNotFound(handleNotFound);
  server.begin();

  addLog("info", "Servidor web listo. Accede a http://192.168.1.1 o http://fluxaignis.local");

  ultimoChequeoOTA = millis();

  Serial.println("\n=== Comandos disponibles por Serial ===");
  Serial.println("  help      - Muestra esta ayuda");
  Serial.println("  update_fw - Forzar OTA del firmware");
  Serial.println("  fw        - (abreviatura)");
  Serial.println("  update_html - Forzar actualización del HTML");
  Serial.println("  html      - (abreviatura)");
  Serial.println("  restart   - Reiniciar ESP32");
  Serial.println("========================================\n");
}

// ==================== LOOP PRINCIPAL ====================
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();

  // Comandos seriales
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    if (cmd == "help") {
      Serial.println("Comandos: update_fw, fw, update_html, html, restart, help");
    } else if (cmd == "update_fw" || cmd == "fw") {
      chequearActualizacionGitHub();
    } else if (cmd == "update_html" || cmd == "html") {
      actualizarHTML();
    } else if (cmd == "restart") {
      ESP.restart();
    } else {
      Serial.println("Comando no reconocido.");
    }
  }

  // MQTT
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      if (client.connect("ESP32_FXI", "Admin", "FluxaIgnis2026")) {
        client.subscribe("fxi/comandos");
        client.subscribe("fxi/simular");
        client.subscribe("fxi/admin/cmd");
        addLog("info", "MQTT conectado");
      }
    } else {
      client.loop();
    }
  }

  unsigned long ahora = millis();

  // DHT11 cada 2s
  if (ahora - ultimoTiempoDHT >= 2000) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (t > 0 && t < 80) tempGuardada = t;
    if (!isnan(h)) humGuardada = h;
    ultimoTiempoDHT = ahora;
  }

  // Leer sensores de llama y gas
  llamaIzq = analogRead(PIN_LLAMA_IZQ);
  llamaDer = analogRead(PIN_LLAMA_DER);
  gasValue = analogRead(PIN_MQ2);
  rssiGuardado = WiFi.RSSI();

  // Detección de emergencias (solo si estamos en reposo)
  if (estadoActual == REPOSO && !emergenciaActiva) {
    bool fuegoIzq = (llamaIzq < UMBRAL_FUEGO);
    bool fuegoDer = (llamaDer < UMBRAL_FUEGO);
    bool hayGas = (gasValue > UMBRAL_GAS);
    bool calorCritico = (tempGuardada >= TEMP_CRITICA);
    bool simulacion = simularFuego;

    int lado = 0; // 0=centro, 1=izquierda, 2=derecha, 3=ambos
    String motivo = "";

    if (simulacion) {
      lado = 0;
      motivo = "SIMULACIÓN DE FUEGO (MQTT)";
    } else if (calorCritico) {
      lado = 0;
      motivo = "TEMPERATURA CRÍTICA";
    } else if (hayGas) {
      lado = 0;
      motivo = "GAS COMBUSTIBLE DETECTADO (MQ-2)";
    } else if (fuegoIzq && fuegoDer) {
      lado = 3;
      motivo = "FUEGO EN AMBOS SENSORES";
    } else if (fuegoIzq) {
      lado = 1;
      motivo = "FUEGO IZQUIERDO (KY-026)";
    } else if (fuegoDer) {
      lado = 2;
      motivo = "FUEGO DERECHO (KY-026)";
    }

    if (lado != 0 || motivo != "") {
      iniciarRutina(lado, motivo);
      if (!emergenciaEnviada) {
        enviarNotificacionMQTT(motivo, tempGuardada, gasValue);
        emergenciaEnviada = true;
      }
    }
  } else if (tempGuardada < TEMP_CRITICA - 2.0 && !emergenciaActiva) {
    emergenciaEnviada = false;
  }

  // Control de LEDs y buzzer
  if (estadoActual != REPOSO) {
    setColor(255, 0, 0);  // Rojo fijo
    if ((ahora / 300) % 2 == 0) tone(PIN_BUZZER, 2000);
    else noTone(PIN_BUZZER);
  } else {
    noTone(PIN_BUZZER);
    // En reposo, colores según temperatura (simple)
    int r = (tempGuardada >= 35) ? 255 : (tempGuardada >= 30 ? 255 : 0);
    int g = (tempGuardada >= 35) ? 0 : (tempGuardada >= 30 ? 100 : 255);
    int b = (tempGuardada < 30) ? 255 : 0;
    setColor(r, g, b);
  }

  // Máquina de estados para la extinción
  switch (estadoActual) {
    case REPOSO:
      break;
    case ESPERANDO_AGUA:
      if (ahora - cronometroRutina >= WATER_DELAY_MS) {
        addLog("info", "Agua lista, apuntando servo");
        // Orientar servo según el lado
        switch (ladoEmergencia) {
          case 1: servoHorizontal.write(SERVO_IZQ); break;
          case 2: servoHorizontal.write(SERVO_DER); break;
          case 3: // Ambos: podrías hacer un barrido o elegir centro. Aquí centro.
          default: servoHorizontal.write(SERVO_CENTRO); break;
        }
        cronometroRutina = ahora;
        estadoActual = APUNTANDO;
      }
      break;
    case APUNTANDO:
      if (ahora - cronometroRutina >= TIEMPO_APUNTAR) {
        addLog("info", "Deteniendo extinción");
        detenerRutina();
      }
      break;
    default:
      break;
  }

  // Publicación MQTT de datos
  if (WiFi.status() == WL_CONNECTED && client.connected()) {
    if (ahora - cronometroDatos > 2000) {
      cronometroDatos = ahora;
      String hStr = isnan(humGuardada) ? "0" : String(humGuardada);
      String payload = "{\"temp\":" + String(tempGuardada) + ",\"hum\":" + hStr + ",\"gas\":" + String(gasValue) +
                       ",\"llama_izq\":" + String(llamaIzq) + ",\"llama_der\":" + String(llamaDer) + "}";
      client.publish("fxi/datos", payload.c_str());
      client.publish("fxi/gas", String(gasValue).c_str());
    }
    if (ahora - cronometroRSSI > 5000) {
      cronometroRSSI = ahora;
      String rssiPayload = "{\"rssi\":" + String(rssiGuardado) + "}";
      client.publish("fxi/rssi", rssiPayload.c_str());
    }
  }

  // OTA periódica
  if (ahora - ultimoChequeoOTA >= INTERVALO_OTA) {
    ultimoChequeoOTA = ahora;
    chequearActualizacionGitHub();
  }

  // Actualización HTML diaria
  static unsigned long ultimaActualizacionHTML = 0;
  if (ahora - ultimaActualizacionHTML >= 86400000UL) {
    if (WiFi.status() == WL_CONNECTED) actualizarHTML();
    ultimaActualizacionHTML = ahora;
  }
}
