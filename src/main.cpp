/*
 * ESP32-S3 DevKit (OLIMEX ESP32-S3-DevKit-Lipo) + 2x PN532 por I2C
 * + DFPlayer Mini por UART1 -- sistema de historias por combinacion
 * + portal web de monitoreo/configuracion (AP propio, ver src/portal.h)
 *
 * Cada tag fisico representa un "personaje" (tabla en config.json). Cuando
 * dos personajes reconocidos quedan presentes a la vez (uno en cada lector,
 * en cualquier orden), se busca la historia asociada a esa pareja y se
 * dispara la pista correspondiente por el DFPlayer. Sacar y volver a poner
 * la misma combinacion la repite -- el disparo es por flanco: se resetea
 * apenas cualquiera de los dos lectores deja de tener un personaje
 * reconocido, no bloquea mientras ambos siguen puestos.
 *
 * Si un tag no esta en config.json, se imprime su UID por serie/portal (para
 * darlo de alta) en vez de intentar formar una combinacion con el.
 *
 * Si queda un solo lector ocupado (personaje reconocido o no), la espera
 * tiene dos etapas: a TIEMPO_ESPERAR_MS suena PISTA_ESPERAR (un unico audio
 * generico, igual sin importar cual personaje quedo solo). Si sigue solo
 * hasta TIEMPO_SOLITARIO_MS, suena el pistaSolo especifico de ESE personaje
 * (config.json), si tiene uno configurado.
 *
 * El audio se dispara con playMp3Folder(), no play(): el comando nativo
 * play() del DFPlayer reproduce por posicion FISICA en la tabla FAT de la
 * SD, no por el numero en el nombre del archivo -- si los mp3 no se
 * copiaron en orden numerico estricto, play(1) puede sonar cualquier
 * archivo. playMp3Folder() si busca por el numero del nombre, pero exige
 * que los archivos esten en una carpeta /mp3/ en la raiz de la SD,
 * nombrados con 4 digitos al principio (0001xxx.mp3, 0002xxx.mp3, ...).
 *
 * La tabla de personajes (UID->id) e historias (personajes->pista) vive en
 * data/config.json, subido aparte con `pio run -t uploadfs`, no compilada en
 * el firmware -- ver src/config.h. El portal web (src/portal.h) sirve una UI
 * en /www/index.html (misma carpeta data/, mismo uploadfs) para editarla sin
 * SSH ni recompilar, mas un modal de ajustes (hostname/volumen, en
 * settings.json via src/settings.h) y un monitor de actividad en vivo que
 * lee del mismo buffer que usa el logger (src/log.h) para el monitor serie.
 * El portal corre en un Access Point propio (ver src/portal.cpp) y se apaga
 * solo a los 5 minutos del boot.
 *
 * Cada lector va en su propio periferico I2C de hardware (Wire y Wire1): son
 * dos buses fisicamente independientes, no un bus compartido con dos
 * direcciones. Los dos chips pueden estar en 0x24 (la fija del PN532) sin
 * colisionar porque nunca comparten lineas.
 *
 * El ESP32-S3 tiene 3 UART de hardware. UART0 (Serial) sale por el puente
 * USB-serie de la placa y es el monitor de la PC -- no se toca. El DFPlayer
 * usa UART1 (Serial1) en pines aparte, un bus totalmente independiente.
 *
 * Cableado:
 *   Lector 1   VCC -> 3V3   GND -> GND   SDA -> GPIO 8   SCL -> GPIO 9
 *   Lector 2   VCC -> 3V3   GND -> GND   SDA -> GPIO 12  SCL -> GPIO 11
 *   DFPlayer   VCC -> 5V    GND -> GND   TX  -> GPIO 17  RX  -> GPIO 18
 *              (TX del DFPlayer al RX del ESP, RX del DFPlayer al TX del ESP
 *              -- cruzados, como cualquier conexion serie punto a punto.
 *              Nota: la placa designa GPIO17/18 como su TX1/RX1 "oficial";
 *              aca van al reves -- 17 es RX del ESP, 18 es TX -- porque
 *              conviene asi en el PCB. Sigue siendo UART1/Serial1 igual: el
 *              ESP32 mapea los pines de UART por matriz de software, no por
 *              asignacion fija de hardware.)
 *
 * GPIO 5 y 6 en esta placa estan cableados a sensado de bateria LiPo
 * (PWR_SENSE / BAT_SENSE): evitarlos. GPIO 11/12/17/18 son pines del header
 * pUEXT opcional -- sin nada soldado por defecto, libres para I2C/UART.
 *
 * Cada PN532 debe estar en modo I2C por hardware (DIP switches, jumper, o el
 * mecanismo que use tu placa concreta -- no todas usan el mismo esquema).
 */

#include <Adafruit_PN532.h>
#include <Arduino.h>
#include <DFRobotDFPlayerMini.h>
#include <Wire.h>

#include "config.h"
#include "log.h"
#include "portal.h"
#include "settings.h"

// --- Lector 1: Wire (I2C0) ---------------------------------------------------
#define SDA_1 8
#define SCL_1 9
#define IRQ_1 4
#define RESET_1 5

// --- Lector 2: Wire1 (I2C1) --------------------------------------------------
#define SCL_2 11
#define SDA_2 12
#define IRQ_2 14
#define RESET_2 15

// --- DFPlayer Mini: Serial1 (UART1), 9600 baudios ---------------------------
#define DFPLAYER_RX 17 // ESP recibe  <- TX del DFPlayer
#define DFPLAYER_TX 18 // ESP transmite -> RX del DFPlayer

// Timeout corto en readPassiveTargetID(): sin esto, la libreria usa
// timeout=0 ("bloquear para siempre" -- ver Adafruit_PN532.h). Con dos
// lectores independientes, si el Lector 1 no tiene tag puesto y bloqueara
// para siempre, el Lector 2 nunca se llegaria a consultar.
#define TIMEOUT_LECTURA_MS 50

// Lecturas fallidas seguidas antes de considerar que un tag se retiro. Sin
// este debounce, un fallo de lectura transitorio (el tag sigue puesto pero
// una lectura puntual no salio bien) se confundiria con un retiro real y
// reiniciaria el flanco de la historia sin motivo.
#define FALLOS_PARA_AUSENCIA 3

Adafruit_PN532 pn532_1(IRQ_1, RESET_1, &Wire);
Adafruit_PN532 pn532_2(IRQ_2, RESET_2, &Wire1);
DFRobotDFPlayerMini dfPlayer;

// Inicializa un lector: misma secuencia que ejemplo_ok.ino, sin nada que
// toque el bus antes de begin(). No cuelga si no aparece -- devuelve false y
// el otro lector sigue andando igual.
static bool initLector(Adafruit_PN532 &pn532, const char *nombre) {
  pn532.begin();
  uint32_t versiondata = pn532.getFirmwareVersion();
  if (!versiondata) {
    logf("%s: no se encuentra el PN532 en el bus.", nombre);
    return false;
  }

  logf("%s: Chip PN532 detectado. Firmware v%d.%d", nombre, (int)((versiondata >> 16) & 0xFF),
       (int)((versiondata >> 8) & 0xFF));

  pn532.SAMConfig();
  return true;
}

static bool lector1_ok = false;
static bool lector2_ok = false;
static bool dfPlayer_ok = false;

// Estado de "que personaje hay puesto" por lector, con el debounce de
// ausencia descripto abajo. 0 y -1 son sentinelas distintos a proposito: sin
// esa distincion, "lector vacio" y "tag presente pero no reconocido" son
// indistinguibles (los dos valdrian 0), y el aviso de "tag no reconocido"
// nunca se dispara para el primer tag desconocido que aparece tras bootear.
#define PERSONAJE_VACIO 0
#define PERSONAJE_DESCONOCIDO -1

struct EstadoLector {
  int personajeId = PERSONAJE_VACIO;
  uint8_t fallosSeguidos = 0;
};

static EstadoLector estado1;
static EstadoLector estado2;

// true mientras la combinacion actual ya disparo su historia. Se resetea en
// cuanto cualquiera de los dos lectores deja de tener un personaje
// reconocido, asi sacar y volver a poner los mismos dos personajes dispara
// la historia de nuevo.
static bool comboYaDisparada = false;

// Espera en dos etapas cuando queda un solo lector ocupado (personaje
// reconocido o no): primero un audio generico ("pone el otro personaje"),
// y si sigue solo mas tiempo, el audio "solitario" especifico de ESE
// personaje (pistaSolo en config.json -- solo aplica si es reconocido).
#define TIEMPO_ESPERAR_MS 2000    // dispara PISTA_ESPERAR (generico, cualquiera)
#define TIEMPO_SOLITARIO_MS 10000 // dispara pistaSolo especifico del personaje
#define PISTA_ESPERAR 8

static unsigned long soloDesde = 0;     // millis() en que quedo solo; 0 = no aplica
static bool esperarYaDisparado = false; // ya sono el audio generico de espera
static bool soloYaDisparado = false;    // ya sono el audio especifico de soledad

// Construye la representacion hex del UID en un buffer propio -- a
// diferencia de Serial.print() encadenado, logf() necesita la linea
// completa de una sola vez para poder guardarla en el buffer circular.
static void formatearUID(const uint8_t *uid, uint8_t len, char *out, size_t outLen) {
  size_t pos = 0;
  for (uint8_t i = 0; i < len && pos + 3 < outLen; i++) {
    pos += snprintf(out + pos, outLen - pos, "%02X ", uid[i]);
  }
}

// Consulta un lector y actualiza su EstadoLector. No bloquea gracias al
// timeout corto -- se puede llamar a los dos lectores en cada vuelta de
// loop() sin que uno le robe tiempo al otro.
static void actualizarLector(Adafruit_PN532 &pn532, bool activo, EstadoLector &estado,
                             const char *nombre) {
  if (!activo) {
    return;
  }

  uint8_t uid[7] = {0}; // 4 bytes (MIFARE Classic) o 7 (Ultralight/NTAG)
  uint8_t uidLength = 0;

  if (pn532.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength,
                                TIMEOUT_LECTURA_MS)) {
    estado.fallosSeguidos = 0;
    int id = personajeDeUID(uid, uidLength);
    int nuevoEstado = (id != 0) ? id : PERSONAJE_DESCONOCIDO;
    if (nuevoEstado != estado.personajeId) {
      estado.personajeId = nuevoEstado;
      if (id != 0) {
        logf("%s: personaje detectado -> %s", nombre, nombreDePersonaje(id));
      } else {
        char uidTexto[32];
        formatearUID(uid, uidLength, uidTexto, sizeof(uidTexto));
        logf("%s: tag NO reconocido, UID: %s(agregalo a config.json)", nombre, uidTexto);
      }
    }
    return;
  }

  estado.fallosSeguidos++;
  if (estado.fallosSeguidos >= FALLOS_PARA_AUSENCIA && estado.personajeId != PERSONAJE_VACIO) {
    logf("%s: personaje retirado.", nombre);
    estado.personajeId = PERSONAJE_VACIO;
  }
}

// Puente hacia el portal: le permite aplicar un volumen nuevo al DFPlayer
// sin que src/portal.cpp tenga que incluir la libreria del DFPlayer.
static void aplicarVolumen(uint8_t v) {
  if (dfPlayer_ok) {
    dfPlayer.volume(v);
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
  }
  logln("Inicializando sistema de historias...");

  cargarConfiguracion();
  Settings settings = cargarSettings();

  logln("Inicializando lectores PN532 por I2C (2 buses independientes)...");

  Wire.begin(SDA_1, SCL_1);
  Wire.setTimeOut(100);
  lector1_ok = initLector(pn532_1, "Lector 1");

  Wire1.begin(SDA_2, SCL_2);
  Wire1.setTimeOut(100);
  lector2_ok = initLector(pn532_2, "Lector 2");

  if (!lector1_ok && !lector2_ok) {
    logln("Ningun lector encontrado. Revisa alimentacion, cableado SDA/SCL y");
    logln("el modo I2C de cada modulo.");
    while (1) {
    }
  }
  logln("Esperando personajes...");

  // Serial1 (UART1) para el DFPlayer, independiente de Serial (UART0, el
  // monitor). Igual que los lectores: si no responde, no se cuelga el resto.
  Serial1.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
  if (dfPlayer.begin(Serial1, /*isACK=*/true, /*doReset=*/true)) {
    dfPlayer_ok = true;
    dfPlayer.volume(settings.volumen); // 0-30, valor de settings.json
    logf("DFPlayer Mini detectado. Volumen %u/30.", settings.volumen);
  } else {
    logln("DFPlayer Mini: no responde por Serial1 (GPIO17/18).");
  }

  iniciarPortal(aplicarVolumen);
}

void loop() {
  actualizarPortal();

  actualizarLector(pn532_1, lector1_ok, estado1, "Lector 1");
  actualizarLector(pn532_2, lector2_ok, estado2, "Lector 2");

  bool p1Vacio = (estado1.personajeId == PERSONAJE_VACIO);
  bool p2Vacio = (estado2.personajeId == PERSONAJE_VACIO);
  bool p1Conocido = (estado1.personajeId > 0);
  bool p2Conocido = (estado2.personajeId > 0);

  // Solo dispara historia si los DOS lados son personajes reconocidos -- un
  // tag no reconocido no forma combinacion valida con nada.
  bool ambosConocidos = p1Conocido && p2Conocido;
  // Para el timer de "esperando al otro" cuenta cualquier cosa puesta, sea
  // reconocida o no -- fisicamente hay algo en ese lector de todos modos.
  bool exactamenteUnoPresente = (!p1Vacio) != (!p2Vacio); // XOR

  if (ambosConocidos && !comboYaDisparada) {
    int pista = pistaDePersonajes(estado1.personajeId, estado2.personajeId);
    if (pista != 0) {
      logf(">>> Historia: %s + %s -> pista %04d (/mp3/)", nombreDePersonaje(estado1.personajeId),
           nombreDePersonaje(estado2.personajeId), pista);
      if (dfPlayer_ok) {
        dfPlayer.playMp3Folder(pista);
      }
    } else {
      logf(">>> %s + %s: combinacion sin historia asociada.", nombreDePersonaje(estado1.personajeId),
           nombreDePersonaje(estado2.personajeId));
    }
    comboYaDisparada = true;
  } else if (!ambosConocidos) {
    comboYaDisparada = false;
  }

  // Espera en dos etapas cuando queda exactamente un lector ocupado
  // (reconocido o no). A TIEMPO_ESPERAR_MS suena PISTA_ESPERAR (generico,
  // igual para cualquiera). Si sigue solo hasta TIEMPO_SOLITARIO_MS, suena
  // el pistaSolo especifico de ESE personaje (si tiene uno configurado). Las
  // dos etapas se resetean juntas apenas deja de haber exactamente uno --
  // se empareja, se saca, o llega un segundo tag.
  if (exactamenteUnoPresente) {
    if (soloDesde == 0) {
      soloDesde = millis();
      esperarYaDisparado = false;
      soloYaDisparado = false;
    }
    unsigned long transcurrido = millis() - soloDesde;

    if (!esperarYaDisparado && transcurrido >= TIEMPO_ESPERAR_MS) {
      logf(">>> Solo hace %lu s -> pista %04d (/mp3/) [generico]", TIEMPO_ESPERAR_MS / 1000,
           PISTA_ESPERAR);
      if (dfPlayer_ok) {
        dfPlayer.playMp3Folder(PISTA_ESPERAR);
      }
      esperarYaDisparado = true;
    }

    if (!soloYaDisparado && transcurrido >= TIEMPO_SOLITARIO_MS) {
      int idSolo = p1Vacio ? estado2.personajeId : estado1.personajeId;
      int pista = pistaSolitariaDePersonaje(idSolo);
      if (pista != 0) {
        logf(">>> %s sigue solo hace %lu s -> pista %04d (/mp3/)", nombreDePersonaje(idSolo),
             TIEMPO_SOLITARIO_MS / 1000, pista);
        if (dfPlayer_ok) {
          dfPlayer.playMp3Folder(pista);
        }
      } else {
        logf(">>> %s sigue solo hace %lu s: sin pistaSolo configurada.", nombreDePersonaje(idSolo),
             TIEMPO_SOLITARIO_MS / 1000);
      }
      soloYaDisparado = true;
    }
  } else {
    soloDesde = 0;
    esperarYaDisparado = false;
    soloYaDisparado = false;
  }
}
