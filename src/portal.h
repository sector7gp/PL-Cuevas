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

// 60 minutos. Arranco en 5, pero con OTA en el equipo esa ventana quedaba muy
// corta para trabajar: cada carga por red exigia resetear el ESP y apurarse.
#define PORTAL_TIMEOUT_MS (60UL * 60UL * 1000UL) // 60 minutos desde el boot

// Arranca el AP WiFi, mDNS y el servidor web. Llamar una vez en setup(),
// despues de cargarConfiguracion(). `onVolumenCambiado` se invoca cuando el
// modal de ajustes guarda un volumen nuevo, para que main.cpp lo aplique al
// DFPlayer sin que este modulo dependa de esa libreria.
void iniciarPortal(void (*onVolumenCambiado)(uint8_t));

// Atiende clientes HTTP pendientes y chequea el timeout de apagado. Llamar
// en cada vuelta de loop() sin condiciones -- no hace nada si el portal ya
// esta apagado.
void actualizarPortal();
