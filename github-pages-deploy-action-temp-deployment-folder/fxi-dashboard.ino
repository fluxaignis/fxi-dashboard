#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>
#include <SPIFFS.h>
#include <FS.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>

// ==================== COMPENSACIÓN DE TEMPERATURA ====================
const float OFFSET_TEMP = 5.0;

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

// ==================== GITHUB OTA ====================
String FIRMWARE_VERSION = "1.0.80";
const char* urlVersion = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/version.txt";
const char* urlFirmware = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/firmware.bin";
const char* urlHTML = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/main/index.html";
unsigned long ultimoChequeoOTA = 0;
const unsigned long INTERVALO_OTA = 60001;

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

// ==================== FUNCIONES (forward declarations necesarias) ====================
void addLog(String level, String message);
void setColor(int r, int g, int b);
void iniciarRutina(int lado, String motivo);
void detenerRutina();
void handleRoot();
void handleEstado();
void handleToggle();
void handleSetThresholds();
void handleNotFound();
void handleInfo();
void handleLogs();
void handleCmd();
void handleWiFiConfig();
bool conectarWiFiSTA();

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
float dangerTempThreshold = 40.0;
float umbralDesactivacionFuego = 35.0;
int umbralGas = 250;
const int UMBRAL_FUEGO = 500;
const char* THRESHOLD_FILE = "/umbrales.txt";

bool loadThresholds() {
  File f = SPIFFS.open(THRESHOLD_FILE, "r");
  if (!f) {
    dangerTempThreshold = 40.0; umbralDesactivacionFuego = 35.0; umbralGas = 250;
    return false;
  }
  dangerTempThreshold = f.readStringUntil('\n').toFloat();
  umbralDesactivacionFuego = f.readStringUntil('\n').toFloat();
  umbralGas = f.readStringUntil('\n').toInt();
  f.close();
  addLog("info", "Umbrales cargados");
  return true;
}

void saveThresholds() {
  File f = SPIFFS.open(THRESHOLD_FILE, "w");
  if (f) {
    f.printf("%.1f\n%.1f\n%d\n", dangerTempThreshold, umbralDesactivacionFuego, umbralGas);
    f.close();
  }
}

// ==================== VARIABLES ====================
bool emergenciaEnviada = false;
float tempGuardada = NAN, humGuardada = NAN;
int rssiGuardado = -99, gasValue = 0;
unsigned long cronometroDatos = 0, cronometroRSSI = 0, ultimoTiempoDHT = 0;
bool simularFuego = false;
int llamaIzq = 4095, llamaDer = 4095;

// ==================== BUZZER ====================
const int BEEPS_PER_CYCLE = 3;
const unsigned long BEEP_ON_MS = 150, BEEP_OFF_MS = 150, PAUSE_MS = 500;
int beepCounter = 0;
unsigned long lastBuzzerTime = 0;
bool buzzerState = false, inPause = false;

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
    if (!buzzerState && ahora - lastBuzzerTime >= PAUSE_MS) { inPause = false; lastBuzzerTime = ahora; }
  }
}

// ==================== CREDENCIALES DINÁMICAS ====================
const char* DEFAULT_SSID = "NauticaNet";
const char* DEFAULT_PASS = "Princess-2015";
String wifiSSID = "", wifiPassword = "";
const char* ADMIN_TOKEN = "config2026";

// ==================== AP ====================
const char* ap_ssid = "FluxaIgnis TECH";
const char* ap_password = "";
IPAddress apIP(192, 168, 1, 1), apGateway(192, 168, 1, 1), apSubnet(255, 255, 255, 0);

// ==================== MQTT ====================
const char* mqtt_server = "df734b8fbeed43978f29869442892dcf.s1.eu.hivemq.cloud";
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -14400;
const int daylightOffset_sec = 0;

// ==================== AUXILIARES ====================
void setColor(int r, int g, int b) {
  analogWrite(PIN_ROJO, r); analogWrite(PIN_VERDE, g); analogWrite(PIN_AZUL, b);
}

// Definición de enviarNotificacionMQTT (sin forward declaration, se define aquí antes de usarse)
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
  File f = SPIFFS.open("/wifi.txt", "r");
  if (!f) { wifiSSID = DEFAULT_SSID; wifiPassword = DEFAULT_PASS; return false; }
  wifiSSID = f.readStringUntil('\n'); wifiSSID.trim();
  wifiPassword = f.readStringUntil('\n'); wifiPassword.trim();
  f.close();
  if (wifiSSID.length() == 0) { wifiSSID = DEFAULT_SSID; wifiPassword = DEFAULT_PASS; return false; }
  return true;
}

void guardarCredencialesWiFi(const String &ssid, const String &password) {
  File f = SPIFFS.open("/wifi.txt", "w");
  if (f) { f.println(ssid); f.println(password); f.close(); addLog("info", "Credenciales guardadas"); }
}

// ==================== OTA ====================
void guardarVersion(String version) { File f = SPIFFS.open("/version.txt", "w"); if (f) { f.print(version); f.close(); } }
String leerVersion() { File f = SPIFFS.open("/version.txt", "r"); if (!f) return "1.0.80"; String v = f.readString(); v.trim(); f.close(); return v; }

bool actualizarHTML() {
  if (WiFi.status() != WL_CONNECTED) { addLog("error", "HTML: sin WiFi"); return false; }
  HTTPClient http; http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); http.setTimeout(10000); http.begin(espClient, urlHTML);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK || httpCode == 302 || httpCode == 301) {
    String nuevoHTML = http.getString(); http.end();
    if (nuevoHTML.length() < 100) { addLog("error", "HTML muy pequeño"); return false; }
    File f = SPIFFS.open("/index.html", "w");
    if (f) { f.print(nuevoHTML); f.close(); addLog("info", "HTML actualizado"); return true; }
    else { addLog("error", "No se pudo escribir /index.html"); return false; }
  } else { addLog("error", "Error HTTP " + String(httpCode)); http.end(); return false; }
}

bool isNewerVersion(String remote, String current) {
  remote.replace("v", ""); current.replace("v", "");
  int rMaj, rMin, rPat, cMaj, cMin, cPat;
  if (sscanf(remote.c_str(), "%d.%d.%d", &rMaj, &rMin, &rPat) != 3) return false;
  if (sscanf(current.c_str(), "%d.%d.%d", &cMaj, &cMin, &cPat) != 3) return true;
  return (rMaj > cMaj) || (rMaj == cMaj && rMin > cMin) || (rMaj == cMaj && rMin == cMin && rPat > cPat);
}

void chequearActualizacionGitHub() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http; http.setTimeout(10000); http.begin(espClient, urlVersion);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String versionGitHub = http.getString(); versionGitHub.trim();
    if (isNewerVersion(versionGitHub, leerVersion())) {
      addLog("info", "Nueva versión, iniciando OTA...");
      httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      t_httpUpdate_return ret = httpUpdate.update(espClient, urlFirmware);
      if (ret == HTTP_UPDATE_OK) { guardarVersion(versionGitHub); actualizarHTML(); ESP.restart(); }
      else addLog("error", "OTA falló");
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
    data["firmware"] = FIRMWARE_VERSION; data["uptime"] = millis() / 1000;
    data["heap_percent"] = ESP.getFreeHeap() * 100 / ESP.getHeapSize();
    data["sketch_percent"] = ESP.getSketchSize() * 100 / 1992294;
    data["ip_ap"] = WiFi.softAPIP().toString(); data["ip_sta"] = WiFi.localIP().toString();
    data["mqtt_status"] = client.connected() ? "conectado" : "desconectado";
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
  else if (action == "set_thresholds" && extraData) {
    if ((*extraData).containsKey("dangerTemp")) dangerTempThreshold = (*extraData)["dangerTemp"];
    if ((*extraData).containsKey("fireMqtt")) umbralDesactivacionFuego = (*extraData)["fireMqtt"];
    if ((*extraData).containsKey("gasLimit")) umbralGas = (*extraData)["gasLimit"];
    saveThresholds(); DynamicJsonDocument empty(64); respondAdminCommand(id, true, empty);
  }
  else if (action == "restart") { DynamicJsonDocument empty(64); respondAdminCommand(id, true, empty); delay(100); ESP.restart(); }
  else if (action == "update_fw") { DynamicJsonDocument empty(64); respondAdminCommand(id, true, empty); chequearActualizacionGitHub(); }
  else if (action == "update_html") { DynamicJsonDocument empty(64); respondAdminCommand(id, true, empty); actualizarHTML(); }
  else if (action == "toggle_sim") { simularFuego = !simularFuego; DynamicJsonDocument data(32); data["simulating"] = simularFuego; respondAdminCommand(id, true, data); }
  else if (action == "clear_logs") { logIndex = 0; logCount = 0; DynamicJsonDocument empty(64); respondAdminCommand(id, true, empty); }
  else if (action == "ping") { DynamicJsonDocument data(32); data["pong"] = true; respondAdminCommand(id, true, data); }
  else { DynamicJsonDocument empty(64); respondAdminCommand(id, false, empty, "Unknown action"); }
}

// ==================== CALLBACK MQTT ====================
void callback(char* topic, byte* payload, unsigned int length) {
  String msg; for (int i=0; i<length; i++) msg += (char)payload[i];
  if (String(topic) == "fxi/comandos" && msg == "TOGGLE") {
    if (estadoActual == REPOSO) iniciarRutina(0, "Comando MQTT"); else detenerRutina();
  }
  else if (String(topic) == "fxi/simular") simularFuego = (msg == "FUEGO_ON");
  else if (String(topic) == "fxi/admin/cmd") {
    DynamicJsonDocument doc(256); DeserializationError err = deserializeJson(doc, msg);
    if (!err) {
      String action = doc["action"];
      if (action == "set_thresholds") {
        DynamicJsonDocument data(64);
        data["dangerTemp"] = doc["dangerTemp"] | dangerTempThreshold;
        data["fireMqtt"] = doc["fireMqtt"] | umbralDesactivacionFuego;
        data["gasLimit"] = doc["gasLimit"] | umbralGas;
        processAdminCommand(doc["id"], action, &data);
      } else processAdminCommand(doc["id"], action);
    }
  }
}

// ==================== MANEJADORES WEB ====================
void handleRoot() {
  File file = SPIFFS.open("/index.html", "r");
  if (!file) {
    server.send(200, "text/html", "<html><body><h2>Panel no encontrado</h2><p>Sube el archivo index.html con ESP32 Sketch Data Upload.</p></body></html>");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
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
  if (estadoActual == REPOSO) iniciarRutina(0, "Comando Web"); else detenerRutina();
  server.send(200, "text/plain", (estadoActual != REPOSO) ? "ON" : "OFF");
}

void handleSetThresholds() {
  if (server.hasArg("dangerTemp")) dangerTempThreshold = server.arg("dangerTemp").toFloat();
  if (server.hasArg("fireMqtt")) umbralDesactivacionFuego = server.arg("fireMqtt").toFloat();
  if (server.hasArg("gasLimit")) umbralGas = server.arg("gasLimit").toInt();
  saveThresholds(); addLog("info", "Umbrales actualizados"); server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='0;url=http://192.168.1.1/'></head><body></body></html>");
}

void handleInfo() {
  DynamicJsonDocument doc(512);
  doc["firmware"] = FIRMWARE_VERSION; doc["uptime"] = millis() / 1000;
  doc["heap_percent"] = ESP.getFreeHeap() * 100 / ESP.getHeapSize();
  doc["sketch_percent"] = ESP.getSketchSize() * 100 / 1992294;
  doc["ip_ap"] = WiFi.softAPIP().toString(); doc["ip_sta"] = WiFi.localIP().toString();
  doc["mqtt_status"] = client.connected() ? "conectado" : "desconectado";
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
  if (token != ADMIN_TOKEN) {
    if (!server.hasArg("token")) {
      String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Acceso restringido</title>";
      html += "<style>body{background:#0B3954;color:white;font-family:sans-serif;padding:20px;}";
      html += "input{width:100%;padding:10px;margin:10px 0;border-radius:20px;border:none;}";
      html += "button{background:#7AF0D4;border:none;padding:10px 20px;border-radius:20px;}</style>";
      html += "</head><body><h2>Introduce el token de administración</h2>";
      html += "<form method='GET'>";
      html += "Token: <input type='text' name='token'><br>";
      html += "<button type='submit'>Acceder</button>";
      html += "</form></body></html>";
      server.send(200, "text/html", html);
    } else {
      server.send(401, "text/plain", "Unauthorized: token incorrecto");
    }
    return;
  }
  if (server.method() == HTTP_GET) {
    String html = R"rawliteral(<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Configurar WiFi</title>
<style>body{background:#0B3954;color:white;font-family:sans-serif;padding:20px} input{width:100%;padding:12px;margin:10px 0;border-radius:20px;border:none} button{background:#7AF0D4;padding:12px 24px;border-radius:20px;font-weight:bold}</style>
</head><body><h2>Configurar WiFi</h2><form action='/wifi' method='POST'>
<input type='hidden' name='token' value='config2026'>
SSID: <input name='ssid' value=')rawliteral" + wifiSSID + R"rawliteral(' required><br>
Contraseña: <input type='password' name='pass' required><br>
<button type='submit'>Guardar y Reiniciar</button></form></body></html>)rawliteral";
    server.send(200, "text/html", html);
  } else if (server.method() == HTTP_POST) {
    String ssid = server.arg("ssid"), pass = server.arg("pass");
    if (ssid.length() > 0) { guardarCredencialesWiFi(ssid, pass); server.send(200, "text/html", "<html><body><h2>Guardado. Reiniciando...</h2></body></html>"); delay(500); ESP.restart(); }
    else server.send(400, "text/plain", "SSID requerido");
  }
}

// ==================== CONEXIÓN WiFi ====================
bool conectarWiFiSTA() {
  leerCredencialesWiFi();
  if (wifiSSID.length() == 0) return false;
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) { delay(500); intentos++; }
  return (WiFi.status() == WL_CONNECTED);
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.println();
  addLog("info", "Sistema iniciando...");

  dht.begin(); pinMode(PIN_BOMBA, OUTPUT); digitalWrite(PIN_BOMBA, LOW);
  pinMode(PIN_ROJO, OUTPUT); pinMode(PIN_VERDE, OUTPUT); pinMode(PIN_AZUL, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT); noTone(PIN_BUZZER);
  pinMode(PIN_MQ2, INPUT); pinMode(PIN_LLAMA_IZQ, INPUT); pinMode(PIN_LLAMA_DER, INPUT);

  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1); ESP32PWM::allocateTimer(2); ESP32PWM::allocateTimer(3);
  servoHorizontal.setPeriodHertz(50); servoHorizontal.attach(PIN_SERVO_H, 500, 2400); servoHorizontal.write(SERVO_CENTRO);
  servoVertical.setPeriodHertz(50); servoVertical.attach(PIN_SERVO_V, 500, 2400); servoVertical.write(20);

  if (!SPIFFS.begin()) {
    addLog("warn", "SPIFFS no montado, formateando...");
    if (!SPIFFS.format()) {
      addLog("error", "Formateo de SPIFFS fallido. Sistema detenido.");
      return;
    }
    if (!SPIFFS.begin()) {
      addLog("error", "No se pudo montar SPIFFS tras formateo.");
      return;
    }
    addLog("info", "SPIFFS formateado y montado correctamente.");
  } else {
    addLog("info", "SPIFFS montado OK");
  }

  FIRMWARE_VERSION = leerVersion();
  loadThresholds();

  Serial.println("--- Archivos en SPIFFS ---");
  File root = SPIFFS.open("/"); File f = root.openNextFile();
  while (f) { Serial.printf("  %s (%u bytes)\n", f.name(), f.size()); f = root.openNextFile(); }
  Serial.println("--------------------------");

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(ap_ssid, ap_password);
  dnsServer.start(53, "*", apIP);
  MDNS.begin("fluxaignis"); MDNS.addService("http", "tcp", 80);

  addLog("info", "Punto de acceso: " + String(ap_ssid) + " IP: " + WiFi.softAPIP().toString());

  if (conectarWiFiSTA()) {
    addLog("info", "Conectado a WiFi externa: " + WiFi.localIP().toString());
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    espClient.setInsecure(); client.setServer(mqtt_server, 8883); client.setCallback(callback);
    chequearActualizacionGitHub();
  } else {
    addLog("warn", "No se pudo conectar a WiFi externa. Usa http://192.168.1.1/wifi?token=config2026");
  }

  ArduinoOTA.setHostname("fluxaignis_ota"); ArduinoOTA.setPassword("12345678"); ArduinoOTA.begin();

  server.on("/", handleRoot); server.on("/test", [](){server.send(200,"text/plain","OK");});
  server.on("/estado", handleEstado); server.on("/toggleServo", handleToggle);
  server.on("/setThresholds", handleSetThresholds);
  server.on("/info", handleInfo); server.on("/logs", handleLogs);
  server.on("/cmd", handleCmd);
  server.on("/wifi", HTTP_GET, handleWiFiConfig); server.on("/wifi", HTTP_POST, handleWiFiConfig);
  server.onNotFound(handleNotFound);
  server.begin();
  addLog("info", "Servidor web iniciado en http://192.168.1.1");
}

// ==================== LOOP (24/7) ====================
void loop() {
  dnsServer.processNextRequest(); server.handleClient(); ArduinoOTA.handle();

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim(); cmd.toLowerCase();
    if (cmd == "update_fw") chequearActualizacionGitHub();
    else if (cmd == "update_html") actualizarHTML();
    else if (cmd == "restart") ESP.restart();
  }

  if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    if (client.connect("ESP32_FXI", "Admin", "FluxaIgnis2026")) {
      client.subscribe("fxi/comandos"); client.subscribe("fxi/simular"); client.subscribe("fxi/admin/cmd");
      addLog("info", "MQTT conectado");
    }
  }
  client.loop();

  unsigned long ahora = millis();
  if (ahora - ultimoTiempoDHT >= 1000) {
    float t = dht.readTemperature(); float h = dht.readHumidity();
    if (!isnan(t) && t > 0 && t < 80) tempGuardada = t;
    if (!isnan(h) && h >= 0 && h <= 100) humGuardada = h;
    ultimoTiempoDHT = ahora;
  }

  llamaIzq = analogRead(PIN_LLAMA_IZQ); llamaDer = analogRead(PIN_LLAMA_DER);
  gasValue = analogRead(PIN_MQ2); rssiGuardado = WiFi.RSSI();

  if (client.connected()) {
    client.publish("fxi/flama1", (llamaIzq < UMBRAL_FUEGO) ? "ON" : "OFF");
    client.publish("fxi/flama2", (llamaDer < UMBRAL_FUEGO) ? "ON" : "OFF");
    client.publish("fxi/bomba", digitalRead(PIN_BOMBA) ? "ON" : "OFF");
    int angulo = (estadoActual == APUNTANDO) ? ((ladoEmergencia==1)?SERVO_IZQ:(ladoEmergencia==2)?SERVO_DER:SERVO_CENTRO) : SERVO_CENTRO;
    client.publish("fxi/angulo", String(angulo).c_str());
  }

  // Detección 24/7
  if (estadoActual == REPOSO && !emergenciaActiva) {
    bool fuego = (llamaIzq < UMBRAL_FUEGO) || (llamaDer < UMBRAL_FUEGO);
    bool gas = (gasValue > umbralGas);
    bool calor = (!isnan(tempGuardada) && tempGuardada >= dangerTempThreshold);
    if (simularFuego || calor || gas || fuego) {
      String motivo = simularFuego ? "SIMULACIÓN" : calor ? "TEMPERATURA CRÍTICA" : gas ? "GAS COMBUSTIBLE" : "FUEGO DETECTADO";
      iniciarRutina(0, motivo);
      if (!emergenciaEnviada) { enviarNotificacionMQTT(motivo, tempGuardada - OFFSET_TEMP, gasValue); emergenciaEnviada = true; }
    }
  } else if (!emergenciaActiva && tempGuardada < dangerTempThreshold - 2.0) emergenciaEnviada = false;

  // LED y buzzer
  if (estadoActual != REPOSO) { setColor(255,0,0); updateBuzzer(); }
  else {
    noTone(PIN_BUZZER);
    if (!isnan(tempGuardada) && tempGuardada >= 35.0) setColor(255,0,0);
    else if (WiFi.status() == WL_CONNECTED) {
      if (client.connected()) setColor(0,255,0); else setColor(255,255,0);
    } else setColor(0,255,255);
  }

  switch (estadoActual) {
    case ESPERANDO_AGUA:
      if (ahora - cronometroRutina >= WATER_DELAY_MS) {
        servoHorizontal.write((ladoEmergencia==1)?SERVO_IZQ:(ladoEmergencia==2)?SERVO_DER:SERVO_CENTRO);
        cronometroRutina = ahora; estadoActual = APUNTANDO;
      } break;
    case APUNTANDO:
      if (ahora - cronometroRutina >= TIEMPO_APUNTAR) detenerRutina(); break;
  }

  if (WiFi.status() == WL_CONNECTED && client.connected()) {
    if (ahora - cronometroDatos >= 1000) {
      cronometroDatos = ahora;
      float tempEnviar = isnan(tempGuardada) ? 0 : tempGuardada - OFFSET_TEMP;
      String payload = "{\"temp\":" + String(tempEnviar) + ",\"hum\":" + String(isnan(humGuardada)?0:humGuardada) +
                       ",\"gas\":" + gasValue + ",\"llama_izq\":" + llamaIzq + ",\"llama_der\":" + llamaDer + "}";
      client.publish("fxi/datos", payload.c_str());
    }
    if (ahora - cronometroRSSI >= 2000) {
      cronometroRSSI = ahora; client.publish("fxi/rssi", ("{\"rssi\":" + String(rssiGuardado) + "}").c_str());
    }
  }

  if (ahora - ultimoChequeoOTA >= INTERVALO_OTA) { ultimoChequeoOTA = ahora; chequearActualizacionGitHub(); }

  static unsigned long ultimaActualizacionHTML = 0;
  if (ahora - ultimaActualizacionHTML >= 86400000UL && WiFi.status() == WL_CONNECTED) { actualizarHTML(); ultimaActualizacionHTML = ahora; }
}
