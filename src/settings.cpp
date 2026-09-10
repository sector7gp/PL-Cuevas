#include "settings.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "log.h"

bool ssidValido(const String &ssid) {
  return ssid.length() > 0 && ssid.length() <= SSID_MAX_LARGO;
}

bool colorValido(const String &hex) {
  if (hex.length() != 7 || hex[0] != '#') {
    return false;
  }
  for (size_t i = 1; i < 7; i++) {
    if (!isxdigit((unsigned char)hex[i])) {
      return false;
    }
  }
  return true;
}

uint32_t colorDeHex(const String &hex, uint32_t porDefecto) {
  if (!colorValido(hex)) {
    return porDefecto;
  }
  return (uint32_t)strtoul(hex.c_str() + 1, nullptr, 16);
}

Settings cargarSettings() {
  Settings s; // arranca con los defaults del struct

  File f = LittleFS.open("/settings.json", "r");
  if (!f) {
    logln("Settings: /settings.json no existe, uso los valores por defecto.");
    return s;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    logf("Settings: error parseando /settings.json: %s", err.c_str());
    return s;
  }

  s.ssid = doc["ssid"] | s.ssid;
  s.hostname = doc["hostname"] | s.hostname;
  s.volumen = doc["volumen"] | s.volumen;
  s.colorIdle = doc["colorIdle"] | s.colorIdle;
  s.colorDetectado = doc["colorDetectado"] | s.colorDetectado;
  s.colorReproduciendo = doc["colorReproduciendo"] | s.colorReproduciendo;
  s.largoTira = doc["largoTira"] | s.largoTira;
  if (s.volumen > 30) {
    s.volumen = 30; // rango valido del DFPlayer
  }
  // Un settings.json editado a mano puede traer un SSID imposible. Volver al
  // default es preferible a levantar un AP roto: con el AP roto no hay portal
  // ni OTA para arreglarlo, solo el cable.
  if (!ssidValido(s.ssid)) {
    logf("Settings: SSID invalido en settings.json, uso '%s'.", Settings().ssid.c_str());
    s.ssid = Settings().ssid;
  }
  // Mismo criterio que el SSID: un color mal escrito a mano vuelve al default
  // en vez de propagarse como negro y dejar la tira apagada sin explicacion.
  if (!colorValido(s.colorIdle)) {
    logf("Settings: colorIdle invalido, uso %s.", Settings().colorIdle.c_str());
    s.colorIdle = Settings().colorIdle;
  }
  if (!colorValido(s.colorDetectado)) {
    logf("Settings: colorDetectado invalido, uso %s.", Settings().colorDetectado.c_str());
    s.colorDetectado = Settings().colorDetectado;
  }
  if (!colorValido(s.colorReproduciendo)) {
    logf("Settings: colorReproduciendo invalido, uso %s.", Settings().colorReproduciendo.c_str());
    s.colorReproduciendo = Settings().colorReproduciendo;
  }
  if (s.largoTira == 0 || s.largoTira > LED_MAX_LARGO) {
    logf("Settings: largoTira %u fuera de rango (1-%d), uso %u.", s.largoTira, LED_MAX_LARGO,
         Settings().largoTira);
    s.largoTira = Settings().largoTira;
  }
  return s;
}

bool guardarSettings(const Settings &s) {
  JsonDocument doc;
  doc["ssid"] = s.ssid;
  doc["hostname"] = s.hostname;
  doc["volumen"] = s.volumen;
  doc["colorIdle"] = s.colorIdle;
  doc["colorDetectado"] = s.colorDetectado;
  doc["colorReproduciendo"] = s.colorReproduciendo;
  doc["largoTira"] = s.largoTira;

  File f = LittleFS.open("/settings.json", "w");
  if (!f) {
    logln("Settings: no se pudo abrir /settings.json para escribir.");
    return false;
  }
  serializeJson(doc, f);
  f.close();
  return true;
}
