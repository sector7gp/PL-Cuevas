/*
 * Portal web de monitoreo/configuracion: WiFi en modo Access Point propio
 * (no depende de credenciales ni de un router externo), mDNS, y un servidor
 * HTTP con la UI y la API que usan los dos modales del portal (personajes en
 * config.json, ajustes de hostname/volumen en settings.json) y el
 * monitoreo de actividad en vivo.
 *
 * Se apaga solo pasados PORTAL_TIMEOUT_MS desde el boot -- reduce la
 * ventana de exposicion del AP y libera RAM/CPU para el resto del
 * firmware, que sigue funcionando igual sin el portal.
 *
 * OJO: apagar el portal apaga el WiFi entero (WiFi.mode(WIFI_OFF)), y con el
 * se va tambien la carga por red -- ver src/ota.h. O sea que este timeout es
 * tambien la ventana para hacer OTA despues de cada reset.
 */
#pragma once

#include <Arduino.h>

#include "settings.h"

// 60 minutos. Arranco en 5, pero con OTA en el equipo esa ventana quedaba muy
// corta para trabajar: cada carga por red exigia resetear el ESP y apurarse.
#define PORTAL_TIMEOUT_MS (60UL * 60UL * 1000UL) // 60 minutos desde el boot

// Arranca el AP WiFi, mDNS y el servidor web. Llamar una vez en setup(),
// despues de cargarConfiguracion(). `onAjustesCambiados` se invoca cada vez
// que el modal de ajustes guarda, con los valores ya validados, para que
// main.cpp los aplique al hardware (volumen al DFPlayer, colores a la tira)
// sin que este modulo dependa de esas librerias. Es un unico callback con
// todo el struct a proposito: agregar un ajuste no obliga a sumar otro.
void iniciarPortal(void (*onAjustesCambiados)(const Settings &));

// Atiende clientes HTTP pendientes y chequea el timeout de apagado. Llamar
// en cada vuelta de loop() sin condiciones -- no hace nada si el portal ya
// esta apagado.
void actualizarPortal();
