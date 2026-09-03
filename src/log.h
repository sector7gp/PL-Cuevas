/*
 * Logger compartido: cada linea va al monitor serie (como siempre) Y a un
 * buffer circular en RAM, que el portal web expone via /api/log para el
 * monitoreo de actividad en vivo. Un solo lugar para loguear evita
 * duplicar cada mensaje (uno para Serial, otro para el portal).
 */
#pragma once

#include <Arduino.h>

// printf-style: reemplaza Serial.printf() en todo el firmware.
void logf(const char *fmt, ...);

// Linea literal, sin formato: reemplaza Serial.println() en todo el firmware.
void logln(const char *msg);

// Devuelve las lineas recientes como un array JSON de strings, de mas vieja a
// mas nueva -- listo para mandar como respuesta HTTP.
String logComoJSON();
