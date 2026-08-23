#include "config.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#define MAX_PERSONAJES 8
#define MAX_HISTORIAS 16

struct Personaje {
  int id;
  String nombre;
  uint8_t uid[UID_MAX_LEN];
  uint8_t uidLen;
  int pistaSolo; // 0 = sin audio de "personaje solitario" configurado
};

struct Historia {
  int personajeA; // siempre el menor de los dos ids -- normalizado al cargar
  int personajeB; // siempre el mayor
  int pista;
};

static Personaje personajes[MAX_PERSONAJES];
static uint8_t numPersonajes = 0;
static Historia historias[MAX_HISTORIAS];
static uint8_t numHistorias = 0;

// Parsea un UID en hex sin separadores ("04B7E574C12A81") a bytes. Es el
// mismo orden en que main.cpp imprime el UID leido de un tag por serie, asi
// que copiar y pegar esa salida al JSON funciona directo.
static uint8_t parsearUIDHex(const char *hex, uint8_t *out, uint8_t maxLen) {
  uint8_t len = 0;
  size_t hexLen = strlen(hex);
  for (size_t i = 0; i + 1 < hexLen && len < maxLen; i += 2) {
    char byteStr[3] = {hex[i], hex[i + 1], '\0'};
    out[len++] = (uint8_t)strtol(byteStr, nullptr, 16);
  }
  return len;
}

bool cargarConfiguracion() {
  numPersonajes = 0;
  numHistorias = 0;

  // format=true: si la particion nunca se formateo para LittleFS, la
  // inicializa en vez de fallar. Solo pasa la primera vez.
  if (!LittleFS.begin(true)) {
    Serial.println("Config: no se pudo montar LittleFS.");
    return false;
  }

  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    Serial.println("Config: /config.json no existe. Subilo con 'pio run -t uploadfs'.");
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("Config: error parseando /config.json: %s\n", err.c_str());
    return false;
  }

  for (JsonObject p : doc["personajes"].as<JsonArray>()) {
    if (numPersonajes >= MAX_PERSONAJES) {
      Serial.println("Config: mas personajes de los que soporta MAX_PERSONAJES, se ignoran el resto.");
      break;
    }
    Personaje &pj = personajes[numPersonajes];
    pj.id = p["id"] | 0;
    pj.nombre = p["nombre"] | "(sin nombre)";
    const char *uidHex = p["uid"] | "";
    pj.uidLen = parsearUIDHex(uidHex, pj.uid, UID_MAX_LEN);
    pj.pistaSolo = p["pistaSolo"] | 0;
    numPersonajes++;
  }

  for (JsonObject h : doc["historias"].as<JsonArray>()) {
    if (numHistorias >= MAX_HISTORIAS) {
      Serial.println("Config: mas historias de las que soporta MAX_HISTORIAS, se ignoran el resto.");
      break;
    }
    JsonArray par = h["personajes"].as<JsonArray>();
    if (par.size() != 2) {
      Serial.println("Config: entrada de 'historias' sin exactamente 2 personajes, se ignora.");
      continue;
    }
    int a = par[0].as<int>();
    int b = par[1].as<int>();
    Historia &hs = historias[numHistorias];
    hs.personajeA = min(a, b);
    hs.personajeB = max(a, b);
    hs.pista = h["pista"] | 0;
    numHistorias++;
  }

  Serial.printf("Config: %u personajes, %u historias cargadas.\n", numPersonajes, numHistorias);
  return true;
}

int personajeDeUID(const uint8_t *uid, uint8_t len) {
  for (uint8_t i = 0; i < numPersonajes; i++) {
    if (personajes[i].uidLen == len && memcmp(personajes[i].uid, uid, len) == 0) {
      return personajes[i].id;
    }
  }
  return 0;
}

int pistaDePersonajes(int idA, int idB) {
  int lo = min(idA, idB);
  int hi = max(idA, idB);
  for (uint8_t i = 0; i < numHistorias; i++) {
    if (historias[i].personajeA == lo && historias[i].personajeB == hi) {
      return historias[i].pista;
    }
  }
  return 0;
}

int pistaSolitariaDePersonaje(int id) {
  for (uint8_t i = 0; i < numPersonajes; i++) {
    if (personajes[i].id == id) {
      return personajes[i].pistaSolo;
    }
  }
  return 0;
}

const char *nombreDePersonaje(int id) {
  for (uint8_t i = 0; i < numPersonajes; i++) {
    if (personajes[i].id == id) {
      return personajes[i].nombre.c_str();
    }
  }
  return "desconocido";
}
