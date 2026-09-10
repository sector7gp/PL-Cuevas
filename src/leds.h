/*
 * Tira WS2812 de indicacion visual, en GPIO47. Separado de main.cpp igual
 * que log/settings/portal: un modulo por responsabilidad.
 *
 * Tres estados, que main.cpp elige por prioridad en cada vuelta de loop():
 * reproduciendo gana sobre detectado, y detectado sobre reposo.
 *
 *   EFECTO_IDLE           nada puesto        glow (respiracion continua)
 *   EFECTO_DETECTADO      hay tags puestos   fade al color y queda fijo
 *   EFECTO_REPRODUCIENDO  suena el cuento    fade al color y queda fijo
 *
 * Todos los cambios de estado entran con un fade de FADE_MS desde el color
 * que hubiera puesto, asi nunca hay saltos de color. Los tres colores salen
 * de settings.json y se cambian en caliente (ver setColoresLed).
 *
 * Para sumar un efecto: un valor mas en EfectoLed, su caso en el switch de
 * actualizarEfecto() (leds.cpp), y su color en Settings.
 */
#pragma once

#include <Arduino.h>

enum EfectoLed {
  EFECTO_IDLE,          // reposo: glow, respiracion continua
  EFECTO_DETECTADO,     // hay uno o mas tags puestos: color fijo
  EFECTO_REPRODUCIENDO, // esta sonando un cuento: color fijo
};

// Tope de LEDs aceptado. No es una limitacion de la libreria sino del costo
// de pixels.show(), que deshabilita interrupciones ~30 us por LED: con 300
// son ~9 ms por refresco, y durante el glow eso se paga 50 veces por segundo.
// Mas que esto empieza a competirle en serio al WiFi y al OTA.
#define LED_MAX_LARGO 300

// Arranca la tira con el largo y los colores de settings.json. Llamar una vez
// en setup(). Deja la tira en EFECTO_IDLE.
void iniciarLeds(uint16_t largo, uint32_t colorIdle, uint32_t colorDetectado,
                 uint32_t colorReproduciendo);

// Cambia la cantidad de LEDs en caliente (reasigna el buffer de la tira).
// Sirve para no tener que reflashear si se alarga o acorta la tira fisica.
void setLargoTira(uint16_t largo);

// Dibuja el frame que corresponda. No bloquea: se autolimita a ~50 Hz y, en
// los dos efectos de color fijo, deja de refrescar del todo una vez que el
// fade termino -- ver `estabilizado` en leds.cpp. EFECTO_IDLE si anima
// siempre, porque la respiracion es continua por definicion.
void actualizarLeds();

// Cambia el efecto activo, arrancando un fade desde el color que se este
// mostrando. No hace nada si ya es el efecto activo, asi que se puede llamar
// en cada vuelta de loop() sin condiciones.
void setEfectoLed(EfectoLed efecto);

// Cambia los colores en caliente (el portal los edita en el modal de
// ajustes). Reanima desde el color actual, para que se vea suave.
void setColoresLed(uint32_t colorIdle, uint32_t colorDetectado, uint32_t colorReproduciendo);
