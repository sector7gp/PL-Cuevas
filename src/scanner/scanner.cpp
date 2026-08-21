/*
 * Scanner I2C independiente — ESP32-S3
 *
 * Sketch de diagnostico puro: no carga ninguna libreria de lector, solo Wire.
 * Sirve para saber que hay realmente colgado del bus, sin que ningun driver
 * interfiera.
 *
 *   pio run -e scanner -t upload -t monitor
 *
 * El firmware principal vive en src/main.cpp y no se toca: cada uno se compila
 * en su propio entorno (ver build_src_filter en platformio.ini).
 *
 * Repite el informe completo cada 15 s, asi se puede mover el cableado y ver el
 * efecto en vivo.
 */

#include <Arduino.h>
#include <Wire.h>

#define I2C_SDA 6
#define I2C_SCL 7

// Se barre a varias velocidades: un bus con pull-ups debiles o cables largos
// puede funcionar a 50 kHz y fallar a 400 kHz.
static const uint32_t VELOCIDADES[] = {50000, 100000, 400000};

// ---------------------------------------------------------------------------
// Nivel electrico
// ---------------------------------------------------------------------------

static void informeLineas() {
  Serial.println("\n[1] Niveles electricos de las lineas");

  pinMode(I2C_SDA, INPUT);
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

  Serial.printf("    sin pull-up interno:  SDA=%s  SCL=%s\n",
                sdaLibre ? "HIGH" : "LOW", sclLibre ? "HIGH" : "LOW");
  Serial.printf("    con pull-up interno:  SDA=%s  SCL=%s\n",
                sdaPup ? "HIGH" : "LOW", sclPup ? "HIGH" : "LOW");

  // Los tres casos se distinguen justamente por la diferencia entre ambas
  // medidas, que es lo que un escaneo a secas no puede decirte.
  if (sdaLibre && sclLibre) {
    Serial.println("    => pull-ups externos presentes: modulo alimentado.");
  } else if (sdaPup && sclPup) {
    Serial.println("    => solo suben con el pull-up interno: lineas al aire,");
    Serial.println("       el modulo no esta conectado o no tiene alimentacion.");
  } else {
    Serial.println("    => alguna linea no sube ni con pull-up: cortocircuito a");
    Serial.println("       masa, o un esclavo sujetando el bus.");
  }
}

// ---------------------------------------------------------------------------
// Barrido
// ---------------------------------------------------------------------------

// Devuelve cuantos dispositivos respondieron, y deja las direcciones en 'lista'.
static uint8_t barrer(TwoWire &bus, uint8_t *lista, uint8_t maxLista,
                      uint32_t hz) {

  uint8_t n = 0;
  Serial.printf("    %6lu Hz:", (unsigned long)hz);
  for (uint8_t addr = 1; addr < 127; addr++) {
    bus.beginTransmission(addr);
    if (bus.endTransmission() != 0) {
      continue;
    }
    Serial.printf(" 0x%02X", addr);
    if (n < maxLista) {
      lista[n] = addr;
    }
    n++;
  }
  if (n == 0) {
    Serial.print(" (nada)");
  }
  Serial.println();
  return n;
}

// ---------------------------------------------------------------------------
// Interrogatorio de un dispositivo
// ---------------------------------------------------------------------------

static uint8_t regLeer(uint8_t addr, uint8_t reg, bool *ok) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    *ok = false;
    return 0;
  }
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) {
    *ok = false;
    return 0;
  }
  *ok = true;
  return Wire.read();
}

static void regEscribir(uint8_t addr, uint8_t reg, uint8_t valor) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(valor);
  Wire.endTransmission();
}

// Lectura cruda, sin escribir indice antes. Un chip de registros devuelve
// basura o el ultimo registro; un PN532 devuelve su byte de estado.
static void lecturaCruda(uint8_t addr) {
  uint8_t buf[16];
  uint8_t n = Wire.requestFrom(addr, (uint8_t)sizeof(buf));
  uint8_t i = 0;
  while (Wire.available() && i < sizeof(buf)) {
    buf[i++] = Wire.read();
  }
  Serial.printf("    lectura cruda (%u bytes):", n);
  for (uint8_t k = 0; k < i; k++) {
    Serial.printf(" %02X", buf[k]);
  }
  Serial.println();
}

static void volcadoRegistros(uint8_t addr) {
  Serial.println("    volcado de registros 0x00-0x3F:");
  Serial.println("         0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F");
  for (uint8_t base = 0x00; base < 0x40; base += 0x10) {
    Serial.printf("    %02X:", base);
    for (uint8_t off = 0; off < 0x10; off++) {
      bool ok = false;
      uint8_t v = regLeer(addr, base + off, &ok);
      if (ok) {
        Serial.printf(" %02X", v);
      } else {
        Serial.print(" --");
      }
    }
    Serial.println();
  }
}

// Prueba decisiva: escribe patrones en un registro y comprueba si vuelven.
// Solo un banco de registros direccionable reproduce lo escrito.
static bool tieneRegistros(uint8_t addr) {
  const uint8_t REG = 0x2D; // TReloadRegL en MFRC522: R/W y sin efectos raros
  const uint8_t patrones[] = {0x5A, 0xA5, 0x0F};
  bool ok = false;
  uint8_t original = regLeer(addr, REG, &ok);
  uint8_t aciertos = 0;

  Serial.printf("    test R/W en registro 0x%02X:", REG);
  for (uint8_t i = 0; i < sizeof(patrones); i++) {
    regEscribir(addr, REG, patrones[i]);
    delay(2);
    uint8_t v = regLeer(addr, REG, &ok);
    Serial.printf("  %02X->%02X", patrones[i], v);
    if (ok && v == patrones[i]) {
      aciertos++;
    }
  }
  Serial.println();
  regEscribir(addr, REG, original);
  return aciertos == sizeof(patrones);
}

static const char *nombreMFRC522(uint8_t v) {
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

// Registros con nombre del MFRC522, para leer el estado de un vistazo.
static void detalleMFRC522(uint8_t addr) {
  struct {
    uint8_t reg;
    const char *nombre;
  } tabla[] = {
      {0x01, "CommandReg"},  {0x06, "ErrorReg"},   {0x07, "Status1Reg"},
      {0x08, "Status2Reg"},  {0x0A, "FIFOLevel"},  {0x14, "TxControlReg"},
      {0x15, "TxASKReg"},    {0x26, "RFCfgReg"},   {0x37, "VersionReg"},
  };

  Serial.println("    registros con nombre (MFRC522):");
  for (uint8_t i = 0; i < sizeof(tabla) / sizeof(tabla[0]); i++) {
    bool ok = false;
    uint8_t v = regLeer(addr, tabla[i].reg, &ok);
    Serial.printf("      %-14s (0x%02X) = 0x%02X\n", tabla[i].nombre,
                  tabla[i].reg, v);
  }

  bool ok = false;
  uint8_t tx = regLeer(addr, 0x14, &ok);
  uint8_t rf = regLeer(addr, 0x26, &ok);
  uint8_t ver = regLeer(addr, 0x37, &ok);
  Serial.printf("    => chip: %s\n", nombreMFRC522(ver));
  Serial.printf("    => antena TX1/TX2: %s\n",
                (tx & 0x03) == 0x03 ? "habilitadas" : "APAGADAS");
  // Los pasos de ganancia del MFRC522 no son lineales ni monotonos: 010 y 011
  // repiten los valores de 000 y 001 (tabla 98 del datasheet).
  static const uint8_t DB[8] = {18, 23, 18, 23, 33, 38, 43, 48};
  uint8_t g = (rf >> 4) & 0x07;
  Serial.printf("    => ganancia RX: %u/7 (%u dB)\n", g, DB[g]);
}

// Contraprueba: frame GetFirmwareVersion del PN532.
static void pruebaPN532(uint8_t addr) {
  const uint8_t frame[] = {0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};

  Wire.beginTransmission(addr);
  Wire.write(frame, sizeof(frame));
  if (Wire.endTransmission() != 0) {
    Serial.printf("    frame PN532: 0x%02X no acepta la escritura\n", addr);
    return;
  }
  delay(50);

  uint8_t buf[12];
  uint8_t n = Wire.requestFrom(addr, (uint8_t)sizeof(buf));
  uint8_t i = 0;
  while (Wire.available() && i < sizeof(buf)) {
    buf[i++] = Wire.read();
  }

  Serial.printf("    frame PN532 -> %u bytes:", n);
  for (uint8_t k = 0; k < i; k++) {
    Serial.printf(" %02X", buf[k]);
  }

  // ACK del PN532: 00 00 FF 00 FF 00
  bool ack = false;
  for (uint8_t k = 0; k + 4 < i; k++) {
    if (buf[k] == 0x00 && buf[k + 1] == 0x00 && buf[k + 2] == 0xFF &&
        buf[k + 3] == 0x00 && buf[k + 4] == 0xFF) {
      ack = true;
    }
  }
  Serial.println(ack ? "   <- ACK VALIDO DE PN532" : "   (sin ACK de PN532)");
}

static void interrogar(uint8_t addr) {
  Serial.printf("\n[3] Dispositivo en 0x%02X\n", addr);
  lecturaCruda(addr);
  volcadoRegistros(addr);
  bool registros = tieneRegistros(addr);

  if (registros) {
    Serial.println("    => responde como banco de registros direccionable.");
    detalleMFRC522(addr);
  } else {
    Serial.println("    => NO se comporta como banco de registros.");
  }
  pruebaPN532(addr);
}


// Comparativa de GPIO libres. Si un pin se lee LOW con el pull-up interno
// activado mientras sus vecinos se leen HIGH, ese pin tiene algo tirando de el
// a masa — o esta danado. Es la forma de saber si el problema esta en el
// modulo, en el cable, o en el propio ESP32.
static void testGpios() {
  // Pines expuestos y libres del ESP32-S3 DevKit. Se omiten 0 (boot), 19-20
  // (USB), 26-37 (flash y PSRAM) y 43-44 (UART0, donde va el puente serie).
  static const uint8_t PINES[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                  12, 13, 14, 15, 16, 17, 18, 21, 47, 48};

  Serial.println("\n[5] Estado de los GPIO libres (sin nada que los maneje)");
  Serial.println("    pin: sin pull-up / con pull-up interno");

  for (uint8_t i = 0; i < sizeof(PINES); i++) {
    uint8_t pin = PINES[i];
    pinMode(pin, INPUT);
    delayMicroseconds(200);
    bool libre = digitalRead(pin);
    pinMode(pin, INPUT_PULLUP);
    delayMicroseconds(200);
    bool pup = digitalRead(pin);
    pinMode(pin, INPUT);

    const char *nota = "";
    if (!pup) {
      nota = "  <-- NO sube ni con pull-up: algo lo tira a masa";
    } else if (pin == I2C_SDA || pin == I2C_SCL) {
      nota = libre ? "  (linea I2C, con pull-up externo)" : "  (linea I2C)";
    }
    Serial.printf("    GPIO%-2u:  %s / %s%s\n", pin, libre ? "H" : "L",
                  pup ? "H" : "L", nota);
  }
  Serial.println("    Un pin sano y al aire da  L / H.");
}

// ---------------------------------------------------------------------------

static void informe() {
  Serial.println("\n===============================================");
  Serial.printf("  Scanner I2C  —  SDA=GPIO%d  SCL=GPIO%d\n", I2C_SDA, I2C_SCL);
  Serial.println("===============================================");

  informeLineas();

  Serial.println("\n[2] Barrido de direcciones");
  uint8_t lista[8];
  uint8_t total = 0;
  for (uint8_t v = 0; v < sizeof(VELOCIDADES) / sizeof(VELOCIDADES[0]); v++) {
    Wire.end();
    delay(20);
    Wire.begin(I2C_SDA, I2C_SCL, VELOCIDADES[v]);
    uint8_t n = barrer(Wire, lista, sizeof(lista), VELOCIDADES[v]);
    if (n > total) {
      total = n;
    }
  }

  // El interrogatorio se hace a 100 kHz, que es lo que tolera cualquier chip.
  Wire.end();
  delay(20);
  Wire.begin(I2C_SDA, I2C_SCL, 100000);

  if (total == 0) {
    Serial.println("\n[3] Nada que interrogar: el bus esta vacio.");
  } else {
    uint8_t n = barrer(Wire, lista, sizeof(lista), 100000);
    for (uint8_t i = 0; i < n && i < sizeof(lista); i++) {
      interrogar(lista[i]);
    }
  }

  // Barrido cruzado sobre el SEGUNDO periferico I2C. Hacerlo reabriendo Wire
  // con los pines al reves no sirve: si el core considera que el bus sigue
  // inicializado, ignora los pines nuevos y barre con los de siempre, dando un
  // falso positivo. Wire1 es hardware independiente y no arrastra ese estado.
  Serial.println("\n[4] Barrido con SDA/SCL intercambiados (bus secundario)");
  Wire1.begin(I2C_SCL, I2C_SDA, 100000);
  uint8_t cruzado = barrer(Wire1, lista, sizeof(lista), 100000);
  Wire1.end();
  Serial.println(cruzado > 0
                     ? "    OJO: responde con los pines cruzados."
                     : "    nada (correcto: los pines no estan cruzados).");

  testGpios();

  Serial.println("\n--- fin del informe, repito en 15 s ---");
}

void setup() {
  Serial.begin(115200);
  delay(500); // margen para que el monitor serie enganche el arranque
  informe();
}

void loop() {
  delay(15000);
  informe();
}
