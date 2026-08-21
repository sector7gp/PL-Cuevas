# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Qué es

Firmware para un **ESP32-S3 DevKit** que lee tarjetas RFID/NFC ISO14443A por
**I2C** y vuelca el UID por el puerto serie. PlatformIO + framework Arduino.
Todo el firmware vive en [src/main.cpp](src/main.cpp).

**Soporta dos lectores y elige solo cuál usar** según lo que responda en el bus:

| Lector | Dirección | Librería | Protocolo |
|---|---|---|---|
| PN532 | `0x24` | `adafruit/Adafruit PN532` | frames |
| RC522 | `0x28` (algunos clones `0x3C`) | `makerspaceleiden/MFRC522-spi-i2c-uart-async` | registros "escribe índice, lee byte" |

El proyecto empezó apuntando solo al PN532. **El módulo físico está serigrafiado
`HW-147C` (el PCB rojo del PN532 V3) pero monta un FM17522**, un clon de MFRC522:
responde en `0x28` y su `VersionReg` (0x37) devuelve `0xB2`. Es un fraude
frecuente en marketplaces — PCB de PN532, chip distinto debajo. De ahí el soporte
doble: `arrancarLector()` escanea y despacha al driver correcto.

**No te fíes de la serigrafía de la placa.** La prueba que lo zanja está en
`identificarChip()`: escribe varios patrones en el registro `0x2D` y los lee de
vuelta. Si vuelven idénticos es un banco de registros direccionable, o sea
MFRC522/FM17522; un PN532 no tiene registros, interpreta la escritura como un
frame y nunca reproduce el valor. Como contraprueba manda el frame
`GetFirmwareVersion` a `0x24` y a la dirección encontrada, y busca el ACK
`00 00 FF 00 FF 00`.

## Dos entornos

El proyecto tiene **dos sketches independientes**, cada uno en su entorno, con
`build_src_filter` decidiendo cuál se compila:

| Entorno | Fuente | Qué es |
|---|---|---|
| `esp32-s3-devkitc-1` (por defecto) | `src/main.cpp` | el lector RFID |
| `scanner` | `src/scanner/scanner.cpp` | scanner I2C de diagnóstico, sin librerías de lector — solo `Wire` |
| `spi_test` | `src/spi_test/spi_test.cpp` | el RC522 por **SPI** en vez de I2C |

```bash
pio run -e scanner -t upload -t monitor    # diagnóstico del bus I2C
pio run -e spi_test -t upload -t monitor   # el lector por SPI
pio run -t upload -t monitor               # volver al lector I2C
```

**`spi_test` existe para aislar si el problema tiene algo que ver con I2C.** Usa
la misma librería y el mismo chip, pero por la ruta SPI original — la más
probada de esta librería — y con RST en un GPIO real: `PCD_Init()` por I2C
llevaba toda la sesión con `RC522_RST = UNUSED_PIN`, así que el chip nunca
recibió un reset por hardware genuino, solo el power-on al enchufarlo. Cableado
en el header inferior del módulo (SS/MOSI/MISO/SCK/RST), no el lateral I2C — ver
comentario de cabecera en el propio archivo.

El scanner existe porque cuando un driver está de por medio no puedes saber si
lo que ves es el hardware o la librería. Informa de: niveles eléctricos con y sin
pull-up interno, barrido a 50/100/400 kHz, volcado de registros `0x00-0x3F`, test
R/W para distinguir PN532 de MFRC522, frame `GetFirmwareVersion` del PN532, y
barrido con los pines cruzados. Repite cada 15 s para poder mover cables en vivo.

**El barrido cruzado usa `Wire1`, el segundo periférico I2C, a propósito.**
Hacerlo con `Wire.end()` + `Wire.begin(SCL, SDA)` da un falso positivo: si el core
considera que el bus sigue inicializado, ignora los pines nuevos y barre con los
de siempre, así que "responde cruzado" siempre sale a true.

## Estado de verificación

Verificado con hardware real:

- Bus I2C estable en **GPIO 6 (SDA) / 7 (SCL)**. Se movió desde 8/9: el GPIO 8 tiraba
  SDA a masa de forma intermitente y no se soltaba ni con 16 pulsos de reloj,
  aunque en la placa del módulo no hay continuidad SDA-GND. Todo el
  comportamiento errático inicial venía de ahí.
- Chip identificado: **FM17522E** en `0x28`, con test R/W de registros.
- `PN532` en `0x24`: no responde nunca, con `initPN532()` completo (reset + wakeup).
- Configuración ISO14443A tras `PCD_Init()`: los 10 registros críticos correctos,
  incluido `TxASKReg = 0x40`.
- Antena habilitada, las 8 ganancias barridas (18-48 dB).
- `TxControlReg` probado con escritura dirigida (`probarTxControlReg()`): acepta
  cualquier valor correctamente, incluida la única discrepancia esperada (bit
  reservado `0x04`, no escribible por diseño en cualquier unidad genuina). El
  registro no está dañado.
- Estabilidad de alimentación bajo carga: 20/20 ciclos de antena on/off sin
  corrupción de registros.

**Sin verificar: la lectura de un UID.** Ningún tag ha llegado a responder —
`PICC_WakeupA()` devuelve siempre `STATUS_TIMEOUT`. Los tags son NTAG213
(ISO 14443-3A, ATQA `0x0044`, UID de 7 bytes), plenamente compatibles con el chip.

**Con esto se agota lo que el firmware puede verificar.** No queda ninguna
variable digital sin comprobar: bus, protocolo, identificación de chip,
configuración de los 10 registros ISO14443A, estabilidad de alimentación bajo
carga, y el propio registro que enciende la antena — todo sano. Probado además
con dos tags físicamente distintos (sticker NTAG213 y tarjeta MIFARE de antena
grande) y con la ganancia fija al máximo sin barrido: cero detecciones en ambos.

Con osciloscopio se confirmó el cristal **Y1 oscilando limpio a 27.12 MHz** —
descarta un oscilador muerto como causa. Eso también explica el timeout de
~964 ms medido en `PICC_WakeupA()` (vs. los ~25 ms esperados): es el bucle de
polling de la librería, calibrado para SPI (~18 µs/iteración), agotando sus 2000
iteraciones a ~480 µs/iteración real sobre I2C — artefacto de timing, no
evidencia de nada roto.

Con el oscilador descartado, la sospecha física que queda es la red de
adaptación de antena: calculada para un PN532 y con un FM17522 montado encima,
puede que la etapa driver nunca llegue a entregar RF útil a la bobina. Pendiente
de confirmar con el osciloscopio en el punto de alimentación de la antena
(donde la espira del perímetro conecta con `L1`/`L3`/`C11`/`C7`/`C3`) — es la
única medición que falta para cerrar esto con certeza.

**Nota sobre `PCD_AntennaOn()`**: solo hace un OR de los bits 1:0 sobre
`TxControlReg`, nunca lo sobrescribe. Sin un pin RST real cableado en modo I2C
(`RC522_RST = UNUSED_PIN`), si el chip queda en un estado a medio resetear (p.ej.
un power-cycle que no descarga del todo los condensadores del módulo), esos bits
residuales persisten indefinidamente y el registro lee distinto de `0x83` aunque
la antena funcione igual. Por eso `initRC522()` escribe `TxControlReg` completo
en vez de usar `PCD_AntennaOn()`.

Al probar, el tag debe quedar **coplanar** con la placa: la antena del HW-147C es
la espira del perímetro del PCB, y el acoplamiento inductivo cae con el coseno del
ángulo entre los planos. Un tag apoyado sobre el canto no acopla nada.

## Comandos

```bash
pio run                                    # compilar
pio run --target upload                    # compilar + flashear (autodetecta puerto)
pio device monitor                         # monitor serie a 115200
pio run --target upload --target monitor   # flashear y abrir monitor
pio device list                            # ver puertos serie
```

El devkit enumera como `/dev/cu.usbmodem*` (puente USB-serie **CH343**, VID `0x1a86`
PID `0x55d3`). No es USB nativo del S3, así que `ARDUINO_USB_CDC_ON_BOOT` queda en 0
y `Serial` sale por **UART0 (GPIO43/44)**, que es donde está cableado el CH343.
No actives USB CDC salvo que se cambie de placa.

No hay tests: la verificación es flashear y leer el monitor serie. Para capturar
la salida sin bloquear en `pio device monitor`, leer el puerto con `python3` +
`pyserial` (ya instalado) unos segundos, forzando reset con RTS para no perderse
el arranque.

## Hardware y cableado

```
VCC (3.3V) -> 3V3      SDA -> GPIO 8  (I2C_SDA)
GND        -> GND      SCL -> GPIO 9  (I2C_SCL)
```

Los pines se cambian con los `#define I2C_SDA` / `I2C_SCL` al principio de
[src/main.cpp](src/main.cpp) y [src/scanner/scanner.cpp](src/scanner/scanner.cpp)
(los dos hay que tocarlos juntos). Actualmente **SDA=GPIO7, SCL=GPIO6** — se
movieron dos veces: primero de 8/9 a 6/7 porque GPIO8 tiraba SDA a masa de forma
intermitente (ver más abajo), y luego se intercambiaron 6↔7 porque un segundo
módulo llegó con SDA/SCL físicamente invertidos respecto al primero. El scanner
detecta esto solo con la sección `[4]` (barrido con los pines cruzados sobre
`Wire1`): si responde ahí y no en el barrido normal, hay que intercambiar los
`#define`, no recablear.

**Cada módulo necesita estar puesto en modo I2C por hardware**, y es la causa
más frecuente de que no aparezca nada en el bus:

- **PN532** (Elechouse V3): DIP switches `SEL0 = ON`, `SEL1 = OFF`
  (UART/HSU = OFF/OFF, SPI = OFF/ON). Ojo, un dipswitch puede quedarse trabado a
  medio camino y dar la impresión de estar bien puesto.
- **RC522**: pin `EA` a masa en la mayoría de clones. En modo SPI el pin
  serigrafiado `SDA` es en realidad el chip-select, así que la serigrafía
  engaña.

## Diagnóstico integrado

`setup()` hace tres cosas antes de hablar con el lector, en este orden. Es el
grueso del valor del firmware: cada paso descarta una clase distinta de fallo.

1. **`chequeoLineas()`** — mide las líneas **dos veces**, sin y con pull-up
   interno, y devuelve un `EstadoBus`. Es la comparación entre ambas medidas la
   que separa tres fallos que un escaneo vacío confunde en uno solo:

   | | sin pull-up | con pull-up | significa |
   |---|---|---|---|
   | `BUS_OK` | HIGH | HIGH | pull-ups externos: módulo alimentado |
   | `BUS_AL_AIRE` | LOW | HIGH | los pull-ups del módulo van a VCC; si no tiran, **no hay VCC** |
   | `BUS_SUJETO` | LOW | LOW | corto a masa, o un esclavo sujetando el bus |

2. **`liberarBus()`** — pulsa SCL hasta 16 veces y cierra con un STOP. **Solo se
   llama en `BUS_SUJETO`**: con las líneas al aire no hay nada que liberar y sus
   16 pulsos fallidos solo mandan a depurar el cableado de datos cuando el
   problema es la alimentación. Si aun en `BUS_SUJETO` los 16 pulsos no bastan,
   es físico y no de protocolo.
3. **`scanI2C()`** — barrido de direcciones.
4. **`identificarChip()`** — solo si algo respondió; distingue PN532 de MFRC522
   por comportamiento, no por lo que diga la serigrafía.

**Un bus que da resultados distintos en cada arranque con el mismo firmware es un
problema físico, no de código.** Durante el desarrollo el módulo apareció y
desapareció entre reinicios sin tocar el cableado: SDA a veces en HIGH, a veces
liberable en 1-6 pulsos, a veces atascado pese a los 16. En el header lateral del
HW-147C SDA es adyacente a GND, así que un dupont flojo o un pin doblado bastan.
Si `liberarBus()` agota los 16 pulsos, la línea está tirada a masa: no sigas
buscando en el firmware.

**El arranque es tolerante a fallos a propósito.** Si el lector no responde,
`loop()` reintenta cada 2 s en vez de colgarse, para poder recablear en caliente
y ver al instante cuándo engancha. No lo cambies por un `while(true)`.

## Detalles que no son obvios

- **`Wire.begin(SDA, SCL, FREQ)` tiene que llamarse ANTES de `PCD_Init()`.**
  Las librerías llaman internamente a `_wire->begin()` sin argumentos, lo que en
  el core de ESP32 reinicializaría `Wire` con los pines por defecto. Como el bus
  ya está inicializado, el core detecta `i2cIsInit()`, emite un `log_w` y respeta
  nuestros pines. Invertir el orden rompe cualquier pinout que no sea 8/9.

- La librería es `makerspaceleiden/MFRC522-spi-i2c-uart-async`, un fork de
  miguelbalboa con backend I2C. **Su enum `PCD_Register` usa índices directos**
  (`VersionReg = 0x37`) y es el driver SPI el que aplica el `<< 1`; el driver I2C
  escribe el índice tal cual. La librería original de miguelbalboa es solo SPI.

- `MFRC522_I2C_DEFAULT_ADDR` ya es `0x28`, que coincide con este módulo. Algunos
  clones de AliExpress usan `0x3C`.

- `VersionReg` leyendo `0x00` o `0xFF` significa que no hay nadie en el bus, no
  que el chip sea raro. Los valores válidos conocidos están en `nombreVersion()`:
  `0x91`/`0x92` son NXP auténticos, `0x88`/`0xB2` son clones FM17522 y funcionan
  igual.

- `PCD_SetAntennaGain(RxGain_max)` seguido de un ciclo off/on de antena: de
  fábrica estos módulos tienen 2-3 cm de alcance y la ganancia máxima ayuda
  bastante. El ciclo de antena es necesario para que el cambio surta efecto.
  `initRC522()` lo verifica leyendo `TxControlReg`: los bits 0-1 a 1 (`0x83`)
  significan que la antena emite de verdad. Sirve para descartar el lector
  cuando no aparece ninguna tarjeta.

- El PN532 dormido puede no hacer ACK a un escaneo pasivo, por eso
  `arrancarLector()` intenta `initPN532()` aunque el escaneo salga vacío
  (Adafruit hace lo mismo con `i2c_dev->begin(false)`). Un escaneo vacío no
  descarta que el chip esté ahí.

- Tras leer una tarjeta hay que llamar a `PICC_HaltA()` + `PCD_StopCrypto1()`.
  `PICC_IsNewCardPresent()` manda un REQA, al que solo responden las tarjetas que
  no están en HALT; sin eso la misma tarjeta se relee en bucle mientras siga
  encima del lector.

- `CORE_DEBUG_LEVEL=0` en `build_flags` silencia el `[E] Wire.cpp` que el core
  emite por cada sondeo fallido; con el reintento en bucle inundaba el monitor.
