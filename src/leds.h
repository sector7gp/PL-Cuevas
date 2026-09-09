/*
 * Tira WS2812 de indicacion visual, en GPIO47. Separado de main.cpp igual
 * que log/settings/portal: un modulo por responsabilidad.
 *
 * Pensado para sumar efectos con el tiempo sin tocar la forma de llamarlo
 * desde afuera: agregar un efecto nuevo es sumar un valor a EfectoLed y su
 * caso en el switch de actualizarEfecto() (leds.cpp) -- iniciarLeds(),
 * actualizarLeds() y setEfectoLed() no cambian.
 */
#pragma once

#include <Arduino.h>

// Efectos disponibles. EFECTO_IDLE es el unico por ahora (glow verde en
// respiracion continua); los que se sumen despues van aca.
enum EfectoLed {
  EFECTO_IDLE,
};

// Arranca la tira (pixels.begin()). Llamar una vez en setup().
void iniciarLeds();

// Dibuja el frame que corresponda del efecto activo segun el tiempo
// transcurrido. No bloquea: se autolimita a ~50 Hz internamente, así que se
// puede llamar en cada vuelta de loop() sin condiciones ni costo extra.
void actualizarLeds();

// Cambia el efecto activo. Reinicia el reloj de la animacion (el nuevo
// efecto arranca siempre desde su fase inicial, sin arrastrar tiempo del
// efecto anterior). No hace nada si ya es el efecto activo.
void setEfectoLed(EfectoLed efecto);
