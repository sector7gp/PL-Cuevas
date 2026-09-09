#include "ota.h"

#include <ArduinoOTA.h>

#include "log.h"

// true mientras dura una transferencia. Lo consulta main.cpp desde loop():
// mientras esta en true no se llama ni al portal ni a los lectores, porque
// los dos le harian dano a la carga (ver el comentario en loop()).
static bool enProgreso = false;

// Ultima decena de porcentaje logueada. El buffer del logger son 40 lineas:
// loguear cada avance lo llenaria de ruido y se comeria el historial del
// arranque, que es justo lo que uno quiere leer si algo sale mal.
static int ultimaDecena = -1;

void iniciarOTA(const char *hostname) {
  ArduinoOTA.setHostname(hostname);

  ArduinoOTA.onStart([]() {
    enProgreso = true;
    ultimaDecena = -1;
    // U_FLASH es el firmware; el otro comando es la imagen de LittleFS, que
    // llega por `pio run -e ota -t uploadfs` y pisa data/ (config.json, www/).
    logln(ArduinoOTA.getCommand() == U_FLASH ? "OTA: entrando firmware nuevo..."
                                             : "OTA: entrando imagen de LittleFS...");
  });

  ArduinoOTA.onProgress([](unsigned int hechos, unsigned int total) {
    if (total == 0) {
      return;
    }
    int pct = (int)((hechos * 100UL) / total);
    if (pct / 10 != ultimaDecena) {
      ultimaDecena = pct / 10;
      logf("OTA: %d%%", pct);
    }
  });

  ArduinoOTA.onEnd([]() {
    enProgreso = false;
    logln("OTA: carga completa. Reiniciando con el firmware nuevo.");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    enProgreso = false;
    const char *causa;
    switch (error) {
      case OTA_AUTH_ERROR:
        causa = "autenticacion";
        break;
      case OTA_BEGIN_ERROR:
        causa = "no pudo empezar (particion o espacio)";
        break;
      case OTA_CONNECT_ERROR:
        causa = "no se pudo conectar";
        break;
      case OTA_RECEIVE_ERROR:
        causa = "se corto la recepcion";
        break;
      case OTA_END_ERROR:
        causa = "fallo el cierre";
        break;
      default:
        causa = "desconocido";
        break;
    }
    // No se reinicia: sigue corriendo el firmware viejo, que quedo intacto en
    // su particion. Se puede reintentar sin tocar el cable.
    logf("OTA: ERROR %u -- %s. Sigue corriendo el firmware anterior.", (unsigned)error, causa);
  });

  ArduinoOTA.begin();
  logf("OTA: escuchando en %s.local:3232 (pio run -e ota -t upload).", hostname);
}

void actualizarOTA() { ArduinoOTA.handle(); }

bool otaEnProgreso() { return enProgreso; }
