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
// CAMBIO 1: Nueva versión para activar el robot
String FIRMWARE_VERSION = "1.1.0_TEST"; 
const char* urlVersion = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/version.txt";
const char* urlFirmware = "https://raw.githubusercontent.com/fluxaignis/fxi-dashboard/gh-pages/firmware.bin";

unsigned long ultimoChequeoOTA = 0;
const unsigned long INTERVALO_OTA = 300000; 

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

// --- CREDENCIALES ---
const char* ssid = "NauticaNet";
const char* password = "PromoXXX.2026";
const char* mqtt_server = "df734b8fbeed43978f29869442892dcf.s1.eu.hivemq.cloud";
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -14400;
const int daylightOffset_sec = 0;

// --- AP CONFIG ---
const char* ap_ssid = "FluxaIgnis";
const char* ap_password = ""; 
IPAddress apIP(192, 168, 1, 1);
IPAddress apGateway(192, 168, 1, 1);
IPAddress apSubnet(255, 255, 255, 0);

// --- FUNCIONES ---
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
    Serial.println("¡EMERGENCIA! Iniciando... Motivo: " + motivo);
    digitalWrite(PIN_BOMBA, HIGH);
    cronometroRutina = millis();
    estadoActual = ESPERANDO_AGUA;
  }
}

void detenerRutina() {
  servoHorizontal.write(SERVO_H_NEUTRAL);
  digitalWrite(PIN_BOMBA, LOW);
  estadoActual = REPOSO;
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  if (String(topic) == "fxi/comandos") {
    if (msg == "TOGGLE") {
      if (estadoActual == REPOSO) iniciarRutina("Comando MQTT");
      else detenerRutina();
    }
  } else if (String(topic) == "fxi/simular") {
    if (msg == "FUEGO_ON") simularFuego = true;
    else if (msg == "FUEGO_OFF") simularFuego = false;
  }
}

void handleRoot() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) { server.send(500, "text/plain", "Error"); return; }
  server.streamFile(file, "text/html");
  file.close();
}

void handleEstado() {
  String json = "{\"temp\":" + String(tempGuardada) + ",\"hum\":" + String(humGuardada) + ",\"rssi\":" + String(rssiGuardado) + "}";
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

void chequearActualizacionGitHub() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.println("Chequeando actualizaciones en GitHub...");
  HTTPClient http;
  http.begin(espClient, urlVersion); 
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String versionGitHub = http.getString();
    versionGitHub.trim();
    if (versionGitHub != FIRMWARE_VERSION && versionGitHub.length() > 0) {
      Serial.println("Nueva versión: " + versionGitHub);
      httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      t_httpUpdate_return ret = httpUpdate.update(espClient, urlFirmware);
    }
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(PIN_BOMBA, OUTPUT); digitalWrite(PIN_BOMBA, LOW);
  pinMode(PIN_ROJO, OUTPUT); pinMode(PIN_VERDE, OUTPUT); pinMode(PIN_AZUL, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT); noTone(PIN_BUZZER);

  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2); ESP32PWM::allocateTimer(3);
  servoHorizontal.setPeriodHertz(50); servoHorizontal.attach(PIN_SERVO_H, 500, 2400);
  servoVertical.setPeriodHertz(50); servoVertical.attach(PIN_SERVO_V, 500, 2400);
  servoHorizontal.write(SERVO_H_NEUTRAL); servoVertical.write(20);

  if (!LittleFS.begin()) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(ap_ssid, ap_password);
  dnsServer.start(53, "*", apIP);

  if (MDNS.begin("fluxaignis")) MDNS.addService("http", "tcp", 80);

  WiFi.begin(ssid, password);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) { delay(500); intentos++; }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    espClient.setInsecure();
    client.setServer(mqtt_server, 8883);
    client.setCallback(callback);
  }

  ArduinoOTA.begin();
  server.on("/", handleRoot);
  server.on("/estado", handleEstado);
  server.on("/toggleServo", handleToggle);
  server.on("/generate_204", []() { server.send(204); });
  server.onNotFound(handleNotFound);
  server.begin();
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

  int lecturaLlama;
  if (simularFuego) { lecturaLlama = 300; } 
  else { lecturaLlama = analogRead(PIN_LLAMA); if (lecturaLlama < 50) lecturaLlama = 4095; }

  if (estadoActual == REPOSO && lecturaLlama < UMBRAL_FUEGO) {
    iniciarRutina("Fuego detectado");
    if (!emergenciaEnviada) { enviarNotificacionMQTT("FUEGO", tempGuardada); emergenciaEnviada = true; }
  }

  if (tempGuardada >= TEMP_CRITICA) {
    if (!emergenciaEnviada) { enviarNotificacionMQTT("CALOR", tempGuardada); emergenciaEnviada = true; iniciarRutina("Temp Critica"); }
  } else if (tempGuardada < TEMP_CRITICA - 2.0) { emergenciaEnviada = false; }

  // --- CAMBIO 2: Indicador visual de actualización ---
  if (estadoActual != REPOSO) {
    setColor(255, 0, 0); // Rojo en emergencia
    if ((ahora / 300) % 2 == 0) tone(PIN_BUZZER, 2000); else noTone(PIN_BUZZER);
  } else {
    noTone(PIN_BUZZER);
    // Si la temperatura es normal, el LED ahora será CIAN (Verde + Azul)
    int r = (tempGuardada >= 35) ? 255 : (tempGuardada >= 30 ? 255 : 0);
    int g = (tempGuardada >= 35) ? 0 : (tempGuardada >= 30 ? 100 : 255);
    int b = (tempGuardada < 30) ? 255 : 0; // <--- AÑADIMOS AZUL PARA NOTAR EL CAMBIO
    setColor(r, g, b);
  }

  switch (estadoActual) {
    case REPOSO: break;
    case ESPERANDO_AGUA:
      if (ahora - cronometroRutina >= WATER_DELAY_MS) { servoHorizontal.write(SERVO_H_NEUTRAL); cronometroRutina = ahora; estadoActual = GIRANDO_IZQ; }
      break;
    case GIRANDO_IZQ:
      servoHorizontal.write(70);
      if (ahora - cronometroRutina >= TIEMPO_90_IZQ) { cronometroRutina = ahora; estadoActual = GIRANDO_DER; }
      break;
    case GIRANDO_DER:
      servoHorizontal.write(110);
      if (ahora - cronometroRutina >= (TIEMPO_90_DER * 2)) { cronometroRutina = ahora; estadoActual = VOLVIENDO_CENTRO; }
      break;
    case VOLVIENDO_CENTRO:
      servoHorizontal.write(70);
      if (ahora - cronometroRutina >= TIEMPO_RETORNO) detenerRutina();
      break;
  }

  if (WiFi.status() == WL_CONNECTED && client.connected()) {
    if (ahora - cronometroDatos > 2000) {
      cronometroDatos = ahora;
      client.publish("fxi/datos", ("{\"temp\":" + String(tempGuardada) + ", \"hum\":" + String(humGuardada) + "}").c_str());
    }
    if (ahora - cronometroRSSI > 5000) {
      cronometroRSSI = ahora;
      client.publish("fxi/rssi", ("{\"rssi\":" + String(rssiGuardado) + "}").c_str());
    }
  }

  if (ahora - ultimoChequeoOTA >= INTERVALO_OTA) {
    ultimoChequeoOTA = ahora;
    chequearActualizacionGitHub();
  }
}
