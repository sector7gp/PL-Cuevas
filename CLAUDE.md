# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Qué es

Firmware para un **ESP32-S3** (placa real: **OLIMEX ESP32-S3-DevKit-Lipo**) que
implementa un sistema de historias por combinación de personajes: dos módulos
**PN532** por I2C (cada uno en su propio bus de hardware) leen tags NFC que
representan personajes, y cuando dos personajes válidos quedan presentes a la
vez (uno por lector, en cualquier orden), dispara la pista de audio asociada a
esa combinación en un **DFPlayer Mini** por un tercer bus (UART), aparte del
que usa el monitor de la PC.

## Comandos

```bash
pio run -t uploadfs                # sube data/config.json a LittleFS (solo cuando cambia)
pio run -t upload -t monitor       # compilar, flashear y monitorear (entorno por defecto)
pio run -e scanner -t upload -t monitor   # scanner I2C de diagnostico general
```

`uploadfs` y `upload` son independientes: cambiar el firmware no toca
`config.json` en la flash, y viceversa. Hace falta correr `uploadfs` al menos
una vez, y de nuevo cada vez que cambie `data/config.json`.

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

[src/main.cpp](src/main.cpp) — inicializa los dos lectores y el DFPlayer en
`setup()` (misma secuencia mínima por lector: `Wire.begin()` → `pn532.begin()`
→ `getFirmwareVersion()` → `SAMConfig()`), y en `loop()` sondea a ambos de
forma independiente con **timeout corto** en `readPassiveTargetID()`
(`TIMEOUT_LECTURA_MS`, 50 ms). Esto último es obligatorio, no cosmético: sin
timeout explícito la librería usa `timeout=0`, que significa *bloquear para
siempre* (`Adafruit_PN532.h`) — con dos lectores independientes, si el Lector 1
no tiene tag puesto y bloqueara para siempre, el Lector 2 nunca se llegaría a
consultar. Si un lector no aparece al arrancar, el otro sigue funcionando sin
bloquearse — nunca hay un `while(1)` que dependa de que los dos estén presentes.

Está basado directamente en [ejemplo_ok.ino](ejemplo_ok/ejemplo_ok.ino): un
sketch mínimo de un solo lector que sirve como referencia de la secuencia de
arranque que funciona de forma confiable en este hardware.

[src/config.h](src/config.h) / [src/config.cpp](src/config.cpp) — módulo de
configuración, separado de `main.cpp` a propósito: monta LittleFS, parsea
`/config.json` con ArduinoJson y expone tres funciones de consulta
(`personajeDeUID()`, `pistaDePersonajes()`, `nombreDePersonaje()`). Si LittleFS
no monta o el JSON falta/está corrupto, no crashea: lo reporta por serie y deja
las tablas vacías — mismo patrón de degradación prolija que usan los lectores y
el DFPlayer.

[src/scanner/scanner.cpp](src/scanner/scanner.cpp) — herramienta de diagnóstico
I2C standalone (sin librería de lector, solo `Wire`), en su propio entorno de
PlatformIO. Sirve para inspeccionar el bus —barrido de direcciones, niveles
eléctricos con/sin pull-up— de forma independiente de qué chip esté conectado.

### Sistema de historias: personaje → combinación → pista

[data/config.json](data/config.json) vive en **LittleFS**, no compilado en el
firmware — se sube aparte con `pio run -t uploadfs` y se puede reemplazar sin
reflashear (preparado para que a futuro un portal web lo edite). Formato:

```json
{
  "personajes": [
    { "id": 1, "nombre": "Personaje 1", "uid": "04ABE574C12A81", "pistaSolo": 4 }
  ],
  "historias": [
    { "personajes": [1, 2], "pista": 1 }
  ]
}
```

- `uid` es hex sin separadores, en el mismo orden en que `main.cpp` imprime el
  UID leído de un tag por serie — copiar y pegar esa salida funciona directo.
- `pistaSolo` es opcional por personaje: la pista que suena si ese personaje
  queda puesto sin pareja (ver más abajo). `0` o ausente = sin audio de
  espera configurado para ese personaje.
- El **orden no importa**: personaje A en Lector 1 + B en Lector 2 dispara la
  misma historia que B en Lector 1 + A en Lector 2 (`pistaDePersonajes()`
  normaliza el par antes de buscarlo).
- Cada lector mantiene su propio `EstadoLector` (`personajeId` + contador de
  `fallosSeguidos`) en `main.cpp`. Un tag se considera retirado recién después
  de `FALLOS_PARA_AUSENCIA` (3) lecturas fallidas seguidas — evita que un fallo
  de lectura transitorio se confunda con un retiro real.
- El disparo es **por flanco**: `comboYaDisparada` se pone en `true` la primera
  vez que ambos lectores tienen personaje válido a la vez, y se resetea en
  cuanto cualquiera de los dos queda vacío. Sacar y volver a poner la misma
  combinación **repite** la historia — no hace falta que cambie nada más.
- Una combinación de personajes que no esté en `historias` no dispara nada;
  queda logueado por serie (`combinacion sin historia asociada`).

**Personaje solitario**: si queda exactamente un lector ocupado (el otro
vacío) durante `TIEMPO_SOLITARIO_MS` (10 s), suena el `pistaSolo` de ese
personaje. Mismo patrón de flanco que la combinación: `soloDesde`/
`soloYaDisparado` se resetean apenas deja de haber exactamente uno —al
emparejarse o al sacarlo—, así que sacarlo y volver a ponerlo solo reinicia
la cuenta de 10 s. Si el segundo personaje llega antes de los 10 s, el timer
se cancela sin sonar nada y sigue el flujo normal de combinación.

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
