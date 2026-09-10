#include "portal.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "log.h"
#include "settings.h"

// WPA2 exige minimo 8 caracteres. El password sigue fijo aca: cambiarlo
// requiere editar este archivo y reflashear. El SSID en cambio si es
// configurable desde el modal de ajustes (settings.json), porque con varias
// cuevas desplegadas todas emitirian el mismo nombre de red.
#define AP_PASSWORD "cuevas123"

static WebServer server(80);
static bool portalActivo = false;
static unsigned long portalInicio = 0;
static void (*ajustesCallback)(const Settings &) = nullptr;
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
  String json = "{\"ssid\":\"" + settingsActuales.ssid + "\",\"hostname\":\"" +
                settingsActuales.hostname + "\",\"volumen\":" + String(settingsActuales.volumen) +
                ",\"colorIdle\":\"" + settingsActuales.colorIdle + "\",\"colorDetectado\":\"" +
                settingsActuales.colorDetectado + "\",\"colorReproduciendo\":\"" +
                settingsActuales.colorReproduciendo +
                "\",\"largoTira\":" + String(settingsActuales.largoTira) + "}";
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
  nuevo.ssid = doc["ssid"] | settingsActuales.ssid;
  nuevo.hostname = doc["hostname"] | settingsActuales.hostname;
  nuevo.volumen = doc["volumen"] | settingsActuales.volumen;
  nuevo.colorIdle = doc["colorIdle"] | settingsActuales.colorIdle;
  nuevo.colorDetectado = doc["colorDetectado"] | settingsActuales.colorDetectado;
  nuevo.colorReproduciendo = doc["colorReproduciendo"] | settingsActuales.colorReproduciendo;
  nuevo.largoTira = doc["largoTira"] | settingsActuales.largoTira;
  if (nuevo.volumen > 30) {
    nuevo.volumen = 30; // rango valido del DFPlayer
  }

  if (!colorValido(nuevo.colorIdle) || !colorValido(nuevo.colorDetectado) ||
      !colorValido(nuevo.colorReproduciendo)) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"color mal formado, se espera #RRGGBB\"}");
    return;
  }

  if (nuevo.largoTira == 0 || nuevo.largoTira > LED_MAX_LARGO) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"largo de tira fuera de rango (1-" +
                    String(LED_MAX_LARGO) + ")\"}");
    return;
  }

  // Se rechaza en vez de corregir en silencio: guardar un SSID imposible
  // dejaria el AP sin levantar en el proximo arranque, y sin AP no hay portal
  // ni OTA para deshacerlo -- solo el cable.
  if (!ssidValido(nuevo.ssid)) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"SSID vacio o de mas de 32 caracteres\"}");
    return;
  }

  if (!guardarSettings(nuevo)) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"no se pudo guardar\"}");
    return;
  }

  bool hostnameCambio = (nuevo.hostname != settingsActuales.hostname);
  bool ssidCambio = (nuevo.ssid != settingsActuales.ssid);
  settingsActuales = nuevo;

  if (ajustesCallback) {
    ajustesCallback(settingsActuales);
  }
  logf("Portal: ajustes actualizados (hostname=%s, volumen=%u, tira=%u LEDs, idle=%s,"
       " detectado=%s, reproduciendo=%s).",
       settingsActuales.hostname.c_str(), settingsActuales.volumen, settingsActuales.largoTira,
       settingsActuales.colorIdle.c_str(), settingsActuales.colorDetectado.c_str(),
       settingsActuales.colorReproduciendo.c_str());

  if (hostnameCambio) {
    MDNS.end();
    MDNS.begin(settingsActuales.hostname.c_str());
    logf("Portal: mDNS ahora responde en %s.local", settingsActuales.hostname.c_str());
  }

  // El SSID, a diferencia del hostname, NO se aplica en caliente: rehacer el
  // softAP tira a todos los clientes conectados, empezando por el que acaba
  // de mandar este POST -- que se quedaria sin saber si su cambio se guardo.
  // Queda escrito y toma efecto en el proximo arranque.
  if (ssidCambio) {
    logf("Portal: SSID nuevo '%s'. Toma efecto al reiniciar el ESP; hasta"
         " entonces el AP sigue siendo el anterior.",
         settingsActuales.ssid.c_str());
  }

  server.send(200, "application/json",
              ssidCambio ? "{\"ok\":true,\"reiniciar\":true}" : "{\"ok\":true}");
}

void iniciarPortal(void (*onAjustesCambiados)(const Settings &)) {
  ajustesCallback = onAjustesCambiados;
  settingsActuales = cargarSettings();

  WiFi.softAP(settingsActuales.ssid.c_str(), AP_PASSWORD);
  logf("Portal: AP '%s' arriba, IP %s", settingsActuales.ssid.c_str(),
       WiFi.softAPIP().toString().c_str());

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
