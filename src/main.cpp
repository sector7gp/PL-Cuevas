/*
 * ESP32-S3 DevKit (OLIMEX ESP32-S3-DevKit-Lipo) + 2x PN532 por I2C
 * + DFPlayer Mini por UART1 -- sistema de historias por combinacion
 *
 * Cada uno de los 3 tags fisicos representa un "personaje". Cuando dos
 * personajes validos quedan presentes a la vez (uno en cada lector, en
 * cualquier orden), se busca la historia asociada a esa pareja en
 * /config.json (LittleFS) y se dispara la pista correspondiente por el
 * DFPlayer. Sacar y volver a poner la misma combinacion la repite -- el
 * disparo es por flanco: se resetea apenas cualquiera de los dos lectores
 * queda vacio, no bloquea mientras ambos siguen puestos.
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
 * el firmware -- ver src/config.h.
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
 *   Lector 2   VCC -> 3V3   GND -> GND   SDA -> GPIO 11  SCL -> GPIO 12
 *   DFPlayer   VCC -> 5V    GND -> GND   TX  -> GPIO 18  RX  -> GPIO 17
 *              (TX del DFPlayer al RX del ESP, RX del DFPlayer al TX del ESP
 *              -- cruzados, como cualquier conexion serie punto a punto)
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

// --- Lector 1: Wire (I2C0) ---------------------------------------------------
#define SDA_1 8
#define SCL_1 9
#define IRQ_1 4
#define RESET_1 5

// --- Lector 2: Wire1 (I2C1) --------------------------------------------------
#define SDA_2 11
#define SCL_2 12
#define IRQ_2 14
#define RESET_2 15

// --- DFPlayer Mini: Serial1 (UART1), 9600 baudios ---------------------------
#define DFPLAYER_RX 18 // ESP recibe  <- TX del DFPlayer
#define DFPLAYER_TX 17 // ESP transmite -> RX del DFPlayer

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
    Serial.printf("%s: no se encuentra el PN532 en el bus.\n", nombre);
    return false;
  }

  Serial.printf("%s: Chip PN532 detectado. Firmware v%d.%d\n", nombre,
                (int)((versiondata >> 16) & 0xFF), (int)((versiondata >> 8) & 0xFF));

  pn532.SAMConfig();
  return true;
}

static bool lector1_ok = false;
static bool lector2_ok = false;
static bool dfPlayer_ok = false;

// Estado de "que personaje hay puesto" por lector, con el debounce de
// ausencia descripto arriba.
struct EstadoLector {
  int personajeId = 0; // 0 = nada detectado / no reconocido
  uint8_t fallosSeguidos = 0;
};

static EstadoLector estado1;
static EstadoLector estado2;

// true mientras la combinacion actual ya disparo su historia. Se resetea en
// cuanto cualquiera de los dos lectores queda vacio, asi sacar y volver a
// poner los mismos dos personajes dispara la historia de nuevo.
static bool comboYaDisparada = false;

// Si un personaje queda solo (el otro lector vacio) mas de este tiempo,
// suena su audio de "personaje solitario" (pistaSolo en config.json).
#define TIEMPO_SOLITARIO_MS 10000

static unsigned long soloDesde = 0; // millis() en que quedo solo; 0 = no aplica
static bool soloYaDisparado = false; // ya sono el audio para este episodio de soledad

static void imprimirUID(const uint8_t *uid, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    if (uid[i] < 0x10) {
      Serial.print("0");
    }
    Serial.print(uid[i], HEX);
    Serial.print(" ");
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
    if (id != estado.personajeId) {
      estado.personajeId = id;
      if (id != 0) {
        Serial.printf("%s: personaje detectado -> %s\n", nombre, nombreDePersonaje(id));
      } else {
        Serial.printf("%s: tag no reconocido, UID: ", nombre);
        imprimirUID(uid, uidLength);
        Serial.println();
      }
    }
    return;
  }

  estado.fallosSeguidos++;
  if (estado.fallosSeguidos >= FALLOS_PARA_AUSENCIA && estado.personajeId != 0) {
    Serial.printf("%s: personaje retirado.\n", nombre);
    estado.personajeId = 0;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
  }
  Serial.println("Inicializando sistema de historias...");

  cargarConfiguracion();

  Serial.println("Inicializando lectores PN532 por I2C (2 buses independientes)...");

  Wire.begin(SDA_1, SCL_1);
  Wire.setTimeOut(100);
  lector1_ok = initLector(pn532_1, "Lector 1");

  Wire1.begin(SDA_2, SCL_2);
  Wire1.setTimeOut(100);
  lector2_ok = initLector(pn532_2, "Lector 2");

  if (!lector1_ok && !lector2_ok) {
    Serial.println("Ningun lector encontrado. Revisa alimentacion, cableado");
    Serial.println("SDA/SCL y el modo I2C de cada modulo.");
    while (1) {
    }
  }
  Serial.println("Esperando personajes...");

  // Serial1 (UART1) para el DFPlayer, independiente de Serial (UART0, el
  // monitor). Igual que los lectores: si no responde, no se cuelga el resto.
  Serial1.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
  if (dfPlayer.begin(Serial1, /*isACK=*/true, /*doReset=*/true)) {
    dfPlayer_ok = true;
    dfPlayer.volume(25); // 0-30
    Serial.println("DFPlayer Mini detectado.");
  } else {
    Serial.println("DFPlayer Mini: no responde por Serial1 (GPIO17/18).");
  }
}

void loop() {
  actualizarLector(pn532_1, lector1_ok, estado1, "Lector 1");
  actualizarLector(pn532_2, lector2_ok, estado2, "Lector 2");

  bool p1 = (estado1.personajeId != 0);
  bool p2 = (estado2.personajeId != 0);
  bool ambosPresentes = p1 && p2;
  bool exactamenteUno = p1 != p2; // XOR: uno puesto, el otro lector vacio

  if (ambosPresentes && !comboYaDisparada) {
    int pista = pistaDePersonajes(estado1.personajeId, estado2.personajeId);
    if (pista != 0) {
      Serial.printf(">>> Historia: %s + %s -> pista %04d (/mp3/)\n",
                    nombreDePersonaje(estado1.personajeId),
                    nombreDePersonaje(estado2.personajeId), pista);
      if (dfPlayer_ok) {
        dfPlayer.playMp3Folder(pista);
      }
    } else {
      Serial.printf(">>> %s + %s: combinacion sin historia asociada.\n",
                    nombreDePersonaje(estado1.personajeId),
                    nombreDePersonaje(estado2.personajeId));
    }
    comboYaDisparada = true;
  } else if (!ambosPresentes) {
    comboYaDisparada = false;
  }

  // Personaje solitario: si pasan TIEMPO_SOLITARIO_MS con exactamente un
  // lector ocupado, suena su audio de espera. Se resetea (y puede volver a
  // disparar) apenas deja de haber exactamente uno -- se empareja, o se saca.
  if (exactamenteUno) {
    if (soloDesde == 0) {
      soloDesde = millis();
      soloYaDisparado = false;
    } else if (!soloYaDisparado && (millis() - soloDesde >= TIEMPO_SOLITARIO_MS)) {
      int idSolo = p1 ? estado1.personajeId : estado2.personajeId;
      int pista = pistaSolitariaDePersonaje(idSolo);
      if (pista != 0) {
        Serial.printf(">>> %s solo hace %lu s -> pista %04d (/mp3/)\n", nombreDePersonaje(idSolo),
                      TIEMPO_SOLITARIO_MS / 1000, pista);
        if (dfPlayer_ok) {
          dfPlayer.playMp3Folder(pista);
        }
      } else {
        Serial.printf(">>> %s solo hace %lu s: sin pistaSolo configurada.\n",
                      nombreDePersonaje(idSolo), TIEMPO_SOLITARIO_MS / 1000);
      }
      soloYaDisparado = true;
    }
  } else {
    soloDesde = 0;
    soloYaDisparado = false;
  }
}
