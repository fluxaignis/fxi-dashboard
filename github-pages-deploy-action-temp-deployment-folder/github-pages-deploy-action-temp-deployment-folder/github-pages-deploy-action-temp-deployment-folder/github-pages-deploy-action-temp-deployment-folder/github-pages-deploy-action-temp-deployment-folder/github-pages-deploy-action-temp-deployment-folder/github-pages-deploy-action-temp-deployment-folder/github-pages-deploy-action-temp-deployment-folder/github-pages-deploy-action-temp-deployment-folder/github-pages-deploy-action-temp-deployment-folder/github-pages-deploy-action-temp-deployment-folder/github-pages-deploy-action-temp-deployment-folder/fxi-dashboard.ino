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

// ==================== GITHUB OTA ====================
String FIRMWARE_VERSION = "1.0.46";
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
PubSubClient client(espClient);   // <--- Declarado aquí
WebServer server(80);
DNSServer dnsServer;

// ==================== FUNCIÓN addLog (ahora después de declarar client) ====================
void addLog(String level, String message) {
  if (message.length() > 100) message = message.substring(0, 97) + "...";
  logBuffer[logIndex].timestamp = millis();
  logBuffer[logIndex].level = level;
  logBuffer[logIndex].message = message;
  logIndex = (logIndex + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS) logCount++;
  Serial.printf("[%s] %s\n", level.c_str(), message.c_str());

  // Publicar el log en MQTT si el cliente está conectado
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

// ==================== VARIABLES ====================
const int UMBRAL_FUEGO = 500;
const int UMBRAL_GAS = 400;
const float TEMP_CRITICA = 40.0;
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

// ==================== GESTIÓN CREDENCIALES WiFi ====================
bool leerCredencialesWiFi() {
  File f = LittleFS.open("/wifi.txt", "r");
  if (!f) {
    wifiSSID = DEFAULT_SSID;
    wifiPassword = DEFAULT_PASS;
    return false;
  }
  wifiSSID = f.readStringUntil('\n');
  wifiPassword = f.readStringUntil('\n');
  wifiSSID.trim();
  wifiPassword.trim();
  f.close();
  if (wifiSSID.length() == 0) {
    wifiSSID = DEFAULT_SSID;
    wifiPassword = DEFAULT_PASS;
    return false;
  }
  addLog("info", "Credenciales WiFi leídas: " + wifiSSID);
  return true;
}

void guardarCredencialesWiFi(const String &ssid, const String &password) {
  File f = LittleFS.open("/wifi.txt", "w");
  if (f) {
    f.println(ssid);
    f.println(password);
    f.close();
    addLog("info", "Credenciales guardadas");
  }
}

// ==================== ACTUALIZACIONES OTA y HTML ====================
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
  if (!f) return "1.0.46";
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

// ==================== MQTT ADMIN ====================
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

// ==================== CALLBACK MQTT ====================
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  String topicStr = String(topic);
  if (topicStr == "fxi/comandos") {
    if (msg == "TOGGLE") {
      if (estadoActual == REPOSO) iniciarRutina(0, "Comando Manual MQTT");
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
  if (isnan(tempGuardada)) json += "\"temp\":null,";
  else json += "\"temp\":" + String(tempGuardada) + ",";
  if (isnan(humGuardada)) json += "\"hum\":null,";
  else json += "\"hum\":" + String(humGuardada) + ",";
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

// ==================== PÁGINA DE CONFIGURACIÓN WIFI ====================
void handleWiFiConfig() {
  String token = server.arg("token");
  if (token != ADMIN_TOKEN) {
    server.send(401, "text/plain", "Unauthorized: token incorrecto");
    return;
  }
  if (server.method() == HTTP_GET) {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width'><title>Configurar WiFi</title>";
    html += "<style>body{background:#0B3954;color:white;font-family:sans-serif;padding:20px;}";
    html += "input{width:100%;padding:10px;margin:10px 0;border-radius:20px;border:none;}";
    html += "button{background:#7AF0D4;border:none;padding:10px 20px;border-radius:20px;font-weight:bold;}</style>";
    html += "</head><body><h2>Configuración de Red WiFi</h2>";
    html += "<form action='/wifi' method='POST'>";
    html += "<input type='hidden' name='token' value='" + String(ADMIN_TOKEN) + "'>";
    html += "SSID:<br><input type='text' name='ssid' value='" + wifiSSID + "'><br>";
    html += "Contraseña:<br><input type='password' name='pass' value=''><br>";
    html += "<button type='submit'>Guardar y Reiniciar</button>";
    html += "</form></body></html>";
    server.send(200, "text/html", html);
  } else if (server.method() == HTTP_POST) {
    String newSSID = server.arg("ssid");
    String newPass = server.arg("pass");
    if (newSSID.length() > 0) {
      guardarCredencialesWiFi(newSSID, newPass);
      server.send(200, "text/html", "<html><body><h2>Credenciales guardadas. Reiniciando...</h2><script>setTimeout(function(){location.href='/';},3000);</script></body></html>");
      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "SSID no puede estar vacío");
    }
  }
}

// ==================== CONEXIÓN WiFi DINÁMICA ====================
bool conectarWiFiSTA() {
  leerCredencialesWiFi();
  if (wifiSSID.length() == 0) return false;
  addLog("info", "Conectando a WiFi: " + wifiSSID);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 40) {
    delay(500);
    intentos++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    addLog("info", "WiFi conectado - IP: " + WiFi.localIP().toString());
    return true;
  }
  addLog("error", "Error al conectar a " + wifiSSID);
  return false;
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

  // Inicializar LittleFS con formateo automático si falla
  bool littlefsOk = LittleFS.begin(false);
  if (!littlefsOk) {
    addLog("error", "Error al montar LittleFS. Formateando...");
    littlefsOk = LittleFS.begin(true);
    if (littlefsOk) {
      addLog("info", "LittleFS formateado correctamente. Reiniciando...");
      ESP.restart();
    } else {
      addLog("error", "No se pudo formatear LittleFS. El sistema no funcionará correctamente.");
      return;
    }
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

  bool wifiOk = conectarWiFiSTA();
  if (wifiOk) {
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
    addLog("warn", "Modo offline. Configura WiFi en http://192.168.1.1/wifi?token=config2026");
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
  server.on("/wifi", HTTP_GET, handleWiFiConfig);
  server.on("/wifi", HTTP_POST, handleWiFiConfig);
  server.onNotFound(handleNotFound);
  server.begin();

  addLog("info", "Servidor web listo. Accede a http://192.168.1.1");
  Serial.println("\n=== Panel de control ===");
  Serial.println("Para configurar WiFi: http://192.168.1.1/wifi?token=config2026");
  Serial.println("Token por defecto: config2026");
  Serial.println("========================================\n");

  ultimoChequeoOTA = millis();

  // Lectura inicial del DHT
  for (int i = 0; i < 5 && (isnan(tempGuardada) || isnan(humGuardada)); i++) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && t > 0 && t < 80) tempGuardada = t;
    if (!isnan(h) && h >= 0 && h <= 100) humGuardada = h;
    delay(2000);
  }
}

// ==================== LOOP ====================
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();

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
      Serial.println("Comando no reconocido. Usa 'help'.");
    }
  }

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

  if (ahora - ultimoTiempoDHT >= 2000) {
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

  if (estadoActual == REPOSO && !emergenciaActiva) {
    bool fuegoIzq = (llamaIzq < UMBRAL_FUEGO);
    bool fuegoDer = (llamaDer < UMBRAL_FUEGO);
    bool hayGas = (gasValue > UMBRAL_GAS);
    bool calorCritico = (tempGuardada >= TEMP_CRITICA);
    bool simulacion = simularFuego;

    int lado = 0;
    String motivo = "";

    if (simulacion) {
      lado = 0;
      motivo = "SIMULACIÓN DE FUEGO";
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

  if (estadoActual != REPOSO) {
    setColor(255, 0, 0);
    if ((ahora / 300) % 2 == 0) tone(PIN_BUZZER, 2000);
    else noTone(PIN_BUZZER);
  } else {
    noTone(PIN_BUZZER);
    if (!isnan(tempGuardada) && tempGuardada >= 35.0) {
      setColor(255, 0, 0);
    } else {
      if (WiFi.status() == WL_CONNECTED) {
        if (client.connected()) setColor(0, 255, 0);
        else setColor(255, 255, 0);
      } else {
        setColor(0, 255, 255);
      }
    }
  }

  switch (estadoActual) {
    case REPOSO:
      break;
    case ESPERANDO_AGUA:
      if (ahora - cronometroRutina >= WATER_DELAY_MS) {
        addLog("info", "Agua lista, apuntando servo");
        switch (ladoEmergencia) {
          case 1: servoHorizontal.write(SERVO_IZQ); break;
          case 2: servoHorizontal.write(SERVO_DER); break;
          default: servoHorizontal.write(SERVO_CENTRO); break;
        }
        cronometroRutina = ahora;
        estadoActual = APUNTANDO;
      }
      break;
    case APUNTANDO:
      if (ahora - cronometroRutina >= TIEMPO_APUNTAR) {
        detenerRutina();
      }
      break;
  }

  if (WiFi.status() == WL_CONNECTED && client.connected()) {
    if (ahora - cronometroDatos > 2000) {
      cronometroDatos = ahora;
      String hStr = isnan(humGuardada) ? "0" : String(humGuardada);
      String tStr = isnan(tempGuardada) ? "0" : String(tempGuardada);
      String payload = "{\"temp\":" + tStr + ",\"hum\":" + hStr + ",\"gas\":" + String(gasValue) +
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

  if (ahora - ultimoChequeoOTA >= INTERVALO_OTA) {
    ultimoChequeoOTA = ahora;
    chequearActualizacionGitHub();
  }

  static unsigned long ultimaActualizacionHTML = 0;
  if (ahora - ultimaActualizacionHTML >= 86400000UL) {
    if (WiFi.status() == WL_CONNECTED) actualizarHTML();
    ultimaActualizacionHTML = ahora;
  }
}
