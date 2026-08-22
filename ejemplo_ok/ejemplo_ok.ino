#include <Wire.h>
#include <Adafruit_PN532.h>

// Definimos los pines físicos del ESP32-S3 DevKit
#define I2C_SDA 8
#define I2C_SCL 9

// Pines IRQ y RESET del PN532. 
// Si no los tienes conectados, pon un pin que no uses (ej: 4 y 5) o usa -1 si la librería lo permite
#define PN532_IRQ   4  
#define PN532_RESET 5  

// CONSTRUCTOR CORRECTO: Usa el bus I2C de hardware (&Wire)
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET, &Wire);

void setup() {
  Serial.begin(115200);
  while (!Serial);
  Serial.println("Inicializando Lector PN532 mediante Hardware I2C...");

  // 1. Forzamos primero a la librería nativa Wire a usar tus pines exitosos
  Wire.begin(I2C_SDA, I2C_SCL);
  
  // Opcional pero recomendado para el ESP32-S3 y PN532 (evita problemas de reloj)
  Wire.setTimeOut(100); 

  // 2. Ahora arrancamos el objeto NFC de Adafruit
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("Error: Adafruit no encuentra la placa PN532 en el bus.");
    Serial.println("Verifica si conectaste los pines IRQ y RESET o cambia sus números en el código.");
    while (1); 
  }
  
  Serial.print("¡Chip PN532 detectado con éxito! Firmware v"); 
  Serial.print((versiondata>>16) & 0xFF, DEC); 
  Serial.print('.'); 
  Serial.println((versiondata>>8) & 0xFF, DEC);
  
  nfc.SAMConfig();
  Serial.println("Esperando tarjeta...");
}

void loop() {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  
  uint8_t uidLength;                        
  
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);
  
  if (success) {
    Serial.println("------------------------------------");
    Serial.print("¡Tarjeta detectada! UID: ");
    for (uint8_t i = 0; i < uidLength; i++) {
      if (uid[i] < 0x10) Serial.print("0");
      Serial.print(uid[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    delay(1500);
  }
}
