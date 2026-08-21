/*
 * Prueba MFRC522 por SPI — ESP32-S3
 *
 * Entorno de diagnostico independiente: mismo chip, misma libreria, pero por
 * SPI en vez de I2C. Sirve para descartar de una vez que el problema tenga
 * algo que ver con el bus I2C (timing, drivers, lo que sea): SPI es la ruta
 * original y mas probada de esta libreria, y con un pin RST real conseguimos
 * algo que el modo I2C nunca tuvo en esta sesion — un reset por hardware
 * autentico en cada arranque (con RC522_RST=UNUSED_PIN el chip solo vio el
 * power-on-reset al enchufarlo).
 *
 * Cableado (header inferior del modulo, no el lateral I2C):
 *   VCC -> 3V3      SS   -> GPIO 10     MISO -> GPIO 13
 *   GND -> GND      MOSI -> GPIO 11     SCK  -> GPIO 12
 *   RST -> GPIO 9   IRQ  -> sin conectar
 *
 *   pio run -e spi_test -t upload -t monitor
 */

#include <Arduino.h>
#include <MFRC522.h>
#include <SPI.h>

#define PIN_SS 10
#define PIN_MOSI 11
#define PIN_MISO 13
#define PIN_SCK 12
#define PIN_RST 9

MFRC522_SPI rc522bus(PIN_SS, PIN_RST, &SPI);
MFRC522 rc522(&rc522bus);

// Los pasos de ganancia del MFRC522 no son lineales ni monotonos: 010 y 011
// repiten los valores de 000 y 001 (tabla 98 del datasheet). Es ganancia de
// RECEPTOR, no de transmision: el MFRC522 no tiene potencia de TX ajustable,
// los drivers TX1/TX2 son on/off. Si el campo emitido es debil, esto no lo
// arregla — solo sirve para no perder respuestas debiles de vuelta del tag.
static uint8_t gananciaDb(uint8_t rfcfg) {
  static const uint8_t DB[8] = {18, 23, 18, 23, 33, 38, 43, 48};
  return DB[(rfcfg >> 4) & 0x07];
}

static const char *nombreVersion(uint8_t v) {
  switch (v) {
  case 0x88:
    return "clon FM17522";
  case 0x90:
    return "MFRC522 v0.0";
  case 0x91:
    return "MFRC522 v1.0 (NXP)";
  case 0x92:
    return "MFRC522 v2.0 (NXP)";
  case 0xB2:
    return "clon FM17522E";
  default:
    return "desconocido";
  }
}

// Misma comprobacion que en el firmware I2C: si alguno de estos registros no
// tiene el valor debido tras PCD_Init(), las tarjetas no contestan nunca. El
// mas critico es TxASKReg (Force100ASK).
static void verificarConfigISO14443A() {
  struct {
    uint8_t reg;
    uint8_t esperado;
    const char *nombre;
  } req[] = {
      {0x11, 0x3D, "ModeReg"},      {0x12, 0x00, "TxModeReg"},
      {0x13, 0x00, "RxModeReg"},    {0x14, 0x83, "TxControlReg"},
      {0x15, 0x40, "TxASKReg"},     {0x24, 0x26, "ModWidthReg"},
      {0x2A, 0x80, "TModeReg"},     {0x2B, 0xA9, "TPrescalerReg"},
      {0x2C, 0x03, "TReloadRegH"},  {0x2D, 0xE8, "TReloadRegL"},
  };

  Serial.println("  configuracion ISO14443A:");
  uint8_t mal = 0;
  for (uint8_t i = 0; i < sizeof(req) / sizeof(req[0]); i++) {
    uint8_t v = rc522.PCD_ReadRegister((MFRC522::PCD_Register)req[i].reg);
    bool ok = (v == req[i].esperado);
    Serial.printf("    %-14s (0x%02X) = 0x%02X  esperado 0x%02X  %s\n",
                  req[i].nombre, req[i].reg, v, req[i].esperado,
                  ok ? "ok" : "<-- MAL");
    if (!ok) {
      mal++;
    }
  }
  Serial.println(mal == 0 ? "    => configuracion correcta."
                           : "    => hay registros mal configurados.");
}

static void printUID(const MFRC522::Uid &uid) {
  Serial.printf("Tarjeta detectada  |  UID (%u bytes): ", uid.size);
  for (uint8_t i = 0; i < uid.size; i++) {
    Serial.printf("%02X", uid.uidByte[i]);
    if (i + 1 < uid.size) {
      Serial.print(':');
    }
  }
  if (uid.size == 4) {
    uint32_t valor = ((uint32_t)uid.uidByte[0] << 24) |
                     ((uint32_t)uid.uidByte[1] << 16) |
                     ((uint32_t)uid.uidByte[2] << 8) | uid.uidByte[3];
    Serial.printf("   (%lu)", (unsigned long)valor);
  }
  Serial.print("   tipo: ");
  Serial.println(MFRC522::PICC_GetTypeName(MFRC522::PICC_GetType(uid.sak)));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== ESP32-S3 + RC522 por SPI ===");
  Serial.printf("SS=%d MOSI=%d MISO=%d SCK=%d RST=%d\n", PIN_SS, PIN_MOSI,
                PIN_MISO, PIN_SCK, PIN_RST);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);

  // Con RST en un GPIO real (no UNUSED_PIN), PCD_Init() hace un reset por
  // hardware autentico: pulsa el pin y espera 50ms al arranque del cristal.
  rc522.PCD_Init();
  delay(50);

  uint8_t v = rc522.PCD_ReadRegister(MFRC522::VersionReg);
  if (v == 0x00 || v == 0xFF) {
    Serial.println("No responde nada por SPI. Revisa MOSI/MISO/SCK/SS/RST.");
    while (true) {
      delay(1000);
    }
  }

  Serial.printf("Chip encontrado. VersionReg=0x%02X (%s)\n", v,
                nombreVersion(v));

  rc522.PCD_SetAntennaGain(MFRC522::RxGain_max);
  rc522.PCD_AntennaOff();
  rc522.PCD_AntennaOn();

  uint8_t tx = rc522.PCD_ReadRegister(MFRC522::TxControlReg);
  Serial.printf("antena: TxControlReg=0x%02X (%s)\n", tx,
                (tx & 0x03) == 0x03 ? "emitiendo" : "APAGADA");

  verificarConfigISO14443A();
  Serial.println("\nListo. Acerca una tarjeta...\n");
}

void loop() {
  static uint32_t ultimoLatido = 0;
  static uint8_t gan = 0;

  if (millis() - ultimoLatido > 3000) {
    ultimoLatido = millis();
    gan = (gan + 1) & 0x07;
    rc522.PCD_SetAntennaGain(gan << 4);
    rc522.PCD_AntennaOff();
    rc522.PCD_AntennaOn();

    byte atqa[2];
    byte n = sizeof(atqa);
    rc522.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
    rc522.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
    rc522.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);
    MFRC522::StatusCode st = rc522.PICC_WakeupA(atqa, &n);

    Serial.printf("[%lus] ganancia %u/7 (%u dB), WUPA -> ",
                  (unsigned long)(millis() / 1000), gan, gananciaDb(gan << 4));
    Serial.print(MFRC522::GetStatusCodeName(st));
    if (st == MFRC522::STATUS_OK || st == MFRC522::STATUS_COLLISION) {
      Serial.printf("  ATQA=%02X%02X", atqa[1], atqa[0]);
    }
    Serial.println();
  }

  if (!rc522.PICC_IsNewCardPresent() || !rc522.PICC_ReadCardSerial()) {
    return;
  }
  printUID(rc522.uid);
  rc522.PICC_HaltA();
  rc522.PCD_StopCrypto1();
}
