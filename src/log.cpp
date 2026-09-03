#include "log.h"

#include <stdarg.h>

#define LOG_MAX_LINEAS 40
#define LOG_MAX_LARGO 100

static char buffer[LOG_MAX_LINEAS][LOG_MAX_LARGO];
static uint8_t logCount = 0; // cuantas lineas validas hay (hasta llenar el buffer)
static uint8_t logHead = 0;  // proximo indice a escribir (circular)

static void agregarLinea(const char *linea) {
  strncpy(buffer[logHead], linea, LOG_MAX_LARGO - 1);
  buffer[logHead][LOG_MAX_LARGO - 1] = '\0';
  logHead = (logHead + 1) % LOG_MAX_LINEAS;
  if (logCount < LOG_MAX_LINEAS) {
    logCount++;
  }
}

void logf(const char *fmt, ...) {
  char linea[LOG_MAX_LARGO];
  va_list args;
  va_start(args, fmt);
  vsnprintf(linea, sizeof(linea), fmt, args);
  va_end(args);
  Serial.println(linea);
  agregarLinea(linea);
}

void logln(const char *msg) {
  Serial.println(msg);
  agregarLinea(msg);
}

String logComoJSON() {
  String out = "[";
  for (uint8_t i = 0; i < logCount; i++) {
    uint8_t idx = (logHead + LOG_MAX_LINEAS - logCount + i) % LOG_MAX_LINEAS;
    if (i > 0) {
      out += ",";
    }
    out += "\"";
    for (const char *c = buffer[idx]; *c; c++) {
      if (*c == '"' || *c == '\\') {
        out += '\\';
        out += *c;
      } else if ((unsigned char)*c >= 0x20) { // salteando caracteres de control
        out += *c;
      }
    }
    out += "\"";
  }
  out += "]";
  return out;
}
