#include "portal.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "log.h"
#include "settings.h"

// WPA2 exige minimo 8 caracteres. Password fijo: no forma parte del modal de
// ajustes (ese solo expone hostname/volumen) -- cambiarlo requiere editar
// este archivo y reflashear.
#define AP_SSID "Cueva1"
#define AP_PASSWORD "cuevas123"

static WebServer server(80);
static bool portalActivo = false;
static unsigned long portalInicio = 0;
static void (*volumenCallback)(uint8_t) = nullptr;
static Settings settingsActuales;

static void manejarRaiz() {
  File f = LittleFS.open("/www/index.html", "r");
  if (!f) {
    server.send(500, "text/plain",
                "Portal: falta /www/index.html en LittleFS (subilo con 'pio run -t uploadfs').");
    return;
  }
  server.streamFile(f, "text/html");
  f.close();
}

static void manejarGetLog() { server.send(200, "application/json", logComoJSON()); }

static void manejarGetConfig() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    server.send(404, "application/json", "{}");
    return;
  }
  server.streamFile(f, "application/json");
  f.close();
}

static void manejarPostConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"sin body\"}");
    return;
  }
  bool ok = guardarConfiguracionJSON(server.arg("plain"));
  server.send(ok ? 200 : 400, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"JSON invalido\"}");
}

static void manejarGetSettings() {
  String json = "{\"hostname\":\"" + settingsActuales.hostname +
                "\",\"volumen\":" + String(settingsActuales.volumen) + "}";
  server.send(200, "application/json", json);
}

static void manejarPostSettings() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"sin body\"}");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"JSON invalido\"}");
    return;
  }

  Settings nuevo;
  nuevo.hostname = doc["hostname"] | settingsActuales.hostname;
  nuevo.volumen = doc["volumen"] | settingsActuales.volumen;
  if (nuevo.volumen > 30) {
    nuevo.volumen = 30; // rango valido del DFPlayer
  }

  if (!guardarSettings(nuevo)) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"no se pudo guardar\"}");
    return;
  }

  bool hostnameCambio = (nuevo.hostname != settingsActuales.hostname);
  settingsActuales = nuevo;

  if (volumenCallback) {
    volumenCallback(settingsActuales.volumen);
  }
  logf("Portal: ajustes actualizados (hostname=%s, volumen=%u).", settingsActuales.hostname.c_str(),
       settingsActuales.volumen);

  if (hostnameCambio) {
    MDNS.end();
    MDNS.begin(settingsActuales.hostname.c_str());
    logf("Portal: mDNS ahora responde en %s.local", settingsActuales.hostname.c_str());
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

void iniciarPortal(void (*onVolumenCambiado)(uint8_t)) {
  volumenCallback = onVolumenCambiado;
  settingsActuales = cargarSettings();

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  logf("Portal: AP '%s' arriba, IP %s", AP_SSID, WiFi.softAPIP().toString().c_str());

  if (MDNS.begin(settingsActuales.hostname.c_str())) {
    logf("Portal: mDNS activo -> http://%s.local/", settingsActuales.hostname.c_str());
  } else {
    logln("Portal: no se pudo iniciar mDNS (entra igual por IP).");
  }

  server.on("/", HTTP_GET, manejarRaiz);
  server.on("/api/log", HTTP_GET, manejarGetLog);
  server.on("/api/config", HTTP_GET, manejarGetConfig);
  server.on("/api/config", HTTP_POST, manejarPostConfig);
  server.on("/api/settings", HTTP_GET, manejarGetSettings);
  server.on("/api/settings", HTTP_POST, manejarPostSettings);
  server.begin();

  portalActivo = true;
  portalInicio = millis();
  logf("Portal: activo. Se apaga solo a los %lu min del boot.", PORTAL_TIMEOUT_MS / 60000UL);
}

static void apagarPortal() {
  server.stop();
  MDNS.end();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  portalActivo = false;
  logf("Portal: apagado automatico (timeout de %lu min). Reiniciar el ESP para reactivarlo.",
       PORTAL_TIMEOUT_MS / 60000UL);
}

void actualizarPortal() {
  if (!portalActivo) {
    return;
  }
  if (millis() - portalInicio >= PORTAL_TIMEOUT_MS) {
    apagarPortal();
    return;
  }
  server.handleClient();
}
