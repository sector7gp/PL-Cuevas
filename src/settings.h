/*
 * Configuracion general del sistema (SSID del AP, hostname mDNS, volumen del
 * DFPlayer). Vive en /settings.json sobre LittleFS, separado de config.json
 * (que es pura tabla de personajes/historias) porque son dos cosas distintas
 * que edita cada uno su propio modal en el portal.
 */
#pragma once

#include <Arduino.h>

// Por LED_MAX_LARGO: el rango valido de largoTira lo determina el modulo de
// la tira (es un limite de costo de refresco), no este.
#include "leds.h"

// Limite de 802.11 para un SSID. Un SSID vacio o mas largo deja el AP
// inservible, y sin AP no hay portal ni OTA: el unico camino de vuelta seria
// el cable. Por eso se valida al cargar Y al guardar.
#define SSID_MAX_LARGO 32

struct Settings {
  String ssid = "Cueva1";     // SSID del Access Point propio
  String hostname = "cueva1"; // sin ".local", eso lo agrega mDNS
  uint8_t volumen = 25;       // 0-30, rango del DFPlayer

  // Colores de la tira WS2812, en "#RRGGBB". Se guardan como texto y no como
  // entero porque es lo que produce y consume el <input type="color"> del
  // portal, y ademas deja settings.json legible a ojo.
  String colorIdle = "#00B200";           // reposo: verde (pico del glow)
  String colorDetectado = "#80FFFF";      // hay tags puestos: cian claro
  String colorReproduciendo = "#FFA000";  // suena el cuento: ambar

  uint16_t largoTira = 60; // cantidad de LEDs, 1..LED_MAX_LARGO
};

// true si el SSID es usable (no vacio y dentro del limite de 802.11).
bool ssidValido(const String &ssid);

// true si el texto es un color "#RRGGBB" bien formado.
bool colorValido(const String &hex);

// Convierte "#RRGGBB" a 0x00RRGGBB. Devuelve `porDefecto` si no parsea, para
// que un color invalido apague la tira o rompa el arranque.
uint32_t colorDeHex(const String &hex, uint32_t porDefecto);

// Carga /settings.json desde LittleFS. Si falta o esta corrupto, se queda
// con los valores por defecto de arriba -- no cuelga el arranque.
Settings cargarSettings();

// Escribe nuevos valores a /settings.json. Devuelve false si no pudo
// escribir el archivo (LittleFS lleno, etc.).
bool guardarSettings(const Settings &s);
