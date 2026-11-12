#include <Servo.h>
#include <SoftwareSerial.h>

#define PIN_PAN    9          // pin de señal del servo pan (horizontal)
#define PIN_TILT  10          // pin de señal del servo tilt (vertical)

#define PIN_RX_ESP 4          // pin que recibe las órdenes del ESP32 (IO13)
#define PIN_TX_ESP 3          // pin de envío al ESP32 (no se usa)

#define PAN_MIN     50        // ángulo mínimo del pan
#define PAN_MAX    160        // ángulo máximo del pan
#define PAN_CENTRO 110        // ángulo central del pan
#define TILT_MIN    10        // ángulo mínimo del tilt
#define TILT_MAX   120        // ángulo máximo del tilt
#define TILT_CENTRO 70        // ángulo central del tilt

#define DEMO_PAN  50          // cuánto gira el pan en el modo demo
#define DEMO_TILT 25          // cuánto gira el tilt en el modo demo

#define PAN_INVERTIDO  false  // invierte el sentido de giro del pan
#define TILT_INVERTIDO false  // invierte el sentido de giro del tilt

#define VEL_MAX   50.0        // velocidad máxima de los servos (grados/s)
#define ACEL      60.0        // aceleración de los servos (grados/s²)
#define PERIODO_MS 10         // cada cuántos ms se actualiza el movimiento

Servo servoPan, servoTilt;                         // los dos servos
SoftwareSerial desdeESP(PIN_RX_ESP, PIN_TX_ESP);   // puerto serie con el ESP32

struct Eje {
  float pos;      // ángulo actual
  float vel;      // velocidad actual (grados/s)
  int destino;    // ángulo al que debe llegar
};
Eje pan  = {PAN_CENTRO, 0, PAN_CENTRO};    // estado del eje pan
Eje tilt = {TILT_CENTRO, 0, TILT_CENTRO};  // estado del eje tilt
#define panDestino  pan.destino
#define tiltDestino tilt.destino
unsigned long ultimoPaso = 0;              // momento (ms) del último paso de movimiento

#define PAUSA_DEMO_MS 600                  // pausa en cada punto del modo demo
const int recorrido[][2] = {               // puntos (pan, tilt) que recorre el modo demo
  {PAN_CENTRO - DEMO_PAN, TILT_CENTRO}, {PAN_CENTRO + DEMO_PAN, TILT_CENTRO}, {PAN_CENTRO, TILT_CENTRO},
  {PAN_CENTRO, TILT_CENTRO - DEMO_TILT}, {PAN_CENTRO, TILT_CENTRO + DEMO_TILT}, {PAN_CENTRO, TILT_CENTRO},
};
const int PUNTOS = sizeof(recorrido) / sizeof(recorrido[0]);   // cantidad de puntos del recorrido
bool demoActiva = true;                    // true = el modo demo está activo
int puntoDemo = 0;                         // siguiente punto del recorrido
unsigned long llegadaDemo = 0;             // momento en que llegó al punto actual
bool esperandoDemo = false;                // true = está haciendo la pausa en un punto

String bufUSB, bufESP;                     // texto recibido por USB y por el ESP32

// Convierte un ángulo en el ancho de pulso (µs) del servo
int aMicrosegundos(float angulo, bool invertido) {
  if (invertido) angulo = 180.0 - angulo;
  return 544 + (int)(angulo * (2400 - 544) / 180.0);
}

// Envía la posición actual a los dos servos
void escribirServos() {
  servoPan.writeMicroseconds(aMicrosegundos(pan.pos, PAN_INVERTIDO));
  servoTilt.writeMicroseconds(aMicrosegundos(tilt.pos, TILT_INVERTIDO));
}

// Muestra en el monitor serie la posición actual y los límites
void mostrarEstado() {
  Serial.print(F("Pan "));  Serial.print((int)round(pan.pos));
  Serial.print(F(" (")); Serial.print(PAN_MIN); Serial.print('-'); Serial.print(PAN_MAX);
  Serial.print(F(")  Tilt ")); Serial.print((int)round(tilt.pos));
  Serial.print(F(" (")); Serial.print(TILT_MIN); Serial.print('-'); Serial.print(TILT_MAX);
  Serial.println(')');
}

// Ejecuta una orden: "#texto" (aviso del ESP32), "?", "D", "C" o "P95 T88"
void procesarOrden(String orden, const __FlashStringHelper *origen) {
  orden.trim();

  if (orden.startsWith("#")) {
    Serial.print(F("[ESP32 info] "));
    Serial.println(orden.substring(1));
    return;
  }

  orden.toUpperCase();
  if (orden.length() == 0) return;

  if (orden == "?") { mostrarEstado(); return; }

  if (orden == "D") {
    demoActiva = true;
    esperandoDemo = false;
    Serial.println(F("Modo demostracion ACTIVADO"));
    return;
  }

  if (demoActiva) {
    demoActiva = false;
    Serial.println(F("Modo demostracion detenido (D para reactivar)"));
  }

  if (orden == "C") {
    panDestino = PAN_CENTRO;
    tiltDestino = TILT_CENTRO;
  } else {
    int p = orden.indexOf('P');
    int t = orden.indexOf('T');
    if (p < 0 && t < 0) {
      Serial.print(F("Orden no reconocida: ")); Serial.println(orden);
      return;
    }
    if (p >= 0) panDestino  = constrain(orden.substring(p + 1).toInt(), PAN_MIN, PAN_MAX);
    if (t >= 0) tiltDestino = constrain(orden.substring(t + 1).toInt(), TILT_MIN, TILT_MAX);
  }

  Serial.print(origen);
  Serial.print(F(" -> destino P")); Serial.print(panDestino);
  Serial.print(F(" T"));            Serial.println(tiltDestino);
}

// Junta los caracteres recibidos y procesa la orden al llegar el fin de línea
void leerLinea(Stream &entrada, String &buf, const __FlashStringHelper *origen) {
  while (entrada.available()) {
    char c = entrada.read();
    if (c == '\n' || c == '\r') {
      if (buf.length()) procesarOrden(buf, origen);
      buf = "";
    } else if (buf.length() < 60) {
      buf += c;
    }
  }
}

// Avanza un eje hacia su destino con aceleración suave; devuelve true si se movió
bool actualizarEje(Eje &e, float dt) {
  float falta = e.destino - e.pos;
  if (fabs(falta) < 0.05 && fabs(e.vel) < 1.0) {
    e.pos = e.destino;
    e.vel = 0;
    return false;
  }

  float vObjetivo = min(VEL_MAX, sqrt(2.0 * ACEL * fabs(falta)));
  if (falta < 0) vObjetivo = -vObjetivo;

  float dv = vObjetivo - e.vel;
  float maxDv = ACEL * dt;
  e.vel += constrain(dv, -maxDv, maxDv);

  float avance = e.vel * dt;
  if (fabs(avance) > fabs(falta)) avance = falta;
  e.pos += avance;
  return true;
}

// Cada PERIODO_MS mueve un poco los dos ejes y actualiza los servos
void moverSuave() {
  if (millis() - ultimoPaso < PERIODO_MS) return;
  float dt = (millis() - ultimoPaso) / 1000.0;
  ultimoPaso = millis();
  if (dt > 0.05) dt = 0.05;

  bool c1 = actualizarEje(pan, dt);
  bool c2 = actualizarEje(tilt, dt);
  if (c1 || c2) escribirServos();
}

// Devuelve true si los dos ejes llegaron a su destino y están quietos
bool llegaron() {
  return pan.vel == 0 && tilt.vel == 0 &&
         pan.pos == pan.destino && tilt.pos == tilt.destino;
}

// En modo demo, pasa al siguiente punto del recorrido tras una pausa
void avanzarDemo() {
  if (!demoActiva) return;
  if (!llegaron()) return;

  if (!esperandoDemo) {
    esperandoDemo = true;
    llegadaDemo = millis();
    return;
  }
  if (millis() - llegadaDemo < PAUSA_DEMO_MS) return;

  esperandoDemo = false;
  panDestino  = recorrido[puntoDemo][0];
  tiltDestino = recorrido[puntoDemo][1];
  puntoDemo = (puntoDemo + 1) % PUNTOS;
}

// Inicia los puertos serie, coloca los servos en el centro y muestra el menú
void setup() {
  Serial.begin(9600);
  desdeESP.begin(9600);

  escribirServos();
  servoPan.attach(PIN_PAN);
  servoTilt.attach(PIN_TILT);

  Serial.println(F("\n=== VigIA - Servos con Arduino (MEDICION DE LIMITES) ==="));
  mostrarEstado();
  Serial.println(F("Ordenes: P95 | T88 | P95 T88 | C (centrar) | D (demo) | ? (estado)"));
  Serial.println(F("Modo demostracion ACTIVADO (cualquier orden lo detiene)"));
}

// Lee órdenes por USB y del ESP32, avanza el demo y mueve los servos
void loop() {
  leerLinea(Serial,   bufUSB, F("[USB]"));
  leerLinea(desdeESP, bufESP, F("[ESP32]"));
  avanzarDemo();
  moverSuave();
}
