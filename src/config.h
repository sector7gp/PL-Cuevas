/*
 * Configuracion del sistema de historias: UID->personaje y
 * personaje+personaje->pista. Vive en /config.json sobre LittleFS, no
 * compilada en el firmware -- se sube por separado con
 * `pio run -t uploadfs` y se puede reemplazar sin reflashear el firmware
 * (pensado para que a futuro un portal web la edite).
 */
#pragma once

#include <Arduino.h>

#define UID_MAX_LEN 10

// Carga /config.json desde LittleFS a las tablas en memoria. Si LittleFS no
// monta o el archivo falta/esta corrupto, imprime el error por serie y deja
// las tablas vacias -- no cuelga el arranque.
bool cargarConfiguracion();

// Busca un UID en la tabla de personajes. Devuelve el id (>0) o 0 si no
// coincide con ningun personaje conocido.
int personajeDeUID(const uint8_t *uid, uint8_t len);

// Busca la pista asociada a una pareja de personajes. El orden no importa
// (se normaliza internamente). Devuelve la pista (>0) o 0 si esa
// combinacion no esta definida en historias.
int pistaDePersonajes(int idA, int idB);

// Pista del audio "personaje solitario" especifico de ese personaje (segunda
// etapa de la espera, mas larga que el audio generico). Devuelve 0 si el
// personaje no tiene pistaSolo configurada en config.json.
int pistaSolitariaDePersonaje(int id);

// Nombre legible de un personaje por id, para los mensajes de serie.
const char *nombreDePersonaje(int id);
