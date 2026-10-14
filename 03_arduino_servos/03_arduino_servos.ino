/*
 * VigIA - Paso 3: Control de servos con Arduino Uno
 * ------------------------------------------------------------
 * El Arduino es el "músculo": recibe órdenes de texto y mueve los
 * servos pan-tilt de forma suave y dentro de límites seguros.
 * El ESP32-CAM (el "cerebro") enviará estas órdenes más adelante;
 * por ahora se prueban a mano desde el Monitor Serie.
 *
 * Órdenes (terminar con "Nueva línea"):
 *   P95        -> pan a 95°
 *   T88        -> tilt a 88°
 *   P95 T88    -> ambos a la vez (formato que usará el ESP32)
 *   C          -> centrar ambos
 *   D          -> activar el modo demostración (movimiento automático)
 *   ?          -> mostrar posición actual y límites
 *
 * MODO DEMOSTRACIÓN: al encender, los servos se mueven solos de
 * forma lenta recorriendo los límites seguros (izquierda, derecha,
 * arriba, abajo). Cualquier orden P/T/C lo detiene; "D" lo reactiva.
 *
 * Funcionan por dos vías:
 *   - Monitor Serie del Arduino (USB, 9600 baud) -> pruebas a mano
 *   - Pin 2 (desde IO13 del ESP32-CAM, 9600 baud) -> sistema final
 *
 * Conexiones:
 *   Pin 9   -> naranja servo PAN  (izquierda/derecha)
 *   Pin 10  -> naranja servo TILT (arriba/abajo)
 *   Pin 2   <- IO13 del ESP32-CAM (más adelante)
 *   GND     -> riel (−) común (cargador, servos y ESP32)
 *   Servos rojo/marrón -> cargador 5V (+/−), NUNCA al 5V del Arduino
 *
 * Errores comunes:
 *   - Los servos tiemblan o no se mueven: falta el GND común entre
 *     Arduino y cargador.
 *   - El Arduino se reinicia al mover: los servos están conectados
 *     al 5V del Arduino; pasarlos al cargador.
 *   - No se puede subir este código: desconectar los cables de los
 *     pines 0 y 1, y quitar el puente RESET-GND.
 *   - Escribo órdenes y no pasa nada: en el Monitor Serie elegir
 *     9600 baud y "Nueva línea".
 */

#include <Servo.h>
#include <SoftwareSerial.h>

// ---- Pines ----
#define PIN_PAN    9
#define PIN_TILT  10
#define PIN_RX_ESP 2   // recibe del ESP32 (IO13)
#define PIN_TX_ESP 3   // no se usa (el Arduino no responde al ESP32)

// ---- Límites en MEDICIÓN: probar de a poco con P y T y anotar ----
// hasta dónde llega cada servo sin chocar ni tensar cables.
// Luego fijar aquí los valores definitivos.
// (13 oct) Medidos por el usuario con el montaje final (cables de pan y
// tilt corregidos: antes estaban intercambiados)
#define PAN_MIN     50
#define PAN_MAX    160
#define PAN_CENTRO 110
#define TILT_MIN    10
#define TILT_MAX   120
#define TILT_CENTRO 70

// Amplitud del modo demostración (independiente de los límites)
#define DEMO_PAN  50   // pan recorre 40°-140°
#define DEMO_TILT 25   // tilt recorre 65°-115°

// ---- Sentido de giro (cambiar a true si gira al revés) ----
// Con "P100" la cámara debe girar hacia TU derecha (mirándola de frente).
// Con "T100" la cámara debe mirar MÁS ARRIBA.
#define PAN_INVERTIDO  false
#define TILT_INVERTIDO false

// ---- Suavidad: perfil con aceleración y frenado ----
// Cada movimiento arranca despacio, acelera y frena al acercarse al
// destino (como un auto que no arranca ni frena de golpe).
// (13 oct) Suave a pedido del usuario: arranques y frenadas lentos para
// que no se suelte ningún cable ni se mueva la estructura.
// ¡Si se cambian, actualizar ARDUINO_VEL/ARDUINO_ACEL en el ESP32!
#define VEL_MAX   50.0   // grados por segundo (velocidad crucero)
#define ACEL      60.0   // grados por segundo² (qué tan rápido acelera/frena)
#define PERIODO_MS 10    // cada cuánto se actualiza la posición

Servo servoPan, servoTilt;
SoftwareSerial desdeESP(PIN_RX_ESP, PIN_TX_ESP);

struct Eje {
  float pos;    // posición actual (con decimales, para moverse fino)
  float vel;    // velocidad actual (grados/s, con signo)
  int destino;  // ángulo pedido
};
Eje pan  = {PAN_CENTRO, 0, PAN_CENTRO};
Eje tilt = {TILT_CENTRO, 0, TILT_CENTRO};
#define panDestino  pan.destino
#define tiltDestino tilt.destino
unsigned long ultimoPaso = 0;

// ---- Modo demostración ----
#define PAUSA_DEMO_MS 600   // espera en cada punto del recorrido
const int recorrido[][2] = {   // {pan, tilt}
  {PAN_CENTRO - DEMO_PAN, TILT_CENTRO}, {PAN_CENTRO + DEMO_PAN, TILT_CENTRO}, {PAN_CENTRO, TILT_CENTRO},
  {PAN_CENTRO, TILT_CENTRO - DEMO_TILT}, {PAN_CENTRO, TILT_CENTRO + DEMO_TILT}, {PAN_CENTRO, TILT_CENTRO},
};
const int PUNTOS = sizeof(recorrido) / sizeof(recorrido[0]);
bool demoActiva = true;
int puntoDemo = 0;
unsigned long llegadaDemo = 0;
bool esperandoDemo = false;

String bufUSB, bufESP;

// Convierte un ángulo con decimales al pulso del servo (544-2400 µs,
// el mismo rango que usa Servo.write). Así se mueve en fracciones de grado.
int aMicrosegundos(float angulo, bool invertido) {
  if (invertido) angulo = 180.0 - angulo;
  return 544 + (int)(angulo * (2400 - 544) / 180.0);
}

void escribirServos() {
  servoPan.writeMicroseconds(aMicrosegundos(pan.pos, PAN_INVERTIDO));
  servoTilt.writeMicroseconds(aMicrosegundos(tilt.pos, TILT_INVERTIDO));
}

void mostrarEstado() {
  Serial.print(F("Pan "));  Serial.print((int)round(pan.pos));
  Serial.print(F(" (")); Serial.print(PAN_MIN); Serial.print('-'); Serial.print(PAN_MAX);
  Serial.print(F(")  Tilt ")); Serial.print((int)round(tilt.pos));
  Serial.print(F(" (")); Serial.print(TILT_MIN); Serial.print('-'); Serial.print(TILT_MAX);
  Serial.println(')');
}

// Interpreta una orden como "P95 T88", "P95", "T88", "C" o "?"
void procesarOrden(String orden, const __FlashStringHelper *origen) {
  orden.trim();

  // Líneas de información del ESP32 (empiezan con '#'): solo se muestran
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

  // Cualquier otra orden detiene la demostración
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

// Lee caracteres sin bloquear y procesa la orden al llegar '\n'
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

// Mueve un eje un instante con aceleración y frenado.
// Devuelve true si cambió de posición.
bool actualizarEje(Eje &e, float dt) {
  float falta = e.destino - e.pos;
  if (fabs(falta) < 0.05 && fabs(e.vel) < 1.0) {   // llegó
    e.pos = e.destino;
    e.vel = 0;
    return false;
  }
  // Velocidad máxima que todavía permite frenar a tiempo: v = sqrt(2·a·d)
  float vObjetivo = min(VEL_MAX, sqrt(2.0 * ACEL * fabs(falta)));
  if (falta < 0) vObjetivo = -vObjetivo;

  // Acercar la velocidad actual a la objetivo sin pasar de ACEL
  float dv = vObjetivo - e.vel;
  float maxDv = ACEL * dt;
  e.vel += constrain(dv, -maxDv, maxDv);

  float avance = e.vel * dt;
  if (fabs(avance) > fabs(falta)) avance = falta;   // no pasarse
  e.pos += avance;
  return true;
}

void moverSuave() {
  if (millis() - ultimoPaso < PERIODO_MS) return;
  float dt = (millis() - ultimoPaso) / 1000.0;
  ultimoPaso = millis();
  if (dt > 0.05) dt = 0.05;   // por si el loop se atrasó

  bool c1 = actualizarEje(pan, dt);
  bool c2 = actualizarEje(tilt, dt);
  if (c1 || c2) escribirServos();
}

bool llegaron() {
  return pan.vel == 0 && tilt.vel == 0 &&
         pan.pos == pan.destino && tilt.pos == tilt.destino;
}

// Cuando los servos llegan a un punto, espera y pasa al siguiente
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

void setup() {
  Serial.begin(9600);
  desdeESP.begin(9600);

  // Fijar la posición ANTES de attach: así el primer pulso ya es el centro
  // y el servo no da un salto a una posición aleatoria
  escribirServos();
  servoPan.attach(PIN_PAN);
  servoTilt.attach(PIN_TILT);

  Serial.println(F("\n=== VigIA - Servos con Arduino (MEDICION DE LIMITES) ==="));
  mostrarEstado();
  Serial.println(F("Ordenes: P95 | T88 | P95 T88 | C (centrar) | D (demo) | ? (estado)"));
  Serial.println(F("Modo demostracion ACTIVADO (cualquier orden lo detiene)"));
}

void loop() {
  leerLinea(Serial,   bufUSB, F("[USB]"));
  leerLinea(desdeESP, bufESP, F("[ESP32]"));
  avanzarDemo();
  moverSuave();
}
