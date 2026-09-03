# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Qué es

Firmware para un **ESP32-S3** (placa real: **OLIMEX ESP32-S3-DevKit-Lipo**) que
implementa un sistema de historias por combinación de personajes: dos módulos
**PN532** por I2C (cada uno en su propio bus de hardware) leen tags NFC que
representan personajes, y cuando dos personajes válidos quedan presentes a la
vez (uno por lector, en cualquier orden), dispara la pista de audio asociada a
esa combinación en un **DFPlayer Mini** por un tercer bus (UART), aparte del
que usa el monitor de la PC. Incluye además un portal web de monitoreo y
configuración, servido desde un Access Point propio del ESP32 (ver
[Portal web](#portal-web-de-monitoreoconfiguración) más abajo).

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
  los que la placa ya reserva para un segundo UART, libres de otro uso. Los
  roles quedan invertidos respecto a esa designación (17 es RX del ESP, 18 es
  TX) porque conviene así en el PCB — sigue siendo válido porque el ESP32
  mapea los pines de UART por matriz de software, no por asignación fija.

### Cableado

```
Lector 1   VCC->3V3  GND->GND  SDA->GPIO 8   SCL->GPIO 9    (bus Wire,   I2C0)
Lector 2   VCC->3V3  GND->GND  SDA->GPIO 12  SCL->GPIO 11   (bus Wire1,  I2C1)
DFPlayer   VCC->5V   GND->GND  TX->GPIO 17   RX->GPIO 18    (bus Serial1, UART1)
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

[src/log.h](src/log.h) / [src/log.cpp](src/log.cpp) — logger compartido:
`logf()`/`logln()` reemplazan a `Serial.printf()`/`Serial.println()` en todo
el firmware, escribiendo cada línea tanto al monitor serie como a un buffer
circular en RAM (`LOG_MAX_LINEAS` = 40 líneas de `LOG_MAX_LARGO` = 100
caracteres) que `logComoJSON()` expone como array JSON — así el portal web
puede mostrar el mismo log sin duplicar cada punto de logueo.

[src/settings.h](src/settings.h) / [src/settings.cpp](src/settings.cpp) —
`hostname` y `volumen` del DFPlayer, en `/settings.json` sobre LittleFS
(separado de `config.json`, que es solo la tabla de personajes/historias:
son dos cosas distintas que edita cada una su propio modal en el portal). Si
falta o está corrupto, usa los valores por defecto del struct — mismo patrón
de degradación que el resto del firmware.

[src/portal.h](src/portal.h) / [src/portal.cpp](src/portal.cpp) — portal web,
ver sección propia más abajo.

### Sistema de historias: personaje → combinación → pista

[data/config.json](data/config.json) vive en **LittleFS**, no compilado en el
firmware — se sube aparte con `pio run -t uploadfs` y se puede reemplazar sin
reflashear (por eso el portal web puede editarlo en caliente, ver más abajo,
con `guardarConfiguracionJSON()` en [src/config.cpp](src/config.cpp): valida
que el JSON parsee antes de escribirlo, así un JSON mal formado no deja al
sistema sin configuración). Formato:

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
  vez que los **dos** lectores tienen un personaje *reconocido* a la vez
  (`ambosConocidos`), y se resetea en cuanto cualquiera de los dos deja de
  tenerlo. Sacar y volver a poner la misma combinación **repite** la
  historia — no hace falta que cambie nada más.
- Una combinación de personajes que no esté en `historias` no dispara nada;
  queda logueado por serie (`combinacion sin historia asociada`).
- **Tag no reconocido**: `EstadoLector.personajeId` usa dos sentinelas
  distintos a propósito — `PERSONAJE_VACIO` (0, nada puesto) y
  `PERSONAJE_DESCONOCIDO` (-1, hay un tag pero no está en `config.json`). Si
  los dos casos compartieran el valor `0`, el primer tag desconocido tras
  bootear no dispararía el aviso (no habría "cambio de estado" que detectar).
  Un tag desconocido se imprime por serie (`tag NO reconocido, UID: ...`) para
  poder copiar ese UID y darlo de alta en `config.json`, pero no cuenta como
  personaje válido para combinaciones (`ambosConocidos` exige id `> 0`).

**Espera en dos etapas** cuando queda exactamente un lector ocupado
(reconocido o no —un tag desconocido también cuenta como "hay algo puesto"
para esto):

1. A los `TIEMPO_ESPERAR_MS` (2 s) suena `PISTA_ESPERAR` (pista `8`): un único
   audio genérico ("poné el otro personaje"), igual sin importar cuál quedó
   solo.
2. Si sigue solo hasta `TIEMPO_SOLITARIO_MS` (10 s totales), suena el
   `pistaSolo` **específico** de ese personaje (si tiene uno configurado; un
   tag desconocido no tiene `pistaSolo`, así que no suena nada en esta etapa).

Las dos etapas comparten el mismo flanco: `soloDesde` / `esperarYaDisparado` /
`soloYaDisparado` se resetean juntos apenas deja de haber exactamente uno
—se empareja, se saca, o llega un segundo tag—, así que sacar y volver a
poner el mismo personaje solo reinicia la cuenta desde cero.

### Audios: por qué `playMp3Folder()` y no `play()`

El firmware dispara pistas con `dfPlayer.playMp3Folder(pista)`, **no** con
`dfPlayer.play(pista)`. No es intercambiable — son dos comandos distintos del
chip del DFPlayer:

- `play(n)` (comando `0x03`) reproduce el archivo número *n* según **su
  posición física en la tabla FAT de la SD** (el orden en que quedó escrito),
  no según el número en el nombre. Si los mp3 no se copiaron a la tarjeta en
  orden numérico estricto —muy fácil que pase con un arrastre en lote—,
  `play(1)` puede terminar sonando cualquier otro archivo. Así se descubrió
  este problema: el log decía "pista 0001" y sonaba `0003.mp3`.
- `playMp3Folder(n)` (comando `0x12`) sí busca por el número **en el nombre
  del archivo**, inmune al orden físico de copiado.

**Requisito de la SD para `playMp3Folder()`**: los audios van en una carpeta
`/mp3/` en la raíz de la tarjeta (no sueltos en la raíz), nombrados con 4
dígitos al principio — `0001xxx.mp3`, `0002xxx.mp3`, etc. (el resto del
nombre después del número es libre). El número de cada archivo tiene que
coincidir con la `pista` que le asignaste a esa historia/`pistaSolo` en
`config.json`.

## Portal web de monitoreo/configuración

El ESP32 levanta un **Access Point propio** (no se une a ningún WiFi
existente, no necesita credenciales de red) con la UI y la API del portal:

- **SSID** `Cueva1`, **password** `cuevas123` (fijo en
  [src/portal.cpp](src/portal.cpp), no editable desde el portal — cambiarlo
  requiere tocar el código y reflashear).
- **mDNS**: `http://<hostname>.local/`, `cueva1.local` por defecto
  (`settings.json`, ver abajo). Si el dispositivo que se conecta no resuelve
  mDNS, entra igual por la IP del AP (`192.168.4.1`).
- Interfaz en [data/www/index.html](data/www/index.html) (subida a LittleFS
  con `pio run -t uploadfs`, igual que `config.json`): un panel de actividad
  en vivo (poll a `/api/log` cada 1.5 s) y dos modales —
  **Personajes/Historias** (edita `config.json` crudo en un textarea, valida
  el JSON en el navegador antes de mandarlo) y **Ajustes** (hostname y
  volumen).
- **API**: `GET/POST /api/config` (tabla de personajes/historias),
  `GET/POST /api/settings` (hostname/volumen), `GET /api/log` (líneas
  recientes del buffer de [src/log.h](src/log.h) como JSON). Guardar
  ajustes aplica el volumen al DFPlayer al instante (vía un callback que
  `main.cpp` pasa a `iniciarPortal()`, para que `portal.cpp` no dependa de
  la librería del DFPlayer) y, si cambió el hostname, reinicia el mDNS
  (`MDNS.end()` + `MDNS.begin()`) sin necesidad de reiniciar el ESP.
- **Se apaga solo** a los `PORTAL_TIMEOUT_MS` (5 minutos) del boot —
  `actualizarPortal()`, llamada sin condiciones en cada vuelta de `loop()`,
  corta el servidor, el mDNS y el AP. Reduce la ventana de exposición del AP
  y libera RAM/CPU; el resto del firmware (lectores, combinaciones, audio)
  sigue funcionando igual sin el portal. Para reactivarlo hace falta
  reiniciar el ESP.

`data/settings.json` (también en LittleFS, mismo mecanismo que
`config.json`):

```json
{
  "hostname": "cueva1",
  "volumen": 25
}
```

`hostname` va sin `.local` (lo agrega mDNS) y `volumen` es 0-30 (rango del
DFPlayer; el portal lo clampea si se manda un valor mayor). Si el archivo
falta o está corrupto, `cargarSettings()` usa estos mismos valores por
defecto — no bloquea el arranque.

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
