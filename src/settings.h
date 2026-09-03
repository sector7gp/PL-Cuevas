/*
 * Configuracion general del sistema (hostname mDNS, volumen del DFPlayer).
 * Vive en /settings.json sobre LittleFS, separado de config.json (que es
 * pura tabla de personajes/historias) porque son dos cosas distintas que
 * edita cada uno su propio modal en el portal.
 */
#pragma once

#include <Arduino.h>

struct Settings {
  String hostname = "cueva1"; // sin ".local", eso lo agrega mDNS
  uint8_t volumen = 25;       // 0-30, rango del DFPlayer
};

// Carga /settings.json desde LittleFS. Si falta o esta corrupto, se queda
// con los valores por defecto de arriba -- no cuelga el arranque.
Settings cargarSettings();

// Escribe nuevos valores a /settings.json. Devuelve false si no pudo
// escribir el archivo (LittleFS lleno, etc.).
bool guardarSettings(const Settings &s);
