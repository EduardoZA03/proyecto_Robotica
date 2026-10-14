/*
 * VigIA - Paso 5: Seguimiento de rostros con IA (TinyML)
 * ------------------------------------------------------------
 * El ESP32-CAM es el "cerebro":
 *   1. Toma una imagen (320x240).
 *   2. El modelo FOMO de Edge Impulse busca rostros (entrada 96x96).
 *   3. Calcula cuánto está corrido el rostro respecto al centro.
 *   4. Le manda al Arduino la nueva posición: "P95 T88".
 * Si no ve rostros durante un rato, entra en MODO BÚSQUEDA y recorre
 * el lugar lentamente de lado a lado.
 *
 * Además envía al Arduino líneas de información que empiezan con "#"
 * (rostros detectados y milisegundos que tarda la IA). Se ven en el
 * Monitor Serie del Arduino (9600) como "[ESP32 info] ...".
 *
 * Conexiones (modo USO):
 *   ESP32 IO13 -> Arduino pin 2      ESP32 GND -> GND común
 *   ESP32 5V   -> 5V (Arduino o cargador)
 *
 * Requiere la librería exportada de Edge Impulse:
 *   MUCKCITO-project-1_inferencing  (en Documentos\Arduino\libraries)
 *
 * Funciona en ciclos "mover, esperar, mirar": después de cada movimiento
 * espera a que los servos se detengan antes de tomar la siguiente foto
 * (una foto movida hace que la IA no reconozca rostros).
 *
 * AJUSTES rápidos (sección "Seguimiento"):
 *   - Si la cámara se ALEJA del rostro en horizontal: cambiar
 *     INVERTIR_PAN a true. En vertical: INVERTIR_TILT a true.
 *   - Si se mueve muy poco por ciclo: subir GANANCIA. Si se pasa de
 *     largo y oscila: bajarla.
 */

#include <MUCKCITO-project-1_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"
#include "img_converters.h"

// ---- Pines de la cámara (AI-Thinker) ----
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ---- Enlace con el Arduino (UART2, solo TX) ----
#define PIN_TX_ARDUINO 13
#define PIN_RX_ARDUINO 12   // no se conecta (no usar GPIO16: es la PSRAM)
HardwareSerial enlace(2);

// ---- Imagen ----
#define CAM_ANCHO 320
#define CAM_ALTO  240
// El modelo recorta el centro cuadrado (240x240) y lo reduce a 96x96.
// Campo de visión de la OV2640 ~65° en 320 px -> ~49° en el recorte.
#define GRADOS_POR_PIXEL_MODELO (49.0 / EI_CLASSIFIER_INPUT_WIDTH)

// ---- Seguimiento ----
// La cámara está montada de cabeza; la imagen se gira 180° en
// iniciarCamara. Prueba del 13 oct con false/false: se iba a los
// extremos; el usuario observa que el pan es el que va al revés.
#define INVERTIR_PAN   true   // true si la cámara se aleja del rostro (horizontal)
// (13 oct, tras corregir los cables intercambiados) con false el tilt se
// alejaba: y36→24→12→4 mientras T subía 99→120. Ahora true.
#define INVERTIR_TILT  true   // true si se aleja del rostro (vertical)
// (13 oct) Más agresivo: como se espera a que el brazo se detenga
// antes de mirar, se puede corregir casi todo el error de una vez.
#define GANANCIA       0.65   // fracción del error que se corrige por ciclo (0.85 se pasaba)
#define MARGEN_CONF    0.15   // con varios rostros, ignorar los que tengan
                              // esta confianza menos que el mejor
#define ZONA_MUERTA    4      // píxeles (del 96x96) en los que no se mueve
#define PASO_MAX       15     // grados máximos de corrección por ciclo
#define UMBRAL         0.30   // confianza mínima para aceptar un rostro

// ---- "Mover, esperar, mirar" ----
// Si se toma la foto mientras los servos se mueven, sale MOVIDA y la IA
// no reconoce rostros. Por eso, después de cada movimiento se espera a
// que el Arduino termine (mismo perfil de velocidad que usa él) más un
// pequeño margen para que el brazo deje de vibrar.
#define ARDUINO_VEL   50.0   // °/s  (VEL_MAX del Arduino)  ¡mantener iguales!
#define ARDUINO_ACEL  60.0   // °/s² (ACEL del Arduino)
#define ASENTAR_MS    120    // margen para que el brazo se quede quieto

// ---- Límites (iguales a los del Arduino) ----
#define PAN_MIN     50
#define PAN_MAX    160
#define PAN_CENTRO 110
#define TILT_MIN    10
#define TILT_MAX   120
#define TILT_CENTRO 70

// ---- Al perder el rostro (13 oct) ----
// La IA no reconoce el rostro en todas las imágenes aunque la persona
// siga ahí, así que NO se sale a buscar al primer fallo:
//   fallos 1..ESPERA_QUIETO        -> quieto, vuelve a mirar (rápido: no mueve)
//   fallo ESPERA_QUIETO+1          -> si estaba en el borde de la imagen,
//                                     gira hacia ese borde; si no, mira cerca
//   después                        -> barrido completo de lado a lado
#define ESPERA_QUIETO   2
#define BORDE_PX       24      // a menos de esto del borde (de 96) = "se iba"
#define PASO_BORDE     20      // grados al girar hacia el borde
#define BUSQ_PAN_MIN   60
#define BUSQ_PAN_MAX  155
#define BUSQ_PASO      25      // grados por ciclo en el barrido (la imagen abarca ~49°)

uint8_t *imagen = nullptr;     // RGB888 320x240 (en PSRAM)

float panCmd = PAN_CENTRO, tiltCmd = TILT_CENTRO;
int ultimoPanEnviado = -1, ultimoTiltEnviado = -1;
int ciclosSinRostro = 0;
int sentidoBusqueda = 1;
float ultimoX = -1, ultimoY = -1;   // último rostro seguido (coords 96x96)
float vistoX = -1;                  // posición X donde se vio el rostro por última vez
float panVisto = PAN_CENTRO;               // pan en el que se vio por última vez

// -------------------------------------------------------------------
bool iniciarCamara() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;          // 10 MHz: más estable
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA;    // 320x240
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST; // siempre la imagen más reciente
  if (esp_camera_init(&config) != ESP_OK) return false;

  // La cámara está montada de cabeza: girar la imagen 180° para que la
  // IA vea los rostros derechos (así fue entrenada)
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
  return true;
}

bool capturar() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;
  bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, imagen);
  esp_camera_fb_return(fb);
  if (!ok) return false;
  // Recorta el centro y reduce al tamaño del modelo (en el mismo buffer)
  ei::image::processing::crop_and_interpolate_rgb888(
      imagen, CAM_ANCHO, CAM_ALTO,
      imagen, EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);
  return true;
}

// Entrega los píxeles al modelo (empaquetados como 0xRRGGBB)
static int obtenerDatos(size_t offset, size_t largo, float *salida) {
  size_t ix = offset * 3;
  for (size_t i = 0; i < largo; i++, ix += 3) {
    // La cámara entrega BGR: se reordena a RGB
    salida[i] = (imagen[ix + 2] << 16) + (imagen[ix + 1] << 8) + imagen[ix];
  }
  return 0;
}

// Tiempo (ms) que tarda el Arduino en recorrer 'grados' con su perfil
// de aceleración (trapecio o triángulo de velocidad)
unsigned long tiempoMovimiento(float grados) {
  if (grados <= 0) return 0;
  float dAcel = ARDUINO_VEL * ARDUINO_VEL / ARDUINO_ACEL;  // distancia para acelerar y frenar
  float t = (grados <= dAcel)
              ? 2.0 * sqrt(grados / ARDUINO_ACEL)
              : grados / ARDUINO_VEL + ARDUINO_VEL / ARDUINO_ACEL;
  return (unsigned long)(t * 1000);
}

// Envía la posición y ESPERA a que los servos terminen de moverse,
// para que la siguiente foto salga nítida
void enviarPosicion() {
  int p = (int)round(panCmd), t = (int)round(tiltCmd);
  if (p == ultimoPanEnviado && t == ultimoTiltEnviado) return;
  float grados = 0;
  if (ultimoPanEnviado >= 0) {
    grados = max(abs(p - ultimoPanEnviado), abs(t - ultimoTiltEnviado));
  }
  enlace.printf("P%d T%d\n", p, t);
  ultimoPanEnviado = p;
  ultimoTiltEnviado = t;

  delay(tiempoMovimiento(grados) + ASENTAR_MS);
  // Descartar la imagen que se tomó durante el movimiento
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
}

void info(const char *texto) {
  enlace.printf("#%s\n", texto);   // el Arduino lo muestra como [ESP32 info]
  Serial.println(texto);
}

// -------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  enlace.begin(9600, SERIAL_8N1, PIN_RX_ARDUINO, PIN_TX_ARDUINO);
  delay(500);
  Serial.println("\n=== VigIA - Paso 5: seguimiento de rostros ===");

  imagen = (uint8_t *)ps_malloc(CAM_ANCHO * CAM_ALTO * 3);
  if (!imagen) { info("ERROR sin memoria PSRAM"); while (true) delay(1000); }
  if (!iniciarCamara()) { info("ERROR camara"); while (true) delay(1000); }

  // Descartar las primeras imágenes (exposición automática)
  for (int i = 0; i < 5; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(100);
  }
  info("VigIA listo: buscando rostros");
  enviarPosicion();   // centro
}

void loop() {
  if (!capturar()) { info("ERROR captura"); delay(500); return; }

  ei::signal_t senal;
  senal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  senal.get_data = &obtenerDatos;

  ei_impulse_result_t res = { 0 };
  if (run_classifier(&senal, &res, false) != EI_IMPULSE_OK) {
    info("ERROR al ejecutar el modelo");
    return;
  }

  // ---- Elegir el rostro a seguir ----
  // 1) Solo se consideran los de confianza alta (cerca del mejor), para no
  //    perseguir detecciones dudosas (ventana, monitor...).
  // 2) Entre ellos, el más cercano al último seguido (o al centro).
  float mejorConf = 0;
  for (uint32_t i = 0; i < res.bounding_boxes_count; i++) {
    if (res.bounding_boxes[i].value > mejorConf) mejorConf = res.bounding_boxes[i].value;
  }
  float refX = (ultimoX < 0) ? EI_CLASSIFIER_INPUT_WIDTH / 2.0 : ultimoX;
  float refY = (ultimoY < 0) ? EI_CLASSIFIER_INPUT_HEIGHT / 2.0 : ultimoY;
  int rostros = 0, elegido = -1;
  float mejorDist = 1e9;
  for (uint32_t i = 0; i < res.bounding_boxes_count; i++) {
    ei_impulse_result_bounding_box_t &b = res.bounding_boxes[i];
    if (b.value < UMBRAL) continue;
    rostros++;
    if (b.value < mejorConf - MARGEN_CONF) continue;   // dudoso frente al mejor
    float cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
    float d = (cx - refX) * (cx - refX) + (cy - refY) * (cy - refY);
    if (d < mejorDist) { mejorDist = d; elegido = i; }
  }

  // Línea de diagnóstico: cuántos, tiempo de IA y dónde está el elegido
  // (x,y en la imagen de 96x96: centro = 48,48; c = confianza en %)
  char linea[64];
  int ms = (int)(res.timing.dsp + res.timing.classification);
  if (elegido >= 0) {
    ei_impulse_result_bounding_box_t &b = res.bounding_boxes[elegido];
    snprintf(linea, sizeof(linea), "rostros %d | IA %d | x%d y%d c%d", rostros, ms,
             (int)(b.x + b.width / 2), (int)(b.y + b.height / 2), (int)(b.value * 100));
  } else {
    snprintf(linea, sizeof(linea), "rostros %d | IA %d", rostros, ms);
  }
  info(linea);

  if (elegido >= 0) {
    // ---- MODO SEGUIMIENTO ----
    ciclosSinRostro = 0;
    ei_impulse_result_bounding_box_t &b = res.bounding_boxes[elegido];
    float cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
    ultimoX = cx; ultimoY = cy;

    float errX = cx - EI_CLASSIFIER_INPUT_WIDTH / 2.0;   // + = rostro a la derecha de la imagen
    float errY = cy - EI_CLASSIFIER_INPUT_HEIGHT / 2.0;  // + = rostro abajo en la imagen

    if (fabs(errX) > ZONA_MUERTA) {
      float paso = constrain(GANANCIA * errX * GRADOS_POR_PIXEL_MODELO, -PASO_MAX, PASO_MAX);
      // Rostro a la derecha de la imagen -> la cámara gira a SU derecha,
      // que para el Arduino es un pan MENOR (mayor = derecha del usuario)
      panCmd += INVERTIR_PAN ? paso : -paso;
    }
    if (fabs(errY) > ZONA_MUERTA) {
      float paso = constrain(GANANCIA * errY * GRADOS_POR_PIXEL_MODELO, -PASO_MAX, PASO_MAX);
      // Rostro abajo en la imagen -> mirar más abajo = tilt MENOR
      tiltCmd += INVERTIR_TILT ? paso : -paso;
    }
    vistoX = cx;          // dónde quedó en la imagen
    panVisto = panCmd;    // y hacia dónde apunta ahora la cámara
  } else {
    // ---- ROSTRO PERDIDO ----
    ciclosSinRostro++;
    ultimoX = ultimoY = -1;
    // Sentido de pan que corresponde al borde DERECHO de la imagen
    int haciaDerechaImagen = INVERTIR_PAN ? 1 : -1;

    if (ciclosSinRostro <= ESPERA_QUIETO) {
      // Quieto: probablemente sigue ahí y la IA solo falló esta vez
    } else if (ciclosSinRostro == ESPERA_QUIETO + 1) {
      if (vistoX >= 0 && vistoX > EI_CLASSIFIER_INPUT_WIDTH - BORDE_PX) {
        sentidoBusqueda = haciaDerechaImagen;            // se iba por la derecha
        panCmd = panVisto + sentidoBusqueda * PASO_BORDE;
      } else if (vistoX >= 0 && vistoX < BORDE_PX) {
        sentidoBusqueda = -haciaDerechaImagen;           // se iba por la izquierda
        panCmd = panVisto + sentidoBusqueda * PASO_BORDE;
      } else {
        panCmd = panVisto + sentidoBusqueda * PASO_BORDE; // mirar cerca, a un lado
      }
    } else if (ciclosSinRostro == ESPERA_QUIETO + 2 &&
               vistoX >= BORDE_PX && vistoX <= EI_CLASSIFIER_INPUT_WIDTH - BORDE_PX) {
      panCmd = panVisto - sentidoBusqueda * PASO_BORDE;   // mirar cerca, al otro lado
      sentidoBusqueda = -sentidoBusqueda;
    } else {
      // Barrido completo
      panCmd += sentidoBusqueda * BUSQ_PASO;
      if (panCmd >= BUSQ_PAN_MAX) { panCmd = BUSQ_PAN_MAX; sentidoBusqueda = -1; }
      if (panCmd <= BUSQ_PAN_MIN) { panCmd = BUSQ_PAN_MIN; sentidoBusqueda = 1; }
      tiltCmd += (TILT_CENTRO - tiltCmd) * 0.3;   // volver poco a poco al centro
      vistoX = -1;
    }
  }

  panCmd  = constrain(panCmd,  PAN_MIN,  PAN_MAX);
  tiltCmd = constrain(tiltCmd, TILT_MIN, TILT_MAX);
  enviarPosicion();
}
