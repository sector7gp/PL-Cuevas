/*
 * Carga de firmware por red (OTA), sobre el mismo Access Point que ya levanta
 * el portal -- no necesita router ni internet, solo estar conectado al AP.
 *
 * Depende de que el WiFi este arriba, asi que iniciarOTA() va DESPUES de
 * iniciarPortal() en setup(). Reusa el mismo hostname de settings.json, asi
 * que el equipo se sube igual que se lo monitorea: cueva1.local.
 *
 * La tabla de particiones de la placa (default_8MB.csv) ya trae otadata mas
 * app0/app1 de 3,34 MB cada una, asi que esto no requiere reparticionar: el
 * firmware nuevo se escribe en la particion inactiva y recien al terminar
 * bien se conmuta el arranque. Una carga cortada a la mitad no deja el equipo
 * inservible -- sigue arrancando el firmware viejo.
 *
 * Ojo con el huevo-y-gallina: para que un ESP acepte OTA primero hay que
 * flashearle POR CABLE una version que ya incluya este modulo. De ahi en mas
 * se actualiza por red.
 */
#pragma once

// Registra los callbacks y empieza a escuchar (puerto 3232). Llamar una vez
// en setup(), despues de iniciarPortal(): necesita el WiFi ya levantado.
void iniciarOTA(const char *hostname);

// Atiende una transferencia en curso. Llamar en cada vuelta de loop().
void actualizarOTA();

// true mientras hay una carga OTA en curso. main.cpp lo consulta para frenar
// el resto de loop() mientras dura -- ver el comentario en loop().
bool otaEnProgreso();
