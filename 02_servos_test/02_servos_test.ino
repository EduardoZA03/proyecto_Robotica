/*
 * VigIA - Paso 2: Prueba de servos pan-tilt
 * ------------------------------------------------------------
 * Objetivo: verificar que los dos MG996R se mueven bien y
 * encontrar los límites reales del soporte (para que la cámara
 * no choque con los brackets).
 *
 * Al encender:
 *   1. Lleva ambos servos al centro (pan 90°, tilt 90°).
 *   2. Hace un barrido lento de prueba.
 *   3. Queda esperando comandos por el Monitor Serie.
 *
 * Comandos (Monitor Serie a 115200, con "Nueva línea"):
 *   p90   -> mueve pan a 90°
 *   t45   -> mueve tilt a 45°
 *   c     -> centra ambos
 *   b     -> repite el barrido de prueba
 *
 * Conexiones:
 *   Servo PAN  señal (naranja) -> GPIO 13
 *   Servo TILT señal (naranja) -> GPIO 12
 *   Servos rojo (+)            -> 5V de la FUENTE EXTERNA (no del FTDI)
 *   Servos marrón (-)          -> GND de la fuente
 *   GND de la fuente           -> GND del ESP32-CAM  (¡obligatorio unirlos!)
 *
 * ¿Por qué GPIO 12 y 13? Con la microSD en modo 1-bit (Paso 1)
 * son los únicos pines libres que no usan la cámara ni la SD.
 *
 * Errores comunes:
 *   - El ESP32 se reinicia al moverse los servos ("Brownout"):
 *       los servos están tomando corriente del FTDI. Usar la fuente
 *       5V 2A para los servos y unir los GND.
 *   - El servo tiembla o no se mueve: falta el GND común.
 *   - No arranca / no se puede subir el código con el servo
 *     conectado en GPIO 12: desconectar el cable de señal del
 *     GPIO 12 al subir el código y volver a conectarlo después.
 *     (GPIO 12 decide el voltaje de la memoria al arrancar.)
 */

// ---- Pines ----
#define PIN_PAN   13
#define PIN_TILT  12

// ---- Límites (ajustar si el soporte choca) ----
// Con el montaje "servo sobre servo" la cámara puede chocar con la
// escuadra en L o con la caja: probar con los comandos t/p y anotar
// el último ángulo seguro de cada lado.
// MODO SEGURO: rango pequeño alrededor del centro para la primera
// prueba con todo armado. Ampliar de a poco cuando se sepa hasta
// dónde llega cada servo sin chocar (meta: pan 0-180, tilt 45-135).
#define PAN_MIN   70
#define PAN_MAX  110
#define TILT_MIN  75
#define TILT_MAX 105

// Amplitud del barrido de prueba (grados a cada lado del centro)
#define BARRIDO_GRADOS 15

// ---- Sentido de giro ----
// Con "t100" la cámara debe mirar MÁS ARRIBA que con "t90".
// Si mira más abajo, cambiar TILT_INVERTIDO a true.
// Con "p100" la cámara debe girar hacia SU izquierda (tu derecha
// si la miras de frente). Si gira al revés, PAN_INVERTIDO a true.
#define PAN_INVERTIDO  false
#define TILT_INVERTIDO false

// ---- PWM para servos ----
// Canales 2 y 3 (timer 1). Los canales 0/1 (timer 0) quedan
// reservados para el reloj de la cámara cuando juntemos todo.
#define CANAL_PAN   2
#define CANAL_TILT  3
#define FREQ_SERVO 50   // 50 Hz = periodo de 20 ms
#define RESOLUCION 16   // 16 bits -> 0..65535
#define PULSO_MIN 500   // µs para 0°   (MG996R)
#define PULSO_MAX 2500  // µs para 180°

int anguloPan  = 90;
int anguloTilt = 90;

void escribirServo(int pin, int angulo) {
  if ((pin == PIN_PAN && PAN_INVERTIDO) || (pin == PIN_TILT && TILT_INVERTIDO)) {
    angulo = 180 - angulo;
  }
  uint32_t us = map(angulo, 0, 180, PULSO_MIN, PULSO_MAX);
  uint32_t duty = (us * 65535UL) / 20000UL;
  ledcWrite(pin, duty);
}

// Mueve de a 1° para no dar golpes al soporte ni picos de corriente
void moverSuave(int pin, int &actual, int destino, int minimo, int maximo) {
  destino = constrain(destino, minimo, maximo);
  int paso = (destino > actual) ? 1 : -1;
  while (actual != destino) {
    actual += paso;
    escribirServo(pin, actual);
    delay(20);  // lento: ~1 segundo cada 50°
  }
}

void moverPan(int a)  { moverSuave(PIN_PAN,  anguloPan,  a, PAN_MIN,  PAN_MAX); }
void moverTilt(int a) { moverSuave(PIN_TILT, anguloTilt, a, TILT_MIN, TILT_MAX); }

void centrar() {
  moverPan(90);
  moverTilt(90);
  Serial.println("Centrado: pan 90, tilt 90");
}

void barrido() {
  Serial.println("Barrido de prueba...");
  moverPan(90 - BARRIDO_GRADOS);  delay(500);
  moverPan(90 + BARRIDO_GRADOS);  delay(500);
  moverPan(90);                   delay(500);
  moverTilt(90 - BARRIDO_GRADOS); delay(500);
  moverTilt(90 + BARRIDO_GRADOS); delay(500);
  moverTilt(90);
  Serial.println("Barrido terminado");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== VigIA - Paso 2: servos pan-tilt ===");

  if (!ledcAttachChannel(PIN_PAN, FREQ_SERVO, RESOLUCION, CANAL_PAN) ||
      !ledcAttachChannel(PIN_TILT, FREQ_SERVO, RESOLUCION, CANAL_TILT)) {
    Serial.println("ERROR: no se pudo configurar el PWM de los servos");
    return;
  }

  // Posición inicial sin movimiento suave (aún no sabemos dónde están)
  escribirServo(PIN_PAN, anguloPan);
  escribirServo(PIN_TILT, anguloTilt);
  delay(800);

  barrido();
  Serial.println("Comandos: p<angulo>, t<angulo>, c (centrar), b (barrido)");
}

void loop() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  char tipo = tolower(cmd.charAt(0));
  int valor = cmd.substring(1).toInt();

  switch (tipo) {
    case 'p':
      moverPan(valor);
      Serial.printf("Pan -> %d\n", anguloPan);
      break;
    case 't':
      moverTilt(valor);
      Serial.printf("Tilt -> %d\n", anguloTilt);
      break;
    case 'c':
      centrar();
      break;
    case 'b':
      barrido();
      break;
    default:
      Serial.println("Comando no reconocido");
  }
}
