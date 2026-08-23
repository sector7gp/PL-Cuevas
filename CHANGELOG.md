# Changelog

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.0.0/).

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
