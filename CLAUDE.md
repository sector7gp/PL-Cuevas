# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Qué es

Firmware para un **ESP32-S3** (placa real: **OLIMEX ESP32-S3-DevKit-Lipo**) que lee
tarjetas NFC/RFID ISO14443A con **dos módulos PN532** conectados por I2C,
cada uno en su propio bus de hardware, y vuelca el UID por serie. Controla
además un **DFPlayer Mini** (reproductor MP3) por un tercer bus, un UART de
hardware aparte del que usa el monitor de la PC.

## Comandos

```bash
pio run -t upload -t monitor       # compilar, flashear y monitorear (entorno por defecto)
pio run -e scanner -t upload -t monitor   # scanner I2C de diagnostico general
```

No hay tests automatizados: la verificación es flashear y leer el monitor serie.

## Hardware

**La placa es una OLIMEX ESP32-S3-DevKit-Lipo**, no un DevKitC-1 genérico — importa
porque cambia el `flash_mode` correcto (`dio`, no `qio`; ver `platformio.ini`) y
porque expone GPIOs con funciones fijas que no son de propósito general:

- **GPIO 5 y 6** están cableados a sensado de batería LiPo (`PWR_SENSE`,
  `BAT_SENSE`) — tienen un divisor resistivo permanente. No usarlos como I2C ni
  como GPIO libre.
- La placa designa **GPIO 47/48** como su propio SDA/SCL "oficial", y
  **GPIO 17/18** como su propio TX1/RX1 "oficial" (disponibles en el conector
  `pUEXT`) — este proyecto usa 17/18 para el DFPlayer justamente porque son
  los que la placa ya reserva para un segundo UART, libres de otro uso.

### Cableado

```
Lector 1   VCC->3V3  GND->GND  SDA->GPIO 8   SCL->GPIO 9    (bus Wire,   I2C0)
Lector 2   VCC->3V3  GND->GND  SDA->GPIO 11  SCL->GPIO 12   (bus Wire1,  I2C1)
DFPlayer   VCC->5V   GND->GND  TX->GPIO 18   RX->GPIO 17    (bus Serial1, UART1)
```

Los dos lectores están en **buses I2C de hardware completamente independientes**
(el ESP32-S3 tiene dos periféricos I2C). No comparten líneas, así que ambos
pueden usar la dirección fija del PN532 (`0x24`) sin colisionar.

El ESP32-S3 tiene 3 UART de hardware. **UART0 (`Serial`) es el monitor de la
PC y no se toca** — el DFPlayer usa **UART1 (`Serial1`)** en pines aparte,
9600 baudios. El TX del DFPlayer va al RX del ESP y viceversa (cruzados, como
cualquier conexión serie punto a punto).

Cada PN532 tiene que estar puesto en modo I2C por hardware (DIP switch, jumper,
o el mecanismo propio de cada placa — no todas usan el mismo esquema, revisar
la serigrafía del módulo concreto).

## Arquitectura del firmware

[src/main.cpp](src/main.cpp) — inicializa los dos lectores en `setup()` con la
misma secuencia mínima cada uno (`Wire.begin()` → `pn532.begin()` →
`getFirmwareVersion()` → `SAMConfig()`), y en `loop()` sondea a ambos de forma
independiente. Si un lector no aparece, el otro sigue funcionando sin
bloquearse — nunca hay un `while(1)` que dependa de que los dos estén presentes.

Está basado directamente en [ejemplo_ok.ino](ejemplo_ok/ejemplo_ok.ino): un
sketch mínimo de un solo lector que sirve como referencia de la secuencia de
arranque que funciona de forma confiable en este hardware. `main.cpp` replica
esa misma secuencia para cada uno de los dos lectores, sin ningún acceso al bus
antes de `begin()`.

[src/scanner/scanner.cpp](src/scanner/scanner.cpp) — herramienta de diagnóstico
I2C standalone (sin librería de lector, solo `Wire`), en su propio entorno de
PlatformIO. Sirve para inspeccionar el bus —barrido de direcciones, niveles
eléctricos con/sin pull-up— de forma independiente de qué chip esté conectado.

### DFPlayer: mapeo tarjeta → pista

Cada lector dispara una pista fija al detectar una tarjeta (`probarLector()`
recibe el número de pista como parámetro):

- **Lector 1** → `dfPlayer.play(1)` → reproduce `0001.mp3`
- **Lector 2** → `dfPlayer.play(2)` → reproduce `0002.mp3`

Los archivos van en la **raíz de la tarjeta SD**, nombrados con 4 dígitos
(`0001.mp3`, `0002.mp3`, ...) — es la numeración por nombre de archivo del
propio DFPlayer (`play(int fileNumber)`), no un índice de carpeta. Si el
DFPlayer no respondió en `setup()` (`dfPlayer_ok == false`), `probarLector()`
sigue imprimiendo el UID normalmente pero no intenta reproducir nada.

## Detalle importante: reset del PN532 tras flashear

El firmware **no controla el pin de reset físico del PN532** (`RSTPD_N`/`RST_OUT`)
por software — en el hardware actual ese pin está atado fijo a 3.3V. Consecuencia
práctica: al reflashear el ESP32, el PN532 puede quedar con su motor I2C
atascado (el ESP32 pasa por el bootloader ROM durante el flasheo, sin manejar
los pines I2C, y el PN532 puede quedar esperando una transacción que nunca se
completa). Si un lector no aparece justo después de flashear, **cortar y
reconectar su VCC** antes de sospechar de otra cosa — no hace falta reflashear
de nuevo, alcanza con resetear el ESP una vez recuperada la alimentación del
módulo.
