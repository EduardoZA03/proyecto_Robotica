/*
 * VigIA - Prueba de los pines de servo del Arduino Uno (sin multímetro)
 * ---------------------------------------------------------------------
 * Genera la señal de servo en los pines 9 y 10 (igual que 03_arduino_servos)
 * y la MIDE por el pin 7 con pulseIn().
 *
 * Conexión: un jumper del pin 9 (o del 10) al pin 7. No hace falta servo.
 * Abrir el Monitor Serie a 9600: cada segundo muestra el ancho del pulso.
 *   ~1000-2000 us  -> el pin SÍ saca la señal de servo (el Uno está bien)
 *   0 (sin pulso)  -> el pin no saca señal o el jumper está mal puesto
 * El ancho cambia solo (barre de 60° a 150°) para ver que responde.
 *
 * Al terminar, volver a subir 03_arduino_servos.
 */
#include <Servo.h>

Servo pan, tilt;

void setup() {
  Serial.begin(9600);
  pan.attach(9);
  tilt.attach(10);
  pinMode(7, INPUT);
  Serial.println(F("=== Prueba de pines de servo: jumper del pin 9 (o 10) al pin 7 ==="));
}

void loop() {
  static int angulo = 60, paso = 30;
  pan.write(angulo);
  tilt.write(angulo);
  delay(300);
  unsigned long ancho = pulseIn(7, HIGH, 50000UL);   // espera hasta 50 ms
  Serial.print(F("angulo "));
  Serial.print(angulo);
  Serial.print(F(" -> pulso medido en pin 7: "));
  Serial.print(ancho);
  Serial.println(ancho ? F(" us  (SEÑAL OK)") : F(" us  (SIN SEÑAL)"));
  angulo += paso;
  if (angulo >= 150 || angulo <= 60) paso = -paso;
  delay(700);
}
