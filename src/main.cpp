/*
 * ESP32-S3 DevKit + lector RFID por I2C
 *
 * Detecta automaticamente que lector hay en el bus y lee el UID de tarjetas
 * ISO14443A (MIFARE Classic, Ultralight, NTAG...), volcandolo por serie:
 *
 *   PN532  -> direccion 0x24, protocolo por frames
 *   RC522  -> direccion 0x28 (o 0x3C), registros "escribe indice, lee byte"
 *
 * Cableado (igual para los dos):
 *   VCC -> 3V3      SDA -> GPIO 6  (I2C_SDA)
 *   GND -> GND      SCL -> GPIO 7  (I2C_SCL)
 *
 * Cada modulo necesita estar puesto en modo I2C por hardware:
 *   PN532 (Elechouse V3): DIP switches SEL0 = ON, SEL1 = OFF
 *   RC522: pin EA a masa en la mayoria de clones
 *
 * Si no aparece ningun lector el firmware NO se cuelga: reintenta cada 2 s.
 */

#include <Adafruit_PN532.h>
#include <Arduino.h>
#include <MFRC522.h>
#include <Wire.h>

// --- Configuracion ----------------------------------------------------------
#define I2C_SDA 6
#define I2C_SCL 7
#define I2C_FREQ 100000

#define PN532_ADDR 0x24
#define RC522_ADDR 0x28

// El PN532 en modo I2C no necesita IRQ ni RESET cableados: la libreria consulta
// el estado por el propio bus. Aun asi el constructor hace pinMode() sobre
// ambos, asi que hay que darle GPIO validos y libres.
#define PN532_IRQ 4
#define PN532_RESET 5

Adafruit_PN532 pn532(PN532_IRQ, PN532_RESET, &Wire);

// El RC522 puede resetearse por software, asi que el pin RST es opcional.
MFRC522_I2C rc522bus(UNUSED_PIN, RC522_ADDR, Wire);
MFRC522 rc522(&rc522bus);

enum Lector { NINGUNO, ES_PN532, ES_RC522 };
static Lector lector = NINGUNO;

// --- Diagnostico del bus ----------------------------------------------------

// Estado electrico del bus. Los tres casos se distinguen SOLO comparando la
// lectura sin pull-up interno con la lectura con el: un escaneo de direcciones
// no puede separarlos, y confundirlos manda a depurar el sitio equivocado.
enum EstadoBus {
  BUS_OK,      // pull-ups externos presentes: modulo alimentado
  BUS_AL_AIRE, // solo suben con el pull-up interno: sin alimentacion o sin cable
  BUS_SUJETO,  // no suben ni con pull-up: corto a masa o esclavo colgado
};

static EstadoBus chequeoLineas() {
  pinMode(I2C_SDA, INPUT); // sin pull-up interno: asi solo se ven los externos
  pinMode(I2C_SCL, INPUT);
  delay(5);
  bool sdaLibre = digitalRead(I2C_SDA);
  bool sclLibre = digitalRead(I2C_SCL);

  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delay(5);
  bool sdaPup = digitalRead(I2C_SDA);
  bool sclPup = digitalRead(I2C_SCL);

  pinMode(I2C_SDA, INPUT);
  pinMode(I2C_SCL, INPUT);

  Serial.printf("Lineas: sin pull-up SDA=%s SCL=%s | con pull-up SDA=%s SCL=%s\n",
                sdaLibre ? "H" : "L", sclLibre ? "H" : "L", sdaPup ? "H" : "L",
                sclPup ? "H" : "L");

  if (sdaLibre && sclLibre) {
    Serial.println("  -> pull-ups externos: modulo conectado y alimentado.");
    return BUS_OK;
  }
  if (sdaPup && sclPup) {
    // Los pull-ups del modulo van a VCC: si no tiran, es que no hay VCC.
    Serial.println("  -> lineas al aire. El modulo NO tiene alimentacion, o VCC");
    Serial.println("     o GND no llegan. No es un problema del bus de datos.");
    return BUS_AL_AIRE;
  }
  Serial.println("  -> alguna linea no sube ni con pull-up: corto a masa, o un");
  Serial.println("     esclavo sujetando el bus.");
  return BUS_SUJETO;
}

// Recuperacion de bus bloqueado. Si el master se resetea a mitad de una lectura
// el esclavo se queda a medio enviar un byte y mantiene SDA en LOW: el bus
// queda muerto y ningun escaneo encuentra nada. La cura estandar es pulsar SCL
// hasta que suelte SDA y cerrar con una condicion de STOP.
static void liberarBus() {
  // El pull-up interno se activa aqui a proposito: es lo que permite ver si el
  // esclavo suelta la linea. Para *decidir* si hace falta liberar se usa
  // chequeoLineas(), que mide sin pull-up.
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SCL, HIGH);
  delayMicroseconds(10);

  Serial.println("SDA en LOW: liberando el bus...");

  uint8_t pulsos = 0;
  while (digitalRead(I2C_SDA) == LOW && pulsos < 16) {
    digitalWrite(I2C_SCL, LOW);
    delayMicroseconds(10);
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(10);
    pulsos++;
  }

  // Condicion de STOP: SDA pasa de LOW a HIGH con SCL en HIGH.
  pinMode(I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SDA, LOW);
  delayMicroseconds(10);
  digitalWrite(I2C_SCL, HIGH);
  delayMicroseconds(10);
  digitalWrite(I2C_SDA, HIGH);
  delayMicroseconds(10);

  pinMode(I2C_SDA, INPUT);
  pinMode(I2C_SCL, INPUT);
  delay(5);

  bool libre = digitalRead(I2C_SDA) == HIGH;
  Serial.printf("  %u pulsos de SCL -> SDA %s\n", pulsos,
                libre ? "liberado" : "SIGUE atascado (revisa el cableado)");
}

// Devuelve la primera direccion conocida de lector que responda, o 0.
static uint8_t scanI2C() {
  uint8_t encontrados = 0;
  uint8_t lectorEn = 0;

  Serial.println("Escaneando bus I2C...");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) {
      continue;
    }
    const char *etiqueta = "";
    if (addr == PN532_ADDR) {
      etiqueta = "  <- PN532";
    } else if (addr == RC522_ADDR) {
      etiqueta = "  <- RC522";
    }
    Serial.printf("  dispositivo en 0x%02X%s\n", addr, etiqueta);
    encontrados++;
    if (etiqueta[0] != '\0' && lectorEn == 0) {
      lectorEn = addr;
    }
  }
  if (encontrados == 0) {
    Serial.println("  ninguno.");
  }
  return lectorEn;
}

// --- Identificacion del chip ------------------------------------------------

static uint8_t regLeer(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission();
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) {
    return 0xFF;
  }
  return Wire.read();
}

static void regEscribir(uint8_t addr, uint8_t reg, uint8_t valor) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(valor);
  Wire.endTransmission();
}

// Prueba decisiva: un MFRC522 tiene registros direccionables y devuelve lo que
// le escribas; un PN532 no los tiene, interpreta la escritura como un frame mal
// formado y nunca reproduce el valor.
static void identificarChip(uint8_t addr) {
  Serial.printf("\n--- Identificando el chip en 0x%02X ---\n", addr);

  const uint8_t REG = 0x2D; // TReloadRegL en MFRC522: R/W y sin efectos raros
  const uint8_t patrones[] = {0x5A, 0xA5, 0x0F};
  uint8_t original = regLeer(addr, REG);
  uint8_t aciertos = 0;

  for (uint8_t i = 0; i < sizeof(patrones); i++) {
    regEscribir(addr, REG, patrones[i]);
    delay(2);
    uint8_t v = regLeer(addr, REG);
    Serial.printf("  escribo 0x%02X en reg 0x%02X -> leo 0x%02X  %s\n",
                  patrones[i], REG, v, v == patrones[i] ? "OK" : "no coincide");
    if (v == patrones[i]) {
      aciertos++;
    }
  }
  regEscribir(addr, REG, original);

  Serial.printf("  VersionReg(0x37) = 0x%02X\n", regLeer(addr, 0x37));

  if (aciertos == sizeof(patrones)) {
    Serial.println("  => registros R/W: es un MFRC522/FM17522, NO un PN532.");
  } else {
    Serial.println("  => no se comporta como banco de registros.");
  }

  // Contraprueba: frame GetFirmwareVersion del PN532, en 0x24 y en addr.
  const uint8_t frame[] = {0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};
  const uint8_t destinos[] = {PN532_ADDR, addr};
  for (uint8_t d = 0; d < 2; d++) {
    uint8_t a = destinos[d];
    Wire.beginTransmission(a);
    Wire.write(frame, sizeof(frame));
    if (Wire.endTransmission() != 0) {
      Serial.printf("  frame PN532 -> 0x%02X: sin ACK (no hay nadie)\n", a);
      continue;
    }
    delay(50);
    uint8_t buf[12];
    uint8_t n = Wire.requestFrom(a, (uint8_t)sizeof(buf));
    Serial.printf("  frame PN532 -> 0x%02X responde %u bytes:", a, n);
    uint8_t i = 0;
    while (Wire.available() && i < sizeof(buf)) {
      buf[i] = Wire.read();
      Serial.printf(" %02X", buf[i]);
      i++;
    }
    // ACK del PN532: 00 00 FF 00 FF 00
    bool ack = false;
    for (uint8_t k = 0; k + 4 < i; k++) {
      if (buf[k] == 0x00 && buf[k + 1] == 0x00 && buf[k + 2] == 0xFF &&
          buf[k + 3] == 0x00 && buf[k + 4] == 0xFF) {
        ack = true;
      }
    }
    Serial.println(ack ? "   <- ACK de PN532" : "   (sin ACK de PN532)");
  }
  Serial.println("--- fin identificacion ---\n");
}

// --- Drivers ----------------------------------------------------------------

static bool initPN532() {
  pn532.begin();

  uint32_t version = pn532.getFirmwareVersion();
  if (!version) {
    return false;
  }

  Serial.printf("PN532 encontrado en 0x%02X. Chip PN5%02X, firmware %d.%d\n",
                PN532_ADDR, (version >> 24) & 0xFF, (int)((version >> 16) & 0xFF),
                (int)((version >> 8) & 0xFF));

  pn532.SAMConfig(); // SAM en modo normal (lector)

  // Reintentos de activacion. 0xFF = infinito; un valor bajo hace que
  // readPassiveTargetID() devuelva el control rapido y no bloquee el loop.
  pn532.setPassiveActivationRetries(0x14);
  return true;
}

// VersionReg (0x37) identifica el chip. Los clones FM17522 devuelven valores
// distintos a los del MFRC522 original de NXP, pero son compatibles.
// Los pasos de ganancia del MFRC522 no son lineales ni monotonos: 010 y 011
// repiten los valores de 000 y 001 (tabla 98 del datasheet).
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
    return "MFRC522 v1.0";
  case 0x92:
    return "MFRC522 v2.0";
  case 0xB2:
    return "clon FM17522E";
  default:
    return "desconocido";
  }
}

// Comprueba la configuracion que exige ISO14443A despues de PCD_Init(). Si uno
// de estos registros no tiene el valor debido, las tarjetas no contestan nunca
// y el sintoma es identico al de una antena que no radia: WUPA -> Timeout. El
// mas critico es TxASKReg: sin Force100ASK (0x40) no hay modulacion 100% ASK.
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
  if (mal == 0) {
    Serial.println("    => configuracion correcta: si no lee, es la antena.");
  } else {
    Serial.printf("    => %u registro(s) mal: no es la antena, es la config.\n",
                  mal);
  }
}

// Prueba dirigida a TxControlReg (0x14) especificamente. Ya sabemos que el
// registro 0x2D acepta cualquier patron arbitrario sin problema (identificarChip).
// Si TxControlReg NO se comporta igual -bits que no cambian pase lo que se
// escriba- es un hallazgo de hardware mucho mas fuerte que "antena mal
// adaptada": seria el propio registro que enciende los drivers de antena el
// que no responde a escrituras.
static void probarTxControlReg() {
  Serial.println("\nProbando escritura en TxControlReg (0x14) especificamente:");
  const uint8_t patrones[] = {0x00, 0xFF, 0x83, 0x03, 0x80};
  for (uint8_t i = 0; i < sizeof(patrones); i++) {
    rc522.PCD_WriteRegister(MFRC522::TxControlReg, patrones[i]);
    delay(2);
    uint8_t v = rc522.PCD_ReadRegister(MFRC522::TxControlReg);
    Serial.printf("  escribo 0x%02X -> leo 0x%02X  (bits distintos: 0x%02X)%s\n",
                  patrones[i], v, patrones[i] ^ v,
                  v == patrones[i] ? "  OK" : "  <-- NO COINCIDE");
  }
  // Dejar el registro en el valor que realmente necesitamos para operar.
  rc522.PCD_WriteRegister(MFRC522::TxControlReg, 0x83);
  delay(2);
  Serial.printf("  valor final tras forzar 0x83: 0x%02X\n",
                rc522.PCD_ReadRegister(MFRC522::TxControlReg));
}

static bool initRC522() {
  rc522.PCD_Init();
  delay(50);

  uint8_t v = rc522.PCD_ReadRegister(MFRC522::VersionReg);
  // 0x00 y 0xFF significan que no hay nadie escuchando en el bus.
  if (v == 0x00 || v == 0xFF) {
    return false;
  }

  Serial.printf("RC522 encontrado en 0x%02X. VersionReg=0x%02X (%s)\n",
                RC522_ADDR, v, nombreVersion(v));

  // Ganancia maxima del receptor (48 dB): de fabrica estos modulos tienen
  // 2-3 cm de alcance.
  rc522.PCD_SetAntennaGain(MFRC522::RxGain_max);

  // Escritura COMPLETA, no PCD_AntennaOn() (que solo hace un OR de bits 1:0
  // sobre lo que ya hubiera). Sin RST fisico (RC522_RST=UNUSED_PIN, el modo
  // I2C no lo tiene cableado) el chip nunca recibe un reset real por hardware.
  rc522.PCD_WriteRegister(MFRC522::TxControlReg, 0x83);
  probarTxControlReg();

  uint8_t tx = rc522.PCD_ReadRegister(MFRC522::TxControlReg);
  Serial.printf("  antena: TxControlReg=0x%02X (%s), ganancia %u/7 (%u dB)\n", tx,
                (tx & 0x03) == 0x03 ? "emitiendo" : "APAGADA",
                (rc522.PCD_GetAntennaGain() >> 4) & 0x07,
                gananciaDb(rc522.PCD_GetAntennaGain()));
  verificarConfigISO14443A();
  return true;
}

static void printUID(const uint8_t *uid, uint8_t len, const char *tipo) {
  Serial.printf("Tarjeta detectada  |  UID (%u bytes): ", len);
  for (uint8_t i = 0; i < len; i++) {
    Serial.printf("%02X", uid[i]);
    if (i + 1 < len) {
      Serial.print(':');
    }
  }

  // Los UID de 4 bytes tambien se suelen expresar como un entero decimal.
  if (len == 4) {
    uint32_t valor = ((uint32_t)uid[0] << 24) | ((uint32_t)uid[1] << 16) |
                     ((uint32_t)uid[2] << 8) | uid[3];
    Serial.printf("   (%lu)", (unsigned long)valor);
  }
  if (tipo != nullptr) {
    Serial.printf("   tipo: %s", tipo);
  }
  Serial.println();
}

// --- Arranque ---------------------------------------------------------------

static bool arrancarLector() {
  uint8_t addr = scanI2C();

  if (addr != 0) {
    identificarChip(addr);
  }

  // El PN532 se intenta SIEMPRE y primero, pase lo que pase en el escaneo: un
  // PN532 dormido no hace ACK a un sondeo pasivo, y begin() hace un reset y un
  // wakeup reales que el frame crudo de identificarChip() no replica. Que el
  // escaneo devuelva otra direccion no es motivo para no intentarlo.
  Serial.printf("Probando driver PN532 en 0x%02X... ", PN532_ADDR);
  if (initPN532()) {
    lector = ES_PN532;
    Serial.println("Listo. Acerca una tarjeta...\n");
    return true;
  }
  Serial.println("no responde.");
  if (addr == RC522_ADDR) {
    if (initRC522()) {
      lector = ES_RC522;
      Serial.println("Listo. Acerca una tarjeta...\n");
      return true;
    }
  }

  lector = NINGUNO;
  return false;
}

// Detector indirecto de brownout de alimentacion, sin necesitar un multimetro.
// I2C tolera mucho margen de voltaje; el driver de antena tira picos de
// decenas de mA justo al conmutar TX1/TX2. Si el riel de 3.3V se hunde en ese
// instante, el chip puede sufrir un power-on-reset silencioso: los registros
// vuelven a su valor de reset por hardware sin que I2C deje de responder. El
// mas facil de vigilar es TxControlReg: PCD_Init()+antena lo deja en 0x83; tras
// un reset de hardware vuelve a su default de 0x80.
static void pruebaAlimentacion() {
  Serial.println("\nProbando estabilidad de alimentacion bajo carga...");
  uint8_t anomalias = 0;
  for (uint8_t i = 0; i < 20; i++) {
    rc522.PCD_AntennaOff();
    delay(5);
    rc522.PCD_AntennaOn();
    delay(2);

    uint8_t tx = rc522.PCD_ReadRegister(MFRC522::TxControlReg);
    uint8_t ver = rc522.PCD_ReadRegister(MFRC522::VersionReg);
    if (tx != 0x83 || ver != 0xB2) {
      anomalias++;
      Serial.printf("  ciclo %2u: TxControlReg=0x%02X VersionReg=0x%02X  <-- "
                    "anomalo (reset silencioso probable)\n",
                    i, tx, ver);
    }
  }
  if (anomalias == 0) {
    Serial.println("  20/20 ciclos estables: no hay evidencia de brownout al");
    Serial.println("  conmutar la antena. La alimentacion aguanta la carga.");
  } else {
    Serial.printf("  %u/20 ciclos con registros corrompidos: fuerte indicio de\n",
                  anomalias);
    Serial.println("  brownout. Prueba con alimentacion externa al modulo.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500); // margen para que el monitor serie enganche el arranque
  Serial.println("\n\n=== ESP32-S3 + lector RFID por I2C ===");

  if (chequeoLineas() == BUS_SUJETO) {
    liberarBus();
    chequeoLineas();
  }

  // Wire.begin() con pines explicitos ANTES de los init: las librerias llaman
  // internamente a Wire.begin() sin argumentos, y el core de ESP32 detecta que
  // el bus ya esta inicializado y respeta estos pines en vez de los default.
  Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);
  Serial.printf("I2C en SDA=%d SCL=%d @ %d Hz\n", I2C_SDA, I2C_SCL, I2C_FREQ);

  arrancarLector();
  if (lector == ES_RC522) {
    pruebaAlimentacion();
  }
}

void loop() {
  if (lector == NINGUNO) {
    Serial.println("\nNo encuentro ningun lector. Comprueba:");
    Serial.println("  - alimentacion: VCC a 3V3 y GND comun con el ESP");
    Serial.printf("  - SDA -> GPIO%d y SCL -> GPIO%d\n", I2C_SDA, I2C_SCL);
    Serial.println("  - modo I2C por hardware:");
    Serial.println("      PN532 -> DIP SEL0 ON, SEL1 OFF");
    Serial.println("      RC522 -> pin EA a masa");
    Serial.println("Reintentando en 2 s...");
    delay(2000);

    if (chequeoLineas() == BUS_SUJETO) {
      liberarBus();
      Wire.end();
      delay(20);
      Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);
    }
    arrancarLector();
    return;
  }

  if (lector == ES_PN532) {
    uint8_t uid[7] = {0}; // 4 bytes (MIFARE Classic) o 7 (Ultralight/NTAG)
    uint8_t uidLength = 0;

    if (pn532.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 200)) {
      printUID(uid, uidLength, nullptr);
      delay(1000); // no repetir la misma tarjeta decenas de veces
    }
    return;
  }

  static uint32_t sondeos = 0;
  static uint32_t ultimoLatido = 0;
  sondeos++;
  if (millis() - ultimoLatido > 3000) {
    ultimoLatido = millis();
    uint8_t tx = rc522.PCD_ReadRegister(MFRC522::TxControlReg);

    // Ganancia fija al maximo (48 dB) a pedido: sin barrido, para descartar
    // que el problema fuera aterrizar en una ganancia baja justo cuando habia
    // tarjeta encima. Escritura completa de TxControlReg, no AntennaOff/On
    // (que solo hace un OR y puede arrastrar bits residuales entre ciclos).
    uint8_t gan = 7;
    rc522.PCD_SetAntennaGain(MFRC522::RxGain_max);
    rc522.PCD_WriteRegister(MFRC522::TxControlReg, 0x83);

    // WUPA en vez de REQA: despierta tambien a las etiquetas que quedaron en
    // HALT, que es donde las deja una lectura anterior. El StatusCode dice si
    // el problema es que no contesta nadie (TIMEOUT) o que si contesta pero la
    // comunicacion falla (CRC, colision, etc).
    byte atqa[2];
    byte n = sizeof(atqa);
    rc522.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
    rc522.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
    rc522.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);
    // El timer interno que produce el timeout de 25ms lo deriva el chip del
    // mismo cristal que genera la portadora de 13.56 MHz. Medir cuanto tarda
    // DE VERDAD es una forma indirecta de verificar el oscilador sin
    // osciloscopio: si el cristal estuviera desafinado, el I2C seguiria
    // funcionando perfecto (no depende de el) pero este tiempo se desviaria
    // de 25ms de forma medible.
    uint32_t t0 = millis();
    MFRC522::StatusCode st = rc522.PICC_WakeupA(atqa, &n);
    uint32_t transcurrido = millis() - t0;

    Serial.printf("[%lus] %lu sondeos, antena %s, ganancia %u/7 (%u dB), WUPA -> ",
                  (unsigned long)(millis() / 1000), (unsigned long)sondeos,
                  (tx & 0x03) == 0x03 ? "ON" : "OFF", gan,
                  gananciaDb(gan << 4));
    Serial.print(MFRC522::GetStatusCodeName(st));
    if (st == MFRC522::STATUS_OK || st == MFRC522::STATUS_COLLISION) {
      Serial.printf("  ATQA=%02X%02X", atqa[1], atqa[0]);
    }
    Serial.printf("  (%lu ms, esperado ~25ms)", (unsigned long)transcurrido);
    Serial.println();
    sondeos = 0;
  }

  // ES_RC522: PICC_IsNewCardPresent() manda un REQA, al que solo responden las
  // tarjetas que no estan en HALT; de ahi el PICC_HaltA() del final.
  if (!rc522.PICC_IsNewCardPresent() || !rc522.PICC_ReadCardSerial()) {
    return;
  }

  const __FlashStringHelper *tipo =
      MFRC522::PICC_GetTypeName(MFRC522::PICC_GetType(rc522.uid.sak));
  printUID(rc522.uid.uidByte, rc522.uid.size, (const char *)tipo);

  rc522.PICC_HaltA();
  rc522.PCD_StopCrypto1();
}
