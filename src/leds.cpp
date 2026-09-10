#include "leds.h"

#include <Adafruit_NeoPixel.h>
#include <math.h>

#define LED_STRIP_PIN 47

// Largo por defecto, solo para construir el objeto antes de que se cargue
// settings.json. El largo real lo fija iniciarLeds()/setLargoTira() con
// updateLength(); el resto del modulo usa fill(), que escala solo.
#define LED_LARGO_DEFECTO 60

// ~50 Hz mientras hay algo que animar: fluido a simple vista sin saturar el
// bus WS2812 ni competir de mas con el resto de loop().
#define LED_FRAME_MS 20

// Duracion del fade entre estados. Suficiente para que se lea como una
// transicion y no como un salto, sin demorar la reaccion al cuento.
#define FADE_MS 800UL

// --- Glow del reposo ---------------------------------------------------------
// El color configurado es el PICO de la respiracion (por eso el maximo es
// 1.00): lo que se elige en el color picker del portal es exactamente lo que
// se ve en el punto mas brillante del ciclo. El minimo baja a 0.10 y no a
// algo mas alto porque el brillo aparente va con la raiz -- un rango angosto
// se percibe como un color quieto, no como una respiracion.
#define IDLE_BRILLO_MIN 0.10f
#define IDLE_BRILLO_MAX 1.00f
#define IDLE_PERIODO_MS 3000UL

static Adafruit_NeoPixel pixels(LED_LARGO_DEFECTO, LED_STRIP_PIN, NEO_GRB + NEO_KHZ800);
static uint16_t largoActual = LED_LARGO_DEFECTO;

static EfectoLed efectoActual = EFECTO_IDLE;
static unsigned long efectoDesde = 0;
static unsigned long ultimoFrame = 0;

// Colores de cada estado, en 0x00RRGGBB. Los pone settings.json via
// iniciarLeds()/setColoresLed(); estos son solo el arranque.
static uint32_t colorIdleCfg = 0x00B200;          // verde
static uint32_t colorDetectadoCfg = 0x80FFFF;     // cian tirando a blanco
static uint32_t colorReproduciendoCfg = 0xFFA000; // ambar

// Color que se estaba mostrando cuando arranco el efecto actual: el punto de
// partida del fade. Se toma del color real que habia en la tira y no de un
// valor asumido, asi un cambio de estado a mitad de otro fade sale continuo.
static uint32_t colorAlCambiar = 0;

// Ultimo color efectivamente escrito, origen del proximo fade.
static uint32_t colorActual = 0;

// true cuando un efecto de color fijo termino su fade. A partir de ahi no se
// vuelve a llamar a pixels.show(): el color ya esta en la tira. No es
// microoptimizacion -- show() deshabilita interrupciones ~1,8 ms con 60 LEDs,
// y hacerlo 50 veces por segundo para siempre le compite al WiFi y, sobre
// todo, a las cargas por OTA. EFECTO_IDLE nunca lo activa: respira siempre.
static bool estabilizado = false;

// Interpola dos colores canal por canal. t va de 0 (todo `a`) a 1 (todo `b`).
static uint32_t mezclar(uint32_t a, uint32_t b, float t) {
  uint8_t ra = (a >> 16) & 0xFF, ga = (a >> 8) & 0xFF, ba = a & 0xFF;
  uint8_t rb = (b >> 16) & 0xFF, gb = (b >> 8) & 0xFF, bb = b & 0xFF;
  uint8_t r = (uint8_t)(ra + (rb - ra) * t + 0.5f);
  uint8_t g = (uint8_t)(ga + (gb - ga) * t + 0.5f);
  uint8_t bl = (uint8_t)(ba + (bb - ba) * t + 0.5f);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
}

// Escala los tres canales por k (0..1). Es mezclar() contra negro, pero
// nombrado por lo que hace: bajarle el brillo a un color conservando su tono.
static uint32_t escalar(uint32_t color, float k) { return mezclar(0x000000, color, k); }

static void aplicar(uint32_t color) {
  colorActual = color;
  pixels.fill(pixels.Color((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
}

// Efectos de color fijo: fade desde colorAlCambiar hacia `destino`, y
// sostener. Marca `estabilizado` al llegar para dejar de refrescar.
static void efectoFijo(uint32_t destino, unsigned long transcurrido) {
  float t = (transcurrido >= FADE_MS) ? 1.0f : (float)transcurrido / (float)FADE_MS;
  aplicar(mezclar(colorAlCambiar, destino, t));
  if (t >= 1.0f) {
    estabilizado = true;
  }
}

// Reposo: respiracion continua sobre colorIdleCfg. Los primeros FADE_MS
// ademas mezclan desde el color previo, asi entrar al reposo desde cualquier
// otro estado no salta -- se funde hacia la respiracion ya en curso.
static void efectoIdle(unsigned long transcurrido) {
  float fase = (transcurrido % IDLE_PERIODO_MS) / (float)IDLE_PERIODO_MS; // 0..1
  float onda = (sinf(2.0f * PI * fase) + 1.0f) / 2.0f;                    // 0..1, suave
  float k = IDLE_BRILLO_MIN + onda * (IDLE_BRILLO_MAX - IDLE_BRILLO_MIN);
  uint32_t respirado = escalar(colorIdleCfg, k);

  float t = (transcurrido >= FADE_MS) ? 1.0f : (float)transcurrido / (float)FADE_MS;
  aplicar(mezclar(colorAlCambiar, respirado, t));
  // A proposito no se marca `estabilizado`: la respiracion no termina nunca.
}

static void actualizarEfecto(unsigned long transcurrido) {
  switch (efectoActual) {
    case EFECTO_IDLE:
      efectoIdle(transcurrido);
      break;
    case EFECTO_DETECTADO:
      efectoFijo(colorDetectadoCfg, transcurrido);
      break;
    case EFECTO_REPRODUCIENDO:
      efectoFijo(colorReproduciendoCfg, transcurrido);
      break;
  }
}

// Reinicia el fade tomando como origen el color que hay puesto ahora mismo.
static void reanimarDesdeColorActual() {
  colorAlCambiar = colorActual;
  efectoDesde = millis();
  estabilizado = false;
}

void iniciarLeds(uint16_t largo, uint32_t colorIdle, uint32_t colorDetectado,
                 uint32_t colorReproduciendo) {
  colorIdleCfg = colorIdle;
  colorDetectadoCfg = colorDetectado;
  colorReproduciendoCfg = colorReproduciendo;

  if (largo > 0 && largo <= LED_MAX_LARGO) {
    largoActual = largo;
    pixels.updateLength(largo);
  }

  pixels.begin();
  pixels.clear();
  pixels.show();

  // Arranca desde apagado, asi el encendido entra con el mismo fade que
  // cualquier otro cambio en vez de prender de golpe.
  colorActual = 0;
  reanimarDesdeColorActual();
}

void actualizarLeds() {
  if (estabilizado) {
    return;
  }

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
  reanimarDesdeColorActual();
}

void setLargoTira(uint16_t largo) {
  if (largo == 0 || largo > LED_MAX_LARGO || largo == largoActual) {
    return;
  }
  largoActual = largo;
  // updateLength() libera y reasigna el buffer, dejandolo en cero. Hay que
  // redibujar: sin esto la tira queda apagada hasta el proximo cambio de
  // estado, que en reposo podria no llegar nunca.
  pixels.updateLength(largo);
  reanimarDesdeColorActual();
}

void setColoresLed(uint32_t colorIdle, uint32_t colorDetectado, uint32_t colorReproduciendo) {
  if (colorIdle == colorIdleCfg && colorDetectado == colorDetectadoCfg &&
      colorReproduciendo == colorReproduciendoCfg) {
    return;
  }
  colorIdleCfg = colorIdle;
  colorDetectadoCfg = colorDetectado;
  colorReproduciendoCfg = colorReproduciendo;
  reanimarDesdeColorActual(); // que el cambio se vea como fade, no como salto
}
