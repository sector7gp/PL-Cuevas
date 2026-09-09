#include "leds.h"

#include <Adafruit_NeoPixel.h>
#include <math.h>

#define LED_STRIP_PIN 47

// Cantidad fisica de LEDs de la tira de hoy. Si se agregan mas mas adelante,
// alcanza con subir este numero -- el resto del modulo (fill() por efecto)
// ya escala solo al tamano de la tira.
#define NUM_LEDS 60

// ~50 Hz: fluido a simple vista sin saturar el bus WS2812 ni competir de mas
// con el resto de loop() (lectores I2C, portal web).
#define LED_FRAME_MS 20

static Adafruit_NeoPixel pixels(NUM_LEDS, LED_STRIP_PIN, NEO_GRB + NEO_KHZ800);
static EfectoLed efectoActual = EFECTO_IDLE;
static unsigned long efectoDesde = 0;
static unsigned long ultimoFrame = 0;

// --- Efecto: EFECTO_IDLE -----------------------------------------------------
// Glow azul: respiracion continua entre IDLE_BRILLO_MIN y IDLE_BRILLO_MAX del
// brillo maximo, un ciclo completo (subida + bajada) cada IDLE_PERIODO_MS.
//
// El brillo se hornea directo en el valor del canal azul (en vez de usar
// pixels.setBrightness()) a proposito: setBrightness() reescala el buffer de
// color ya guardado en la tira segun la relacion con el brillo anterior, y
// llamarlo en cada frame acumula error de redondeo con el tiempo. Escribir
// el color final completo en cada frame evita ese problema de raiz.
#define IDLE_BRILLO_MIN 0.40f
#define IDLE_BRILLO_MAX 0.70f
#define IDLE_PERIODO_MS 3000UL

static void efectoIdle(unsigned long transcurrido) {
  float fase = (transcurrido % IDLE_PERIODO_MS) / (float)IDLE_PERIODO_MS; // 0..1
  float onda = (sinf(2.0f * PI * fase) + 1.0f) / 2.0f;                    // 0..1, suave
  uint8_t azul = (uint8_t)((IDLE_BRILLO_MIN + onda * (IDLE_BRILLO_MAX - IDLE_BRILLO_MIN)) *
                                255.0f +
                            0.5f);
  pixels.fill(pixels.Color(0, 0, azul));
}

// Despacho del efecto activo. Para sumar un efecto nuevo: agregar su valor a
// EfectoLed (leds.h), su funcion estatica aca arriba (misma firma que
// efectoIdle), y un caso mas en este switch.
static void actualizarEfecto(unsigned long transcurrido) {
  switch (efectoActual) {
    case EFECTO_IDLE:
      efectoIdle(transcurrido);
      break;
  }
}

void iniciarLeds() {
  pixels.begin();
  pixels.clear();
  pixels.show();
  efectoDesde = millis();
}

void actualizarLeds() {
  unsigned long ahora = millis();
  if (ahora - ultimoFrame < LED_FRAME_MS) {
    return;
  }
  ultimoFrame = ahora;

  actualizarEfecto(ahora - efectoDesde);
  pixels.show();
}

void setEfectoLed(EfectoLed efecto) {
  if (efecto == efectoActual) {
    return;
  }
  efectoActual = efecto;
  efectoDesde = millis();
}
