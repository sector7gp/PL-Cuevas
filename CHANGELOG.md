# Changelog

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.0.0/).

## [v0.9] - 2026-09-09

### Agregado
- **Versión del firmware visible en el equipo.** `scripts/version.py` es un
  `extra_scripts` de PlatformIO que define `FIRMWARE_VERSION` con la salida de
  `git describe --tags --always --dirty` en cada compilación, y `main.cpp` la
  imprime como primera línea del log (visible también desde el portal).
  Produce `v0.9` sobre el tag, `v0.9-3-gabc1234` en `dev`, y el sufijo
  `-dirty` si había cambios sin commitear — el dato que más importa en una
  cueva desplegada, porque avisa que ese equipo tiene un binario que no se
  puede reproducir desde el repo.
- **Tercer estado de la tira.** Ahora son tres, elegidos por prioridad al
  final de `loop()`: `EFECTO_IDLE` (glow de reposo), `EFECTO_DETECTADO`
  (color fijo apenas hay un tag puesto, reconocido o no) y
  `EFECTO_REPRODUCIENDO` (color fijo mientras se cuenta una historia). Todas
  las transiciones entran con un fade de 800 ms.
- **Configurables desde el modal de Ajustes**, todos aplicados en caliente sin
  reflashear ni reiniciar:
  - **SSID** del Access Point. Con varias cuevas desplegadas, si no todas
    emitirían el mismo nombre de red. A diferencia del resto, este **no** se
    aplica en caliente: rehacer el `softAP()` tiraría a todos los clientes,
    empezando por el que mandó el POST.
  - **Los tres colores** de la tira (`colorIdle`, `colorDetectado`,
    `colorReproduciendo`), con `<input type="color">`.
  - **Largo de la tira** (`largoTira`, 1–300), vía `updateLength()`.
- Validación de formato **al guardar y al cargar** para cada campo nuevo. Un
  SSID vacío o un color mal escrito dejaría el equipo sin AP o con la tira
  apagada, y sin AP no hay portal ni OTA para deshacerlo: solo el cable.

### Cambiado
- El callback del portal pasa de `onVolumenCambiado(uint8_t)` a
  `onAjustesCambiados(const Settings &)`. Con los colores hubieran hecho falta
  dos callbacks, y el próximo ajuste, tres.
- La tira **deja de refrescarse** cuando termina el fade de un estado de color
  fijo. `pixels.show()` deshabilita interrupciones ~30 µs por LED (~1,8 ms con
  60), y hacerlo 50 veces por segundo para siempre le competía al WiFi y sobre
  todo a las cargas por OTA. Ese mismo costo es el que fija el tope de 300
  LEDs.
- El glow de reposo toma el color configurado como **pico** de la respiración
  (rango 0.10–1.00), así lo que se elige en el color picker es exactamente lo
  que se ve en el punto más brillante.

### Corregido
- **El color de "reproduciendo" no se veía nunca.** El flag que lo enciende se
  apagaba también en la rama de "no hay pareja", que con un solo personaje
  puesto corre en *cada* vuelta de `loop()` y está antes de los disparadores de
  audio: se apagaba una iteración después de encenderse, así que el estado
  duraba decenas de milisegundos contra un fade de 800 ms y era invisible.
  Ahora hay un **único** lugar que lo apaga —el cambio de presencia de tags—
  ubicado antes de todos los disparadores.
- El aviso genérico de espera ("poné el otro personaje") ya no cuenta como
  historia: es una instrucción, no una narración, así que la tira se queda en
  el color de detectado mientras suena. Sí cuentan el cuento de la pareja y el
  `pistaSolo` de un personaje.

## [v0.8] - 2026-09-09

### Agregado
- **Carga de firmware por red (OTA)**, sobre el mismo Access Point que ya
  levanta el portal — no hace falta router ni internet.
  `src/ota.h`/`.cpp` (ArduinoOTA) más un entorno `[env:ota]` en
  `platformio.ini` que es la misma build que la del entorno por defecto
  (`extends`) cambiando solo el protocolo. `pio run -t upload` sigue siendo
  por cable. No hizo falta reparticionar: `default_8MB.csv` ya traía `otadata`
  y `app0`/`app1`. Cuesta +18 KB de flash.
  - `upload_port` va por IP y **no** por `cueva1.local`: `espota.py` resuelve
    con `socket.gethostbyname()` de Python, que en macOS no consulta
    mDNSResponder, así que el ping y el navegador encuentran el host pero el
    uploader corta con `Host Not Found`.
  - `loop()` no hace nada más mientras hay una transferencia en curso: el
    portal podría apagarse por timeout en medio y llevarse el WiFi, y el
    sondeo de los lectores le robaría ancho de banda hasta hacerla expirar.

### Cambiado
- `PORTAL_TIMEOUT_MS` de 5 a **60 minutos**. Apagar el portal apaga el WiFi
  entero, y con él el OTA: 5 minutos obligaba a resetear el ESP y apurarse en
  cada carga por red.
- El glow de reposo pasa de azul a verde.

### Corregido
- `apagarPortal()` tenía "5 min" escrito a mano en su mensaje de log, así que
  con el cambio de timeout habría reportado un número falso. Ahora lo calcula
  de `PORTAL_TIMEOUT_MS`, igual que el mensaje de arranque.

## [v0.7] - 2026-09-09

### Agregado
- Tira **WS2812** de indicación visual en GPIO47 (`src/leds.h`/`.cpp`), con un
  efecto de reposo y la estructura para sumar más sin cambiar la interfaz del
  módulo. Dependencia `adafruit/Adafruit NeoPixel@^1.12.5`.

### Corregido
- **Si no aparecía ningún lector, el portal no llegaba a levantar.** `setup()`
  se colgaba en un `while(1)` ubicado antes de `iniciarPortal()`, así que no
  arrancaban ni el DFPlayer, ni los LEDs, ni el portal — justo en el único
  caso en que el portal es imprescindible, porque sin monitor serie enchufado
  es la única forma de leer el log y averiguar por qué no aparecen. Ahora
  loguea el diagnóstico y sigue de largo.

## [v0.6] - 2026-09-03

### Agregado
- Portal web de monitoreo y configuración, servido desde un **Access Point
  propio** del ESP32 (SSID `Cueva1`, sin depender de un router externo) con
  **mDNS** (`cueva1.local` por defecto) y apagado automático a los 5 minutos
  del boot (`PORTAL_TIMEOUT_MS`).
  - Modal **Personajes/Historias**: lee y escribe `config.json` en caliente
    desde el navegador (`GET/POST /api/config`), sin reflashear ni tocar
    `uploadfs`. `guardarConfiguracionJSON()` valida el JSON antes de
    escribirlo.
  - Modal **Ajustes**: hostname y volumen del DFPlayer
    (`GET/POST /api/settings`, nuevo `data/settings.json`). Guardar aplica
    el volumen al instante y reinicia el mDNS si cambió el hostname.
  - Monitoreo de actividad en vivo (`GET /api/log`), alimentado por un logger
    compartido (`src/log.h`/`.cpp`) que espeja cada línea al monitor serie y
    a un buffer circular en RAM — reemplaza todos los `Serial.print*` del
    firmware por `logf()`/`logln()`.
  - UI en `data/www/index.html` (LittleFS, sin dependencias externas — el AP
    no tiene salida a internet).
- `src/settings.h`/`.cpp`: carga/guarda `hostname` y `volumen` en
  `/settings.json`, con degradación a valores por defecto si falta o está
  corrupto.

### Cambiado
- Pines del DFPlayer (UART1) cruzados por conveniencia de layout del PCB:
  `DFPLAYER_RX` pasa de GPIO18 a GPIO17, `DFPLAYER_TX` de GPIO17 a GPIO18.
- Pines I2C del Lector 2 cruzados por el mismo motivo: `SDA_2` pasa de
  GPIO11 a GPIO12, `SCL_2` de GPIO12 a GPIO11.

## [v0.5] - 2026-08-23

### Agregado
- Espera en dos etapas cuando queda un solo personaje puesto: a los 2 s suena
  un audio genérico (`PISTA_ESPERAR`, pista 8, "poné el otro personaje"), y si
  sigue solo hasta los 10 s totales, suena el `pistaSolo` específico de ese
  personaje. Repone el campo `pistaSolo` por personaje en `config.json` que
  se había sacado en v0.3.

### Corregido
- `EstadoLector.personajeId` usaba `0` tanto para "lector vacío" como para
  "tag presente pero no reconocido", así que el primer tag desconocido tras
  bootear no disparaba ningún aviso (no había cambio de estado que detectar).
  Se separan en dos sentinelas (`PERSONAJE_VACIO`=0, `PERSONAJE_DESCONOCIDO`=-1)
  y ahora un tag no reconocido se imprime por serie de forma confiable, para
  poder copiar su UID y darlo de alta en `config.json`.
- El disparo de historia ahora exige que los dos lados sean personajes
  *reconocidos* (`ambosConocidos`), no solo "algo puesto" — un tag
  desconocido ya no forma una combinación falsa con un personaje válido.

## [v0.4] - 2026-08-23

### Corregido
- El firmware disparaba audio con `dfPlayer.play(pista)`, que reproduce por
  **posición física en la tabla FAT de la SD**, no por el número en el nombre
  del archivo — si los mp3 no se copiaron en orden numérico estricto, sonaba
  un archivo distinto al pedido (el log decía "pista 0001" y sonaba
  `0003.mp3`). Cambiado a `dfPlayer.playMp3Folder(pista)`, que sí busca por el
  número del nombre del archivo. Requiere que los audios estén en una carpeta
  `/mp3/` en la raíz de la SD, nombrados `0001xxx.mp3`, `0002xxx.mp3`, etc.

## [v0.3] - 2026-08-23

### Agregado
- Sistema de historias por combinación de personajes: dos tags NFC válidos
  presentes a la vez (uno por lector, en cualquier orden) disparan la pista
  de audio asociada a esa combinación.
- Audio de "personaje solitario": si un personaje queda puesto sin pareja
  durante 10 segundos (`TIEMPO_SOLITARIO_MS`), suena su pista de espera. Se
  cancela sin sonar si el segundo personaje llega antes.
- `data/config.json` sobre LittleFS (no compilado en el firmware): tabla de
  personajes (UID→id, con su `pistaSolo`) e historias (combinación→pista).
  Se sube por separado con `pio run -t uploadfs`, sin reflashear — preparado
  para que a futuro un portal web la edite sin recompilar.
- `src/config.h` / `src/config.cpp`: módulo de configuración separado de
  `main.cpp`, con degradación prolija si LittleFS no monta o el JSON falta.
- Dependencia `bblanchon/ArduinoJson@^7.4.3`.

### Corregido
- `readPassiveTargetID()` se llamaba sin `timeout` explícito, lo que en la
  librería significa bloquear para siempre. Con dos lectores independientes,
  si el Lector 1 no tenía tag puesto, el Lector 2 nunca se llegaba a
  consultar. Ahora ambos usan un timeout corto (50 ms) en cada vuelta de
  `loop()`.

## [v0.2] - 2026-08-22

### Agregado
- Soporte para DFPlayer Mini (reproductor MP3) por `Serial1` (UART1), en
  pines aparte del monitor de la PC (`Serial`/UART0, sin tocar).
- Disparo de pista por lector: Lector 1 → `0001.mp3`, Lector 2 → `0002.mp3`
  (reemplazado en v0.3 por la lógica de combinación).
- Dependencia `dfrobot/DFRobotDFPlayerMini@^1.0.6`.

## [v0.1] - 2026-08-22

### Agregado
- Lector dual PN532 por I2C funcionando en hardware real: cada módulo en su
  propio periférico I2C (`Wire` y `Wire1`), buses completamente
  independientes.
- Identificación de la placa real (**OLIMEX ESP32-S3-DevKit-Lipo**, no un
  DevKitC-1 genérico) y corrección de `board_build.flash_mode` a `dio`.
- `src/scanner/scanner.cpp`: herramienta de diagnóstico I2C standalone, en su
  propio entorno de PlatformIO.
- `ejemplo_ok.ino`: sketch mínimo de referencia, verificado funcionando
  contra el PN532 real — `main.cpp` replica su secuencia de arranque.

### Quitado
- Soporte RC522 y prueba por SPI (`src/spi_test/`), fuera de alcance del
  proyecto final.

## Sin versionar (commit inicial)

- Firmware exploratorio inicial de lector RFID/NFC por I2C con diagnóstico
  integrado, previo a confirmar el hardware real y el enfoque final del
  proyecto.
