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

// ==================== COMPENSACIÓN DE TEMPERATURA ====================
const float OFFSET_TEMP = 5.0;

// ==================== BUFFER DE LOGS ====================
#define MAX_LOGS 20
struct LogEntry {
  unsigned long timestamp;
  String level;
  String message;
};
LogEntry logBuffer[MAX_LOGS];
int logIndex = 0;
int logCount = 0;

// ==================== GITHUB OTA ====================
String FIRMWARE_VERSION = "AUTO_VERSION";
const char* urlVersion = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/version.txt";
const char* urlFirmware = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/firmware.bin";
const char* urlHTML = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/main/index.html";
unsigned long ultimoChequeoOTA = 0;
const unsigned long INTERVALO_OTA = 30000;   // 30 segundos (pruebas)

// ==================== PINES ====================
#define DHTPIN 27
#define DHTTYPE DHT11
#define PIN_SERVO_H 15
#define PIN_SERVO_V 26
#define PIN_BOMBA 32
#define PIN_ROJO 13
#define PIN_VERDE 12
#define PIN_AZUL 14
#define PIN_LLAMA_IZQ 35
#define PIN_LLAMA_DER 34
#define PIN_MQ2 33
#define PIN_BUZZER 17

DHT dht(DHTPIN, DHTTYPE);
Servo servoHorizontal;
Servo servoVertical;
WiFiClientSecure espClient;
PubSubClient client(espClient);
WebServer server(80);
DNSServer dnsServer;

// ==================== addLog ====================
void addLog(String level, String message) {
  if (message.length() > 100) message = message.substring(0, 97) + "...";
  logBuffer[logIndex].timestamp = millis();
  logBuffer[logIndex].level = level;
  logBuffer[logIndex].message = message;
  logIndex = (logIndex + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS) logCount++;
  Serial.printf("[%s] %s\n", level.c_str(), message.c_str());
  
  if (client.connected()) {
    String logPayload = "{\"timestamp\":" + String(millis()) + ",\"level\":\"" + level + "\",\"message\":\"" + message + "\"}";
    client.publish("fxi/logs", logPayload.c_str());
  }
}

// ==================== ESTADOS Y TIEMPOS ====================
enum EstadoSistema { REPOSO, ESPERANDO_AGUA, APUNTANDO };
EstadoSistema estadoActual = REPOSO;
const int SERVO_IZQ = 0;
const int SERVO_DER = 180;
const int SERVO_CENTRO = 90;
unsigned long TIEMPO_APUNTAR = 2000;
unsigned long cronometroRutina = 0;
const unsigned long WATER_DELAY_MS = 1000;
int ladoEmergencia = 0;
bool emergenciaActiva = false;

// ==================== UMBRALES DINÁMICOS ====================
float dangerTempThreshold = 40.0;       // temperatura peligrosa
float umbralDesactivacionFuego = 35.0;  // para desactivar emergencia MQTT
int umbralGas = 250;                    // gas (ppm)
const int UMBRAL_FUEGO = 500;           // llama (fijo)

// Archivo de umbrales en LittleFS
const char* THRESHOLD_FILE = "/umbrales.txt";

bool loadThresholds() {
  File f = LittleFS.open(THRESHOLD_FILE, "r");
  if (!f) {
    dangerTempThreshold = 40.0;
    umbralDesactivacionFuego = 35.0;
    umbralGas = 250;
    return false;
  }
  String line = f.readStringUntil('\n');
  line.trim();
  if (line.length() > 0) dangerTempThreshold = line.toFloat();
  line = f.readStringUntil('\n');
  line.trim();
  if (line.length() > 0) umbralDesactivacionFuego = line.toFloat();
  line = f.readStringUntil('\n');
  line.trim();
  if (line.length() > 0) umbralGas = line.toInt();
  f.close();
  addLog("info", "Umbrales cargados: dangerTemp=" + String(dangerTempThreshold) + " fireMqtt=" + String(umbralDesactivacionFuego) + " gas=" + String(umbralGas));
  return true;
}

void saveThresholds() {
  File f = LittleFS.open(THRESHOLD_FILE, "w");
  if (f) {
    f.println(dangerTempThreshold);
    f.println(umbralDesactivacionFuego);
    f.println(umbralGas);
    f.close();
    addLog("info", "Umbrales guardados");
  }
}

// ==================== VARIABLES ====================
bool emergenciaEnviada = false;
float tempGuardada = NAN;
float humGuardada = NAN;
int rssiGuardado = -99;
int gasValue = 0;
unsigned long cronometroDatos = 0;
unsigned long cronometroRSSI = 0;
unsigned long ultimoTiempoDHT = 0;
bool simularFuego = false;
int llamaIzq = 4095;
int llamaDer = 4095;

// ==================== BUZZER ====================
const int BEEPS_PER_CYCLE = 3;
const unsigned long BEEP_ON_MS = 150;
const unsigned long BEEP_OFF_MS = 150;
const unsigned long PAUSE_MS = 500;
int beepCounter = 0;
unsigned long lastBuzzerTime = 0;
bool buzzerState = false;
bool inPause = false;

void updateBuzzer() {
  unsigned long ahora = millis();
  if (estadoActual == REPOSO) {
    if (buzzerState) { noTone(PIN_BUZZER); buzzerState = false; }
    beepCounter = 0; inPause = false; lastBuzzerTime = ahora;
    return;
  }
  if (!inPause) {
    if (!buzzerState) {
      tone(PIN_BUZZER, 2000); buzzerState = true; lastBuzzerTime = ahora;
    } else {
      if (ahora - lastBuzzerTime >= BEEP_ON_MS) {
        noTone(PIN_BUZZER); buzzerState = false; lastBuzzerTime = ahora;
        beepCounter++;
        if (beepCounter >= BEEPS_PER_CYCLE) { inPause = true; beepCounter = 0; }
      }
    }
  } else {
    if (!buzzerState && ahora - lastBuzzerTime >= PAUSE_MS) {
      inPause = false; lastBuzzerTime = ahora;
    }
  }
}

// ==================== CREDENCIALES DINÁMICAS ====================
const char* DEFAULT_SSID = "NauticaNet";
const char* DEFAULT_PASS = "PromoXXX.2026";
String wifiSSID = "";
String wifiPassword = "";
const char* ADMIN_TOKEN = "config2026";

// ==================== AP ====================
const char* ap_ssid = "FluxaIgnis TECH";
const char* ap_password = "";
IPAddress apIP(192, 168, 1, 1);
IPAddress apGateway(192, 168, 1, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ==================== MQTT ====================
const char* mqtt_server = "df734b8fbeed43978f29869442892dcf.s1.eu.hivemq.cloud";
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -14400;
const int daylightOffset_sec = 0;

// ==================== AUXILIARES ====================
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
    addLog("alert", "¡EMERGENCIA! " + motivo);
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
  emergenciaEnviada = false;
  addLog("info", "Rutina detenida.");
}

// ==================== CREDENCIALES WiFi ====================
bool leerCredencialesWiFi() {
  File f = LittleFS.open("/wifi.txt", "r");
  if (!f) { wifiSSID = DEFAULT_SSID; wifiPassword = DEFAULT_PASS; return false; }
  wifiSSID = f.readStringUntil('\n');
  wifiPassword = f.readStringUntil('\n');
  wifiSSID.trim(); wifiPassword.trim(); f.close();
  if (wifiSSID.length() == 0) { wifiSSID = DEFAULT_SSID; wifiPassword = DEFAULT_PASS; return false; }
  return true;
}

void guardarCredencialesWiFi(const String &ssid, const String &password) {
  File f = LittleFS.open("/wifi.txt", "w");
  if (f) { f.println(ssid); f.println(password); f.close(); addLog("info", "Credenciales guardadas"); }
}

// ==================== OTA ====================
void guardarVersion(String version) {
  File f = LittleFS.open("/version.txt", "w");
  if (f) { f.print(version); f.close(); }
}

String leerVersion() {
  File f = LittleFS.open("/version.txt", "r");
  if (!f) return "AUTO_VERSION";
  String v = f.readString(); v.trim(); f.close(); return v;
}

// Función actualizarHTML CORREGIDA (sigue redirecciones)
bool actualizarHTML() {
  if (WiFi.status() != WL_CONNECTED) {
    addLog("error", "HTML: sin WiFi");
    return false;
  }
  addLog("info", "Descargando HTML desde GitHub...");
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // sigue redirecciones
  http.setTimeout(15000);
  http.begin(espClient, urlHTML);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK || httpCode == 302 || httpCode == 301) {
    String nuevoHTML = http.getString();
    http.end();
    if (nuevoHTML.length() < 100) {
      addLog("error", "HTML descargado muy pequeño: " + String(nuevoHTML.length()) + " bytes");
      return false;
    }
    File f = LittleFS.open("/index.html", "w");
    if (f) {
      f.print(nuevoHTML);
      f.close();
      addLog("info", "HTML actualizado (" + String(nuevoHTML.length()) + " bytes)");
      return true;
    } else {
      addLog("error", "No se pudo abrir /index.html para escritura");
      return false;
    }
  } else {
    addLog("error", "Error HTTP al descargar HTML: " + String(httpCode));
    http.end();
    return false;
  }
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
  HTTPClient http; http.setTimeout(10000); http.begin(espClient, urlVersion);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String versionGitHub = http.getString(); versionGitHub.trim();
    String currentVersion = leerVersion();
    if (isNewerVersion(versionGitHub, currentVersion) && versionGitHub.length() > 0) {
      addLog("info", "Nueva versión detectada. Iniciando OTA...");
      httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      t_httpUpdate_return ret = httpUpdate.update(espClient, urlFirmware);
      if (ret == HTTP_UPDATE_OK) {
        guardarVersion(versionGitHub);
        actualizarHTML();
        ESP.restart();
      }
    }
  }
  http.end();
}

// ==================== MQTT ADMIN ====================
void respondAdminCommand(int id, bool success, JsonDocument &data, const String &errorMsg = "") {
  if (!client.connected()) return;
  DynamicJsonDocument resp(1024);
  resp["id"] = id; resp["success"] = success;
  if (success) resp["data"] = data; else resp["error"] = errorMsg;
  String out; serializeJson(resp, out);
  client.publish("fxi/admin/resp", out.c_str());
}

void processAdminCommand(int id, const String &action, DynamicJsonDocument *extraData = nullptr) {
  if (action == "get_stats") {
    DynamicJsonDocument data(512);
    data["firmware"] = FIRMWARE_VERSION;
    data["uptime"] = millis() / 1000;
    size_t heapFree = ESP.getFreeHeap();
    data["heap_percent"] = (ESP.getHeapSize() > 0) ? (heapFree * 100) / ESP.getHeapSize() : 0;
    data["sketch_percent"] = (ESP.getSketchSize() * 100) / 1992294;
    data["ip_ap"] = WiFi.softAPIP().toString();
    data["ip_sta"] = WiFi.localIP().toString();
    data["mqtt_status"] = client.connected() ? "conectado" : "desconectado";
    data["mdns"] = "fluxaignis.local";
    respondAdminCommand(id, true, data);
  }
  else if (action == "get_logs") {
    int logsToSend = min(logCount, 10);
    DynamicJsonDocument data(4096);
    JsonArray logs = data.createNestedArray("logs");
    int start = (logIndex - logsToSend + MAX_LOGS) % MAX_LOGS;
    for (int i = 0; i < logsToSend; i++) {
      int idx = (start + i) % MAX_LOGS;
      JsonObject entry = logs.createNestedObject();
      entry["timestamp"] = logBuffer[idx].timestamp;
      entry["level"] = logBuffer[idx].level;
      entry["message"] = logBuffer[idx].message;
    }
    respondAdminCommand(id, true, data);
  }
  else if (action == "set_thresholds") {
    if (extraData) {
      if ((*extraData).containsKey("dangerTemp")) dangerTempThreshold = (*extraData)["dangerTemp"];
      if ((*extraData).containsKey("fireMqtt")) umbralDesactivacionFuego = (*extraData)["fireMqtt"];
      if ((*extraData).containsKey("gasLimit")) umbralGas = (*extraData)["gasLimit"];
      saveThresholds();
      DynamicJsonDocument empty(64);
      respondAdminCommand(id, true, empty);
      addLog("info", "Umbrales actualizados via MQTT");
    } else {
      DynamicJsonDocument empty(64);
      respondAdminCommand(id, false, empty, "Falta JSON con dangerTemp/fireMqtt/gasLimit");
    }
  }
  else if (action == "restart") {
    DynamicJsonDocument empty(64); respondAdminCommand(id, true, empty); delay(100); ESP.restart();
  }
  else if (action == "update_fw") {
    DynamicJsonDocument empty(64); respondAdminCommand(id, true, empty); chequearActualizacionGitHub();
  }
  else if (action == "update_html") {
    DynamicJsonDocument empty(64); respondAdminCommand(id, true, empty); actualizarHTML();
  }
  else if (action == "toggle_sim") {
    simularFuego = !simularFuego;
    DynamicJsonDocument data(32); data["simulating"] = simularFuego;
    respondAdminCommand(id, true, data);
  }
  else if (action == "clear_logs") {
    logIndex = 0; logCount = 0;
    DynamicJsonDocument empty(64); respondAdminCommand(id, true, empty);
  }
  else if (action == "ping") {
    DynamicJsonDocument data(32); data["pong"] = true; respondAdminCommand(id, true, data);
  }
  else {
    DynamicJsonDocument empty(64); respondAdminCommand(id, false, empty, "Unknown action");
  }
}

// ==================== CALLBACK MQTT ====================
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  String topicStr = String(topic);
  if (topicStr == "fxi/comandos" && msg == "TOGGLE") {
    if (estadoActual == REPOSO) iniciarRutina(0, "Comando Manual MQTT");
    else detenerRutina();
  }
  else if (topicStr == "fxi/simular") {
    simularFuego = (msg == "FUEGO_ON");
  }
  else if (topicStr == "fxi/admin/cmd") {
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, msg);
    if (!error) {
      String action = doc["action"] | "";
      if (action == "set_thresholds") {
        DynamicJsonDocument data(64);
        data["dangerTemp"] = doc["dangerTemp"] | dangerTempThreshold;
        data["fireMqtt"] = doc["fireMqtt"] | umbralDesactivacionFuego;
        data["gasLimit"] = doc["gasLimit"] | umbralGas;
        processAdminCommand(doc["id"], action, &data);
      } else {
        processAdminCommand(doc["id"], action);
      }
    }
  }
}

// ==================== MANEJADORES WEB ====================
void handleRoot() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) { server.send(500, "text/plain", "Error"); return; }
  server.streamFile(file, "text/html"); file.close();
}

void handleEstado() {
  float tempAmbiente = isnan(tempGuardada) ? NAN : tempGuardada - OFFSET_TEMP;
  String json = "{";
  json += "\"temp\":" + (isnan(tempAmbiente) ? "null" : String(tempAmbiente)) + ",";
  json += "\"hum\":" + (isnan(humGuardada) ? "null" : String(humGuardada)) + ",";
  json += "\"gas\":" + String(gasValue) + ",";
  json += "\"llama_izq\":" + String(llamaIzq) + ",";
  json += "\"llama_der\":" + String(llamaDer) + ",";
  json += "\"rssi\":" + String(rssiGuardado) + "}";
  server.send(200, "application/json", json);
}

void handleToggle() {
  if (estadoActual == REPOSO) iniciarRutina(0, "Comando Web");
  else detenerRutina();
  server.send(200, "text/plain", (estadoActual != REPOSO) ? "ON" : "OFF");
}

void handleSetThresholds() {
  if (server.hasArg("dangerTemp")) dangerTempThreshold = server.arg("dangerTemp").toFloat();
  if (server.hasArg("fireMqtt")) umbralDesactivacionFuego = server.arg("fireMqtt").toFloat();
  if (server.hasArg("gasLimit")) umbralGas = server.arg("gasLimit").toInt();
  saveThresholds();
  addLog("info", "Umbrales actualizados vía HTTP");
  server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  server.send(200, "text/html", "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='0;url=http://192.168.1.1/'></head><body></body></html>");
}

void handleInfo() {
  DynamicJsonDocument doc(512);
  doc["firmware"] = FIRMWARE_VERSION;
  doc["uptime"] = millis() / 1000;
  doc["heap_percent"] = (ESP.getHeapSize() > 0) ? (ESP.getFreeHeap() * 100) / ESP.getHeapSize() : 0;
  doc["sketch_percent"] = (ESP.getSketchSize() * 100) / 1992294;
  doc["ip_ap"] = WiFi.softAPIP().toString();
  doc["ip_sta"] = WiFi.localIP().toString();
  doc["mqtt_status"] = client.connected() ? "conectado" : "desconectado";
  doc["mdns"] = "fluxaignis.local";
  String json; serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleLogs() {
  DynamicJsonDocument doc(4096);
  JsonArray logs = doc.createNestedArray("logs");
  int start = (logIndex - logCount + MAX_LOGS) % MAX_LOGS;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % MAX_LOGS;
    JsonObject entry = logs.createNestedObject();
    entry["timestamp"] = logBuffer[idx].timestamp;
    entry["level"] = logBuffer[idx].level;
    entry["message"] = logBuffer[idx].message;
  }
  String json; serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleCmd() {
  if (!server.hasArg("action")) { server.send(400, "text/plain", "Missing action"); return; }
  String action = server.arg("action");
  if (action == "restart") { server.send(200, "text/plain", "OK"); delay(100); ESP.restart(); }
  else if (action == "check_ota") { ultimoChequeoOTA = 0; server.send(200, "text/plain", "OK"); }
  else if (action == "toggle_sim") { simularFuego = !simularFuego; server.send(200, "text/plain", "OK"); }
  else if (action == "clear_logs") { logIndex = 0; logCount = 0; server.send(200, "text/plain", "OK"); }
  else if (action == "update_fw") { server.send(200, "text/plain", "OK"); chequearActualizacionGitHub(); }
  else if (action == "update_html") { server.send(200, "text/plain", "OK"); actualizarHTML(); }
  else { server.send(400, "text/plain", "Unknown action"); }
}

void handleWiFiConfig() {
  String token = server.arg("token");
  if (token != ADMIN_TOKEN) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (server.method() == HTTP_GET) {
    String html = "<!DOCTYPE html><html>... (mantén el HTML de configuración WiFi anterior)...</html>";
    server.send(200, "text/html", html);
  } else if (server.method() == HTTP_POST) {
    String newSSID = server.arg("ssid"), newPass = server.arg("pass");
    if (newSSID.length() > 0) {
      guardarCredencialesWiFi(newSSID, newPass);
      server.send(200, "text/html", "<html><body><h2>Guardado. Reiniciando...</h2></body></html>");
      delay(500); ESP.restart();
    } else server.send(400, "text/plain", "SSID requerido");
  }
}

// ==================== CONEXIÓN WiFi SIN BLOQUEOS ====================
bool conectarWiFiSTA() {
  leerCredencialesWiFi();
  if (wifiSSID.length() == 0) return false;
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - inicio) < 20000) {
    delay(20);
  }
  return WiFi.status() == WL_CONNECTED;
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

  if (!LittleFS.begin(true)) {
    addLog("error", "LittleFS no disponible"); return;
  }
  FIRMWARE_VERSION = leerVersion();
  loadThresholds();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(ap_ssid, ap_password);
  dnsServer.start(53, "*", apIP);
  MDNS.begin("fluxaignis");
  MDNS.addService("http", "tcp", 80);

  if (conectarWiFiSTA()) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    espClient.setInsecure();
    client.setServer(mqtt_server, 8883);
    client.setCallback(callback);
    chequearActualizacionGitHub();
  }

  ArduinoOTA.setHostname("fluxaignis_ota");
  ArduinoOTA.setPassword("12345678");
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.on("/estado", handleEstado);
  server.on("/toggleServo", handleToggle);
  server.on("/setThresholds", handleSetThresholds);
  server.on("/generate_204", []() { server.send(204); });
  server.on("/info", handleInfo);
  server.on("/logs", handleLogs);
  server.on("/cmd", handleCmd);
  server.on("/wifi", HTTP_GET, handleWiFiConfig);
  server.on("/wifi", HTTP_POST, handleWiFiConfig);
  server.onNotFound(handleNotFound);
  server.begin();

  for (int i = 0; i < 2 && (isnan(tempGuardada) || isnan(humGuardada)); i++) {
    tempGuardada = dht.readTemperature();
    humGuardada = dht.readHumidity();
    delay(100);
  }
}

// ==================== LOOP PRINCIPAL (24/7) ====================
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim(); cmd.toLowerCase();
    if (cmd == "update_fw" || cmd == "fw") chequearActualizacionGitHub();
    else if (cmd == "update_html" || cmd == "html") actualizarHTML();
    else if (cmd == "restart") ESP.restart();
  }

  if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    if (client.connect("ESP32_FXI", "Admin", "FluxaIgnis2026")) {
      client.subscribe("fxi/comandos");
      client.subscribe("fxi/simular");
      client.subscribe("fxi/admin/cmd");
      addLog("info", "MQTT conectado");
    }
  }
  client.loop();

  unsigned long ahora = millis();

  if (ahora - ultimoTiempoDHT >= 1000) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && t > 0 && t < 80) tempGuardada = t;
    if (!isnan(h) && h >= 0 && h <= 100) humGuardada = h;
    ultimoTiempoDHT = ahora;
  }

  llamaIzq = analogRead(PIN_LLAMA_IZQ);
  llamaDer = analogRead(PIN_LLAMA_DER);
  gasValue = analogRead(PIN_MQ2);
  rssiGuardado = WiFi.RSSI();

  if (client.connected()) {
    client.publish("fxi/flama1", (llamaIzq < UMBRAL_FUEGO) ? "ON" : "OFF");
    client.publish("fxi/flama2", (llamaDer < UMBRAL_FUEGO) ? "ON" : "OFF");
    client.publish("fxi/bomba", digitalRead(PIN_BOMBA) ? "ON" : "OFF");
    int angulo = (estadoActual == APUNTANDO) ? ((ladoEmergencia == 1) ? SERVO_IZQ : (ladoEmergencia == 2) ? SERVO_DER : SERVO_CENTRO) : SERVO_CENTRO;
    client.publish("fxi/angulo", String(angulo).c_str());
  }

  // Detección 24/7
  if (estadoActual == REPOSO && !emergenciaActiva) {
    bool fuegoIzq = (llamaIzq < UMBRAL_FUEGO);
    bool fuegoDer = (llamaDer < UMBRAL_FUEGO);
    bool hayGas = (gasValue > umbralGas);
    bool calorCritico = (!isnan(tempGuardada) && tempGuardada >= dangerTempThreshold);

    if (simularFuego || calorCritico || hayGas || fuegoIzq || fuegoDer) {
      String motivo = simularFuego ? "SIMULACIÓN" : calorCritico ? "TEMPERATURA CRÍTICA" : hayGas ? "GAS COMBUSTIBLE" : (fuegoIzq && fuegoDer) ? "FUEGO AMBOS" : fuegoIzq ? "FUEGO IZQUIERDO" : "FUEGO DERECHO";
      iniciarRutina(simularFuego ? 0 : fuegoIzq ? 1 : fuegoDer ? 2 : 0, motivo);
      if (!emergenciaEnviada) {
        enviarNotificacionMQTT(motivo, tempGuardada - OFFSET_TEMP, gasValue);
        emergenciaEnviada = true;
      }
    }
  } else if (!emergenciaActiva && !isnan(tempGuardada) && tempGuardada < dangerTempThreshold - 2.0) {
    emergenciaEnviada = false;
  }

  // LED y buzzer
  if (estadoActual != REPOSO) {
    setColor(255, 0, 0); updateBuzzer();
  } else {
    noTone(PIN_BUZZER); buzzerState = false;
    if (!isnan(tempGuardada) && tempGuardada >= 35.0) setColor(255, 0, 0);
    else if (WiFi.status() == WL_CONNECTED) setColor(client.connected() ? 0x00FF00 : 0xFFFF00);
    else setColor(0x00FFFF);
  }

  switch (estadoActual) {
    case ESPERANDO_AGUA:
      if (ahora - cronometroRutina >= WATER_DELAY_MS) {
        servoHorizontal.write((ladoEmergencia == 1) ? SERVO_IZQ : (ladoEmergencia == 2) ? SERVO_DER : SERVO_CENTRO);
        cronometroRutina = ahora; estadoActual = APUNTANDO;
      }
      break;
    case APUNTANDO:
      if (ahora - cronometroRutina >= TIEMPO_APUNTAR) detenerRutina();
      break;
  }

  if (WiFi.status() == WL_CONNECTED && client.connected()) {
    if (ahora - cronometroDatos >= 1000) {
      cronometroDatos = ahora;
      float tempEnviar = isnan(tempGuardada) ? 0 : tempGuardada - OFFSET_TEMP;
      String payload = "{\"temp\":" + String(tempEnviar) + ",\"hum\":" + String(isnan(humGuardada) ? 0 : humGuardada) +
                       ",\"gas\":" + String(gasValue) + ",\"llama_izq\":" + String(llamaIzq) + ",\"llama_der\":" + String(llamaDer) + "}";
      client.publish("fxi/datos", payload.c_str());
    }
    if (ahora - cronometroRSSI >= 2000) {
      cronometroRSSI = ahora;
      client.publish("fxi/rssi", ("{\"rssi\":" + String(rssiGuardado) + "}").c_str());
    }
  }

  if (ahora - ultimoChequeoOTA >= INTERVALO_OTA) {
    ultimoChequeoOTA = ahora; chequearActualizacionGitHub();
  }

  // Actualización diaria del HTML (si hay internet)
  static unsigned long ultimaActualizacionHTML = 0;
  if (ahora - ultimaActualizacionHTML >= 86400000UL && WiFi.status() == WL_CONNECTED) {
    actualizarHTML();
    ultimaActualizacionHTML = ahora;
  }
}
