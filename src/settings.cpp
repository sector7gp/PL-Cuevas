#include "settings.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "log.h"

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

  s.hostname = doc["hostname"] | s.hostname;
  s.volumen = doc["volumen"] | s.volumen;
  if (s.volumen > 30) {
    s.volumen = 30; // rango valido del DFPlayer
  }
  return s;
}

bool guardarSettings(const Settings &s) {
  JsonDocument doc;
  doc["hostname"] = s.hostname;
  doc["volumen"] = s.volumen;

  File f = LittleFS.open("/settings.json", "w");
  if (!f) {
    logln("Settings: no se pudo abrir /settings.json para escribir.");
    return false;
  }
  serializeJson(doc, f);
  f.close();
  return true;
}
