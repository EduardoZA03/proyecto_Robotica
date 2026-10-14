/*
 * VigIA - Paso 4: Prueba del enlace ESP32-CAM -> Arduino
 * ------------------------------------------------------------
 * Objetivo: comprobar que el ESP32 puede darle órdenes al Arduino.
 * Cada 3 segundos envía una orden de la secuencia de abajo por IO13.
 * Si los servos la siguen, el enlace funciona. (Aún no usa la cámara.)
 *
 * Conexiones (modo USO):
 *   ESP32 IO13 -> Arduino pin 2
 *   ESP32 GND  -> GND común (Arduino, cargador)
 *   ESP32 5V   -> 5V (del Arduino para esta prueba, o del cargador)
 *
 * El Arduino debe tener cargado 03_arduino_servos. En su Monitor Serie
 * (9600) aparecerá "[ESP32] -> destino P.. T.." con cada orden recibida.
 *
 * Detalle técnico: se usa el UART2 del ESP32 a 9600 baud.
 *   TX = GPIO13 (al Arduino). RX = GPIO12 (no se conecta: el Arduino
 *   no responde). No usar los pines por defecto del UART2, porque el
 *   GPIO16 es la PSRAM en la ESP32-CAM.
 */

#define PIN_TX_ARDUINO 13
#define PIN_RX_ARDUINO 12   // no se conecta

HardwareSerial enlace(2);

// Secuencia de prueba: {orden, descripción}
const char *ordenes[][2] = {
  {"P60 T90",  "pan a tu izquierda"},
  {"P120 T90", "pan a tu derecha"},
  {"P90 T75",  "tilt abajo"},
  {"P90 T105", "tilt arriba"},
  {"C",        "centro"},
};
const int N = sizeof(ordenes) / sizeof(ordenes[0]);
int i = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  enlace.begin(9600, SERIAL_8N1, PIN_RX_ARDUINO, PIN_TX_ARDUINO);
  Serial.println("\n=== VigIA - Paso 4: enlace ESP32 -> Arduino ===");
  Serial.println("Enviando una orden cada 3 segundos por IO13");
}

void loop() {
  enlace.print(ordenes[i][0]);
  enlace.print('\n');
  Serial.printf("Enviado: %-9s (%s)\n", ordenes[i][0], ordenes[i][1]);
  i = (i + 1) % N;
  delay(3000);
}
