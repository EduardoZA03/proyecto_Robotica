#include <Servo.h> // Incluye la librería estándar para controlar servos

Servo miServo;  // Crea un objeto tipo Servo para controlar el motor

int pinServo = 9; // Define el pin digital donde conectarás la señal del servo

void setup() {
  miServo.attach(pinServo); // Vincula el objeto 'miServo' al pin digital 9
}

void loop() {
  // Gira el servo de 0 a 180 grados de forma gradual
  for (int angulo = 0; angulo <= 180; angulo += 1) { 
    miServo.write(angulo);              // Indica al servo que vaya a la posición del ángulo
    delay(15);                          // Espera 15 milisegundos para que el servo alcance la posición
  }
  
  delay(1000); // Pausa de 1 segundo al llegar a los 180 grados

  // Regresa el servo de 180 a 0 grados de forma gradual
  for (int angulo = 180; angulo >= 0; angulo -= 1) { 
    miServo.write(angulo);              // Indica al servo que vaya a la posición del ángulo
    delay(15);                          // Espera 15 milisegundos
  }
  
  delay(1000); // Pausa de 1 segundo al regresar a los 0 grados
}
