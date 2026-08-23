/*
 * ESP32-S3 DevKit (OLIMEX ESP32-S3-DevKit-Lipo) + 2x PN532 por I2C
 * + DFPlayer Mini por UART1
 *
 * Basado directamente en ejemplo_ok.ino, el sketch minimo verificado
 * funcionando contra el PN532 real en este hardware. Misma secuencia de
 * arranque para cada lector, sin nada que toque el bus antes de begin().
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

// Lee un lector si esta activo: imprime el UID y, si hay DFPlayer, reproduce
// la pista asociada. play(1) reproduce 0001.mp3 en la raiz de la SD,
// play(2) reproduce 0002.mp3, etc. -- la numeracion de archivos del DFPlayer
// es la del propio archivo, no un indice arbitrario.
static void probarLector(Adafruit_PN532 &pn532, bool activo, const char *nombre,
                         int pista) {
  if (!activo) {
    return;
  }
  uint8_t uid[7] = {0}; // 4 bytes (MIFARE Classic) o 7 (Ultralight/NTAG)
  uint8_t uidLength = 0;

  if (pn532.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
    Serial.printf("%s: Tarjeta detectada! UID: ", nombre);
    for (uint8_t i = 0; i < uidLength; i++) {
      if (uid[i] < 0x10) {
        Serial.print("0");
      }
      Serial.print(uid[i], HEX);
      Serial.print(" ");
    }
    Serial.println();

    if (dfPlayer_ok) {
      dfPlayer.play(pista);
      Serial.printf("  -> reproduciendo pista %04d.mp3\n", pista);
    }

    delay(1000); // no repetir la misma tarjeta decenas de veces
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
  }
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
  Serial.println("Esperando tarjeta...");

  // Serial1 (UART1) para el DFPlayer, independiente de Serial (UART0, el
  // monitor). Igual que los lectores: si no responde, no se cuelga el resto.
  Serial1.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
  if (dfPlayer.begin(Serial1, /*isACK=*/true, /*doReset=*/true)) {
    dfPlayer_ok = true;
    dfPlayer.volume(15); // 0-30
    Serial.println("DFPlayer Mini detectado.");
  } else {
    Serial.println("DFPlayer Mini: no responde por Serial1 (GPIO17/18).");
  }
}

void loop() {
  probarLector(pn532_1, lector1_ok, "Lector 1", 1); // 0001.mp3
  probarLector(pn532_2, lector2_ok, "Lector 2", 2); // 0002.mp3
}
