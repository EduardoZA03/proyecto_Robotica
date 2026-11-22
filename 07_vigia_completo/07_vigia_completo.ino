#include <MUCKCITO-project-1_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <time.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "secretos.h"

#define VIGILANTE_S  90                       // segundos máximos de un ciclo antes de reiniciar
#define MARCA_VIGILANTE 0x56494721UL          // marca que indica "reinició el vigilante"
RTC_NOINIT_ATTR char pasoGuardado[20];        // último paso del ciclo (sobrevive al reinicio)
RTC_NOINIT_ATTR uint32_t marcaVigilante;      // guarda MARCA_VIGILANTE antes de reiniciar
volatile unsigned long inicioCiclo = 0;       // momento (ms) en que empezó el ciclo actual

#define paso(p) do { strncpy(pasoGuardado, (p), sizeof(pasoGuardado) - 1); \
                     pasoGuardado[sizeof(pasoGuardado) - 1] = 0; } while (0)

#ifndef PC_IP
#define PC_IP ""                              // IP de la PC (vacía = trabajar sin PC)
#endif
#define PC_PUERTO        8000                 // puerto del servidor de Python

#define PC_TIMEOUT_MS    5000                 // espera máxima de una respuesta de la PC
#define ESCANEO_MS       4000                 // cada cuánto se pide a la PC que revise la imagen
#define GRADOS_POR_PIXEL_CAM (49.0 / CAM_ALTO) // grados por píxel en la foto completa
#define PC_REINTENTO_MS  30000                // espera antes de volver a probar una PC caída

#define PC_FALLA      0                       // respuestas posibles de la PC
#define PC_NUEVA      1
#define PC_CONOCIDA   2
#define PC_SIN_ROSTRO 3
#define PC_REPETIR    4
#define ID_SIN_ROSTRO 0                       // número de "persona" para lo que no es un rostro

#define PWDN_GPIO_NUM     32                  // pines de la cámara (placa AI-Thinker)
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

#define PIN_TX_ARDUINO 13                     // pin que envía las órdenes al Arduino Uno
#define PIN_RX_ARDUINO 12                     // pin de recepción (no se usa)
HardwareSerial enlace(2);                     // puerto serie hacia el Arduino Uno

#define CAM_ANCHO 640                         // ancho de la foto
#define CAM_ALTO  480                         // alto de la foto
#define RECORTE_X ((CAM_ANCHO - CAM_ALTO) / 2) // borde que se recorta para dejarla cuadrada
#define ESCALA    ((float)CAM_ALTO / EI_CLASSIFIER_INPUT_WIDTH)   // píxeles de foto por píxel del modelo
#define GRADOS_POR_PIXEL_MODELO (49.0 / EI_CLASSIFIER_INPUT_WIDTH) // grados por píxel del modelo
#define JPG_MAX   (150 * 1024)                // tamaño máximo de una foto JPEG

#define INVERTIR_PAN   true                   // invierte el sentido del pan (cámara de cabeza)
#define INVERTIR_TILT  true                   // invierte el sentido del tilt
#define GANANCIA       0.65                   // fracción del error que se corrige en cada paso
#define MARGEN_CONF    0.15                   // diferencia de confianza aceptada respecto al mejor rostro
#define ZONA_MUERTA    4                      // error (píxeles) que se considera centrado
#define PASO_MAX       15                     // máximo de grados por corrección
#define UMBRAL         0.30                   // confianza mínima para aceptar un rostro

#define ARDUINO_VEL   50.0                    // velocidad de los servos (igual que en el Uno)
#define ARDUINO_ACEL  60.0                    // aceleración de los servos (igual que en el Uno)
#define ASENTAR_MS    120                     // espera extra para que el brazo se detenga

#define PAN_MIN     50                        // límites y centro de los servos
#define PAN_MAX    160
#define PAN_CENTRO 110
#define TILT_MIN    10
#define TILT_MAX   120
#define TILT_CENTRO 70

#define ESPERA_QUIETO   2                     // ciclos sin rostro antes de empezar a buscar
#define BORDE_PX       24                     // píxeles desde el borde que cuentan como "salió por el borde"
#define PASO_BORDE     20                     // grados que gira hacia el borde por donde salió
#define BUSQ_PAN_MIN   60                     // límites del recorrido de búsqueda
#define BUSQ_PAN_MAX  155
#define BUSQ_PASO      25                     // grados por paso de búsqueda

#define VENTANA_MS     (2UL * 60UL * 1000UL)  // tiempo que se ignora a una persona ya fotografiada
#define CENTRADO_PX    8                      // error máximo (píxeles) para tomar la foto
#define CONFIRMACIONES 2                      // ciclos centrado seguidos antes de la foto
#define ESPERA_INICIO_MS 3000                 // espera al arrancar antes de tomar fotos
#define CONF_FOTO      0.60                   // confianza mínima para tomar la foto

#define UMBRAL_MISMA   0.92                   // similitud de color para decir "misma persona" (sin PC)

#define UMBRAL_IGNORAR 0.96                   // similitud de color para ignorar de lejos
#define CICLOS_FIN_SEGUIMIENTO 4              // ciclos sin rostro para olvidar el último aviso
#define MAX_PERSONAS   30                     // personas recordadas a la vez
#define MAX_FOTOS      40                     // fotos guardadas en la flash

uint8_t *imagen  = nullptr;                   // foto completa en RGB
uint8_t *entrada = nullptr;                   // imagen reducida para el modelo
uint8_t *ultimaJpg = nullptr;                 // copia JPEG de la última foto
size_t   ultimaJpgLen = 0;                    // tamaño de esa copia

float panCmd = PAN_CENTRO, tiltCmd = TILT_CENTRO;   // ángulos que se quieren
int ultimoPanEnviado = -1, ultimoTiltEnviado = -1;  // últimos ángulos enviados al Uno
int ciclosSinRostro = 0;                      // ciclos seguidos sin rostro nuevo
int sentidoBusqueda = 1;                      // sentido del giro al buscar (1 o -1)
float ultimoX = -1, ultimoY = -1;             // posición del último rostro seguido
float vistoX = -1;                            // x del último rostro visto (para saber por dónde salió)
float panVisto = PAN_CENTRO;                  // pan cuando se vio ese rostro

#define NFIRMA 64                             // números de la firma de color
struct Persona {
  bool activa;                                // true = se recuerda
  int id;                                     // número de persona
  float firma[NFIRMA];                        // firma de color de la cabeza
  unsigned long ultimaFoto;                   // momento de su última foto
  unsigned long verificada;                   // último momento en que la PC la confirmó
};

#define VERIF_MS  20000UL                     // tras este tiempo se vuelve a confirmar con la PC

#define EVITAR_N       4                      // lugares que se pueden evitar a la vez
#define EVITAR_GRADOS  10.0                   // radio (grados) de un lugar evitado
float evitarPan[EVITAR_N], evitarTilt[EVITAR_N];   // posición de cada lugar evitado
unsigned long evitarHasta[EVITAR_N] = {0};    // hasta cuándo se evita cada lugar
Persona personas[MAX_PERSONAS];               // personas recordadas
int siguienteIdPersona = 1;                   // número para la próxima persona nueva
int ultimaPersonaAvisada = -1;                // última persona anunciada (evita repetir avisos)
int centradoSeguidas = 0;                     // ciclos seguidos con el rostro centrado

struct Foto {
  bool usada;                                 // true = la casilla tiene foto
  int n;                                      // número de foto
  int persona;                                // número de persona
  time_t hora;                                // hora en que se tomó
};
Foto fotos[MAX_FOTOS];                        // lista de fotos guardadas
int totalFotos = 0;                           // fotos tomadas desde siempre
SemaphoreHandle_t candado;                    // evita que la galería y el loop usen las fotos a la vez
Preferences prefs;                            // memoria donde se guarda totalFotos
bool hayFlash = false;                        // true = la memoria flash funciona

// Devuelve el nombre de archivo de una foto
String rutaFoto(const Foto &f) {
  return "/f" + String(f.n) + "_p" + String(f.persona) + "_t" + String((long)f.hora) + ".jpg";
}

WebServer servidor(80);                       // servidor web de la galería
bool hayWiFi = false;                         // true = conectado al WiFi

uint32_t idArranque = 0;                      // número al azar de cada arranque (evita la caché del navegador)

// Configura e inicia la cámara
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
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&config) != ESP_OK) return false;

  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
  return true;
}

void reducirAModelo();

// Toma una foto, guarda su copia JPEG y la prepara para el modelo
bool capturar() {
  paso("camara");
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;
  if (fb->len <= JPG_MAX) {
    memcpy(ultimaJpg, fb->buf, fb->len);
    ultimaJpgLen = fb->len;
  } else {
    ultimaJpgLen = 0;
  }
  paso("decodificar");
  bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, imagen);
  esp_camera_fb_return(fb);
  if (!ok) return false;
  reducirAModelo();
  return true;
}

// Recorta la foto al centro y la achica al tamaño del modelo (96x96)
void reducirAModelo() {
  const int lado = EI_CLASSIFIER_INPUT_WIDTH;
  const int bloque = (int)ESCALA;
  for (int v = 0; v < lado; v++) {
    for (int u = 0; u < lado; u++) {
      int sB = 0, sG = 0, sR = 0;
      int x0 = RECORTE_X + u * bloque, y0 = v * bloque;
      for (int dy = 0; dy < bloque; dy++) {
        const uint8_t *px = imagen + ((y0 + dy) * CAM_ANCHO + x0) * 3;
        for (int dx = 0; dx < bloque; dx++, px += 3) {
          sB += px[0]; sG += px[1]; sR += px[2];
        }
      }
      const int n = bloque * bloque;
      uint8_t *out = entrada + (v * lado + u) * 3;
      out[0] = sB / n; out[1] = sG / n; out[2] = sR / n;
    }
  }
}

// Entrega los píxeles de la imagen reducida al modelo de Edge Impulse
static int obtenerDatos(size_t offset, size_t largo, float *salida) {
  size_t ix = offset * 3;
  for (size_t i = 0; i < largo; i++, ix += 3) {
    salida[i] = (entrada[ix + 2] << 16) + (entrada[ix + 1] << 8) + entrada[ix];
  }
  return 0;
}

// Calcula cuánto tarda el Uno en girar esos grados (ms)
unsigned long tiempoMovimiento(float grados) {
  if (grados <= 0) return 0;
  float dAcel = ARDUINO_VEL * ARDUINO_VEL / ARDUINO_ACEL;
  float t = (grados <= dAcel)
              ? 2.0 * sqrt(grados / ARDUINO_ACEL)
              : grados / ARDUINO_VEL + ARDUINO_VEL / ARDUINO_ACEL;
  return (unsigned long)(t * 1000);
}

// Envía la nueva posición al Uno y espera a que el brazo se detenga
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
  paso("mover");
  delay(tiempoMovimiento(grados) + ASENTAR_MS);
  paso("descartar");
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
}

// Envía un mensaje al Uno (lo muestra en su monitor serie) y al monitor del ESP32
void info(const char *texto) {
  enlace.printf("#%s\n", texto);
  Serial.println(texto);
}

// Calcula la firma de color de la cabeza en tres franjas (pelo, ojos, boca)
void calcularFirma(float cx, float cy, float firma[NFIRMA]) {
  int fx = RECORTE_X + (int)(cx * ESCALA);
  int fy = (int)(cy * ESCALA);
  const int limites[4] = { fy - 70, fy - 25, fy + 15, fy + 55 };
  int x0 = max(0, fx - 45), x1 = min(CAM_ANCHO - 1, fx + 45);
  for (int i = 0; i < NFIRMA; i++) firma[i] = 0;
  float totalFranja[3] = {0, 0, 0}, totalBrillo = 0;

  for (int f = 0; f < 3; f++) {
    int y0 = max(0, limites[f]), y1 = min(CAM_ALTO - 1, limites[f + 1]);
    for (int y = y0; y < y1; y += 2) {
      for (int x = x0; x <= x1; x += 2) {
        uint8_t *px = imagen + (y * CAM_ANCHO + x) * 3;
        int suma = px[0] + px[1] + px[2];
        firma[48 + min(15, suma / 48)] += 1;
        totalBrillo += 1;
        if (suma < 60) continue;
        int r = (px[2] * 4) / (suma + 1);
        int g = (px[1] * 4) / (suma + 1);
        firma[f * 16 + r * 4 + g] += 1;
        totalFranja[f] += 1;
      }
    }
  }

  for (int f = 0; f < 3; f++)
    if (totalFranja[f] > 0) for (int k = 0; k < 16; k++) firma[f * 16 + k] *= 0.25 / totalFranja[f];
  if (totalBrillo > 0) for (int k = 48; k < 64; k++) firma[k] *= 0.25 / totalBrillo;
}

// Compara dos firmas de color (0 = distintas, 1 = iguales)
float similitud(const float *a, const float *b) {
  float s = 0;
  for (int i = 0; i < NFIRMA; i++) s += min(a[i], b[i]);
  return s;
}

// Busca la persona recordada más parecida; olvida a las que pasaron los 2 minutos
int buscarPersona(const float *firma, float umbral, float *simMax) {
  int mejor = -1;
  float mejorSim = 0;
  unsigned long ahora = millis();
  for (int i = 0; i < MAX_PERSONAS; i++) {
    if (!personas[i].activa) continue;
    if (ahora - personas[i].ultimaFoto > VENTANA_MS) {
      personas[i].activa = false;
      continue;
    }
    float s = similitud(firma, personas[i].firma);
    if (s > mejorSim) { mejorSim = s; mejor = i; }
  }
  if (simMax) *simMax = mejorSim;
  return (mejorSim >= umbral) ? mejor : -1;
}

// Guarda una persona nueva (si no hay espacio, reemplaza a la más antigua)
int nuevaPersona(const float *firma, int id) {
  int libre = -1;
  unsigned long masVieja = 0;
  for (int i = 0; i < MAX_PERSONAS; i++) {
    if (!personas[i].activa) { libre = i; break; }
    unsigned long edad = millis() - personas[i].ultimaFoto;
    if (edad >= masVieja) { masVieja = edad; libre = i; }
  }
  Persona &p = personas[libre];
  p.activa = true;
  p.id = id;
  if (id >= siguienteIdPersona) siguienteIdPersona = id + 1;
  memcpy(p.firma, firma, sizeof(p.firma));
  p.ultimaFoto = millis();
  p.verificada = millis();
  return libre;
}

// Busca una persona recordada por su número
int buscarPorId(int id) {
  for (int i = 0; i < MAX_PERSONAS; i++)
    if (personas[i].activa && personas[i].id == id) return i;
  return -1;
}

// Borra la foto más antigua de la flash
bool borrarMasVieja() {
  int idx = -1;
  for (int i = 0; i < MAX_FOTOS; i++)
    if (fotos[i].usada && (idx < 0 || fotos[i].n < fotos[idx].n)) idx = i;
  if (idx < 0) return false;
  LittleFS.remove(rutaFoto(fotos[idx]));
  fotos[idx].usada = false;
  return true;
}

// Guarda la última foto en la flash con el número de persona
bool guardarFoto(int idPersona) {
  if (ultimaJpgLen == 0 || !hayFlash) return false;
  xSemaphoreTake(candado, portMAX_DELAY);

  Foto &f = fotos[totalFotos % MAX_FOTOS];
  if (f.usada) { LittleFS.remove(rutaFoto(f)); f.usada = false; }

  while (LittleFS.totalBytes() - LittleFS.usedBytes() < ultimaJpgLen + 16 * 1024) {
    if (!borrarMasVieja()) break;
  }

  Foto nueva = { true, totalFotos, idPersona, time(nullptr) };
  if (nueva.hora < 1700000000) nueva.hora = 0;
  File arch = LittleFS.open(rutaFoto(nueva), "w");
  bool ok = arch && arch.write(ultimaJpg, ultimaJpgLen) == ultimaJpgLen;
  if (arch) arch.close();
  if (ok) {
    f = nueva;
    totalFotos++;
    prefs.putInt("total", totalFotos);
  } else {
    LittleFS.remove(rutaFoto(nueva));
  }
  xSemaphoreGive(candado);
  return ok;
}

// Monta la flash y carga la lista de fotos que ya estaban guardadas
void iniciarFlash() {
  prefs.begin("vigia", false);
  totalFotos = prefs.getInt("total", 0);
  if (!LittleFS.begin(true)) {
    info("ERROR flash: fotos no se guardaran");
    return;
  }
  hayFlash = true;
  File raiz = LittleFS.open("/");
  File arch = raiz.openNextFile();
  int cargadas = 0;
  while (arch) {
    int n, p; long t;
    String nombre = arch.name();
    arch.close();
    if (sscanf(nombre.c_str(), "f%d_p%d_t%ld.jpg", &n, &p, &t) == 3 &&
        n < totalFotos && n >= totalFotos - MAX_FOTOS) {
      fotos[n % MAX_FOTOS] = { true, n, p, (time_t)t };
      cargadas++;
    } else {
      LittleFS.remove("/" + nombre);
    }
    arch = raiz.openNextFile();
  }
  char linea[64];
  snprintf(linea, sizeof(linea), "Flash: %d fotos guardadas (total %d)", cargadas, totalFotos);
  info(linea);
}

// Avisa (una sola vez) que se ignora a una persona ya registrada
void avisarIgnorada(int i) {
  if (ultimaPersonaAvisada == personas[i].id) return;
  char linea[64];
  if (personas[i].id == ID_SIN_ROSTRO) {
    snprintf(linea, sizeof(linea), "objeto que no es rostro: lo ignoro");
  } else {
    snprintf(linea, sizeof(linea), "persona %d ya registrada (hace %lus): la ignoro",
             personas[i].id, (millis() - personas[i].ultimaFoto) / 1000);
  }
  info(linea);
  ultimaPersonaAvisada = personas[i].id;
}

bool pcCaida = false;                         // true = la PC no respondió
unsigned long pcCaidaDesde = 0;               // momento en que dejó de responder

// Envía la última foto a la PC (ruta /rostro o /escanear) y devuelve su respuesta
bool enviarAPC(const char *ruta, String &txt) {
  if (strlen(PC_IP) == 0 || !hayWiFi || WiFi.status() != WL_CONNECTED || ultimaJpgLen == 0)
    return false;
  if (pcCaida && millis() - pcCaidaDesde < PC_REINTENTO_MS) return false;
  bool ok = false;
  HTTPClient http;
  http.setConnectTimeout(PC_TIMEOUT_MS);
  http.setTimeout(PC_TIMEOUT_MS);
  String url = String("http://") + PC_IP + ":" + String(PC_PUERTO) + ruta;
  if (http.begin(url)) {
    http.addHeader("Content-Type", "image/jpeg");
    if (http.POST(ultimaJpg, ultimaJpgLen) == 200) {
      txt = http.getString();
      txt.trim();
      ok = txt.length() > 0 && !txt.startsWith("ERROR");
    }
    http.end();
  }
  avisarEstadoPC(ok);
  return ok;
}

// Avisa cuando la PC deja de responder o vuelve a conectarse
void avisarEstadoPC(bool ok) {
  if (!ok) {
    if (!pcCaida) info("PC no responde: uso la firma de color");
    pcCaida = true;
    pcCaidaDesde = millis();
  } else if (pcCaida) {
    info("PC conectada de nuevo");
    pcCaida = false;
  }
}

// Pregunta a la PC si el rostro centrado es nuevo, conocido, no es rostro o hay que repetir
int preguntarPC(int &persona, int &dato) {
  String txt;
  if (!enviarAPC("/rostro", txt)) return PC_FALLA;
  if (sscanf(txt.c_str(), "NUEVA %d %d", &persona, &dato) == 2)    return PC_NUEVA;
  if (sscanf(txt.c_str(), "CONOCIDA %d %d", &persona, &dato) == 2) return PC_CONOCIDA;
  if (txt == "SIN_ROSTRO")                                          return PC_SIN_ROSTRO;
  if (txt == "REPETIR")                                             return PC_REPETIR;
  return PC_FALLA;
}

// Marca la posición actual como lugar a evitar por un tiempo
void evitarAqui() {
  int k = 0;
  for (int i = 0; i < EVITAR_N; i++)
    if (evitarHasta[i] < evitarHasta[k]) k = i;
  evitarPan[k] = panCmd;
  evitarTilt[k] = tiltCmd;
  evitarHasta[k] = millis() + VERIF_MS;
  ultimoX = ultimoY = -1;
}

// Devuelve true si un rostro está en un lugar que se debe evitar
bool enLugarEvitado(float cx, float cy) {
  float pan  = panCmd  + (INVERTIR_PAN  ? 1 : -1) * (cx - EI_CLASSIFIER_INPUT_WIDTH / 2.0) * GRADOS_POR_PIXEL_MODELO;
  float tilt = tiltCmd + (INVERTIR_TILT ? 1 : -1) * (cy - EI_CLASSIFIER_INPUT_HEIGHT / 2.0) * GRADOS_POR_PIXEL_MODELO;
  for (int i = 0; i < EVITAR_N; i++) {
    if (millis() >= evitarHasta[i]) continue;
    if (fabs(pan - evitarPan[i]) < EVITAR_GRADOS && fabs(tilt - evitarTilt[i]) < EVITAR_GRADOS) return true;
  }
  return false;
}

// Devuelve true si el servo ya está en su tope y no puede girar más hacia el rostro
bool enTope(float err, float cmd, float minimo, float maximo, bool invertir) {
  float giro = invertir ? err : -err;
  return (giro < 0 && cmd <= minimo) || (giro > 0 && cmd >= maximo);
}

// Si el rostro está centrado (o el servo llegó a su tope), decide si se toma la foto
void registrar(float cx, float cy, float conf) {
  float errX = cx - EI_CLASSIFIER_INPUT_WIDTH / 2.0;
  float errY = cy - EI_CLASSIFIER_INPUT_HEIGHT / 2.0;
  if (enTope(errX, panCmd, PAN_MIN, PAN_MAX, INVERTIR_PAN)) errX = 0;
  if (enTope(errY, tiltCmd, TILT_MIN, TILT_MAX, INVERTIR_TILT)) errY = 0;
  if (millis() < ESPERA_INICIO_MS) return;
  if (fabs(errX) > CENTRADO_PX || fabs(errY) > CENTRADO_PX || conf < CONF_FOTO) {
    centradoSeguidas = 0;
    return;
  }
  if (++centradoSeguidas < CONFIRMACIONES) return;
  centradoSeguidas = 0;

  float firma[NFIRMA];
  calcularFirma(cx, cy, firma);
  char linea[64];

  int idPC = 0, dato = 0;
  paso("PC");
  int resp = preguntarPC(idPC, dato);
  if (resp == PC_NUEVA || resp == PC_CONOCIDA) {
    int j = buscarPorId(idPC);
    if (j < 0) {
      j = nuevaPersona(firma, idPC);
    } else {
      for (int k = 0; k < NFIRMA; k++)
        personas[j].firma[k] = 0.6 * personas[j].firma[k] + 0.4 * firma[k];
    }

    unsigned long hace = (resp == PC_CONOCIDA) ? (unsigned long)dato * 1000UL : 0;
    personas[j].ultimaFoto = millis() - min(hace, millis());
    personas[j].verificada = millis();
    evitarAqui();

    if (resp == PC_NUEVA) {
      snprintf(linea, sizeof(linea), "FOTO #%d persona %d (PC)", dato, idPC);
      info(linea);
      ultimaPersonaAvisada = idPC;
    } else {
      avisarIgnorada(j);
    }
    return;
  }
  if (resp == PC_REPETIR) {
    info("foto de baja calidad (dice la PC): tomo otra");
    return;
  }
  if (resp == PC_SIN_ROSTRO) {
    nuevaPersona(firma, ID_SIN_ROSTRO);
    evitarAqui();
    if (ultimaPersonaAvisada != ID_SIN_ROSTRO) info("no era un rostro (dice la PC): lo ignoro");
    ultimaPersonaAvisada = ID_SIN_ROSTRO;
    return;
  }

  float sim = 0;
  int i = buscarPersona(firma, UMBRAL_MISMA, &sim);

  if (i >= 0) {
    for (int k = 0; k < NFIRMA; k++)
      personas[i].firma[k] = 0.6 * personas[i].firma[k] + 0.4 * firma[k];
    avisarIgnorada(i);
    return;
  }

  int n = nuevaPersona(firma, siguienteIdPersona);
  if (guardarFoto(personas[n].id)) {
    snprintf(linea, sizeof(linea), "FOTO #%d persona %d (nueva, s%d)", totalFotos, personas[n].id,
             (int)(sim * 100));
  } else {
    snprintf(linea, sizeof(linea), "ERROR al guardar foto persona %d", personas[n].id);
  }
  info(linea);
  ultimaPersonaAvisada = personas[n].id;
}

unsigned long ultimoEscaneo = 0;              // momento del último escaneo con la PC
// Cuando el ESP32 no ve rostros, pide a la PC que revise la foto; si ve uno, apunta hacia él
bool escanearConPC() {
  bool pcDisponible = strlen(PC_IP) > 0 && hayWiFi && !pcCaida;
  if (!pcDisponible || millis() - ultimoEscaneo < ESCANEO_MS) return false;
  ultimoEscaneo = millis();
  paso("escaneo");
  String txt;
  if (!enviarAPC("/escanear", txt)) return false;
  int a = 0, b = 0;
  char linea[64];
  if (sscanf(txt.c_str(), "NUEVA %d %d", &a, &b) == 2) {
    float firma[NFIRMA];
    calcularFirma(EI_CLASSIFIER_INPUT_WIDTH / 2.0, EI_CLASSIFIER_INPUT_HEIGHT / 2.0, firma);
    int j = buscarPorId(a);
    if (j < 0) j = nuevaPersona(firma, a);
    personas[j].ultimaFoto = millis();
    personas[j].verificada = millis();
    evitarAqui();
    snprintf(linea, sizeof(linea), "FOTO #%d persona %d (PC, escaneo)", b, a);
    info(linea);
    ultimaPersonaAvisada = a;
    return true;
  }
  if (sscanf(txt.c_str(), "IR %d %d", &a, &b) == 2) {
    float dPan  = constrain(0.8 * a * GRADOS_POR_PIXEL_CAM, -2 * PASO_MAX, 2 * PASO_MAX);
    float dTilt = constrain(0.8 * b * GRADOS_POR_PIXEL_CAM, -2 * PASO_MAX, 2 * PASO_MAX);
    panCmd  += INVERTIR_PAN  ? dPan  : -dPan;
    tiltCmd += INVERTIR_TILT ? dTilt : -dTilt;
    ciclosSinRostro = 0;
    ultimoEscaneo = 0;
    snprintf(linea, sizeof(linea), "PC ve un rostro sin registrar: apunto (%d, %d)", a, b);
    info(linea);
    return true;
  }
  return false;
}

// Escribe la hora de una foto como texto
void textoHora(const Foto &f, char *buf, size_t n) {
  if (f.hora > 1700000000) {
    struct tm t;
    localtime_r(&f.hora, &t);
    strftime(buf, n, "%d/%m %H:%M:%S", &t);
  } else {
    snprintf(buf, n, "sin hora (no habia internet)");
  }
}

// Página web con la galería de fotos
void paginaGaleria() {
  String html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width'>"
    "<meta http-equiv='refresh' content='15'><title>VigIA - registro</title>"
    "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:12px}"
    ".g{display:flex;flex-wrap:wrap;gap:10px}figure{margin:0;background:#222;padding:6px;border-radius:8px}"
    "img{width:240px;display:block;border-radius:4px}figcaption{font-size:13px;margin-top:4px}</style>"
    "</head><body><h2>VigIA - registro de personas</h2>";
  xSemaphoreTake(candado, portMAX_DELAY);
  int desde = max(0, totalFotos - MAX_FOTOS);
  html += "<p>Fotos tomadas: " + String(totalFotos) + " (se guardan las ultimas " +
          String(MAX_FOTOS) + " en la memoria flash). Se actualiza cada 15 s.</p><div class='g'>";
  for (int n = totalFotos - 1; n >= desde; n--) {
    Foto &f = fotos[n % MAX_FOTOS];
    if (!f.usada || f.n != n) continue;
    char hora[40];
    textoHora(f, hora, sizeof(hora));
    String url = "/f?n=" + String(n) + "&a=" + String(idArranque, HEX);
    html += "<figure><a href='" + url + "'><img src='" + url +
            "'></a><figcaption>#" + String(n + 1) + " - persona " + String(f.persona) +
            "<br>" + hora + "</figcaption></figure>";
  }
  xSemaphoreGive(candado);
  html += "</div></body></html>";
  servidor.send(200, "text/html", html);
}

// Envía al navegador una foto guardada
void enviarFotoWeb() {
  int n = servidor.arg("n").toInt();
  xSemaphoreTake(candado, portMAX_DELAY);
  Foto f = fotos[n % MAX_FOTOS];
  xSemaphoreGive(candado);
  if (!f.usada || f.n != n) {
    servidor.send(404, "text/plain", "foto no disponible");
    return;
  }
  File arch = LittleFS.open(rutaFoto(f), "r");
  if (!arch) {
    servidor.send(404, "text/plain", "foto no disponible");
    return;
  }
  servidor.sendHeader("Cache-Control", "no-store");
  servidor.streamFile(arch, "image/jpeg");
  arch.close();
}

// Tarea aparte que atiende la galería web
void tareaServidor(void *) {
  for (;;) {
    servidor.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Busca la red WiFi y avisa si la ve, con qué señal y en qué canal
void buscarRedWiFi() {
  int n = WiFi.scanNetworks();
  char linea[96];
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_NOMBRE) {
      snprintf(linea, sizeof(linea), "WiFi: red vista, senal %d dBm, canal %d",
               WiFi.RSSI(i), WiFi.channel(i));
      info(linea);
      WiFi.scanDelete();
      return;
    }
  }
  snprintf(linea, sizeof(linea), "WiFi: NO veo la red (%d redes vistas). 5 GHz o lejos?", n);
  info(linea);
  WiFi.scanDelete();
}

// Traduce el estado del WiFi a palabras
const char *motivoWiFi(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:  return "red no encontrada";
    case WL_CONNECT_FAILED: return "fallo al conectar (clave?)";
    case WL_CONNECTION_LOST: return "conexion perdida";
    case WL_DISCONNECTED:   return "desconectado (clave o senal?)";
    case WL_IDLE_STATUS:    return "esperando";
    default:                return "otro";
  }
}

// Al conectarse al WiFi: pone la hora e inicia la galería web
void activarServiciosWiFi() {
  hayWiFi = true;
  configTime(-5 * 3600, 0, "pool.ntp.org", "time.google.com");
  servidor.on("/", paginaGaleria);
  servidor.on("/f", enviarFotoWeb);
  servidor.on("/estado", []() {
    char t[64];
    snprintf(t, sizeof(t), "%s %lu", pasoGuardado, millis() - inicioCiclo);
    servidor.send(200, "text/plain", t);
  });
  servidor.begin();
  xTaskCreatePinnedToCore(tareaServidor, "web", 8192, nullptr, 1, nullptr, 0);
  char linea[64];
  snprintf(linea, sizeof(linea), "Galeria: http://%s/", WiFi.localIP().toString().c_str());
  info(linea);
}

#define WIFI_REINTENTO_MS 30000UL             // cada cuánto se reintenta el WiFi
unsigned long ultimoIntentoWiFi = 0;          // momento del último intento
// Si no hay WiFi, lo reintenta cada 30 s y activa la galería cuando conecta
void revisarWiFi() {
  if (hayWiFi) return;
  if (WiFi.status() == WL_CONNECTED) {
    info("WiFi conectado (reintento)");
    activarServiciosWiFi();
    return;
  }
  if (millis() - ultimoIntentoWiFi < WIFI_REINTENTO_MS) return;
  ultimoIntentoWiFi = millis();
  char linea[80];
  snprintf(linea, sizeof(linea), "WiFi: reintento (%s)", motivoWiFi(WiFi.status()));
  info(linea);
  WiFi.disconnect();
  WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
}

// Se conecta al WiFi al arrancar (espera hasta 15 s)
void iniciarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  buscarRedWiFi();
  WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
  WiFi.setTxPower(WIFI_POWER_13dBm);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(300);
  if (WiFi.status() != WL_CONNECTED) {
    char linea[96];
    snprintf(linea, sizeof(linea), "WiFi no conectado (%s): reintento cada 30 s",
             motivoWiFi(WiFi.status()));
    info(linea);
    ultimoIntentoWiFi = millis();
    return;
  }
  activarServiciosWiFi();
}

// Reinicia el ESP32 si un ciclo se queda trabado más de VIGILANTE_S segundos
void tareaVigilante(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (inicioCiclo && millis() - inicioCiclo > VIGILANTE_S * 1000UL) {
      marcaVigilante = MARCA_VIGILANTE;
      esp_restart();
    }
  }
}

// Inicia la memoria, la cámara, la flash, el WiFi y el vigilante
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  enlace.begin(9600, SERIAL_8N1, PIN_RX_ARDUINO, PIN_TX_ARDUINO);
  delay(500);
  Serial.println("\n=== VigIA - Paso 7: sistema completo ===");

  candado = xSemaphoreCreateMutex();
  idArranque = esp_random();
  imagen    = (uint8_t *)ps_malloc(CAM_ANCHO * CAM_ALTO * 3);
  entrada   = (uint8_t *)ps_malloc(EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3);
  ultimaJpg = (uint8_t *)ps_malloc(JPG_MAX);
  if (!imagen || !entrada || !ultimaJpg) { info("ERROR sin memoria PSRAM"); while (true) delay(1000); }
  if (!iniciarCamara()) { info("ERROR camara"); while (true) delay(1000); }

  for (int i = 0; i < 5; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(100);
  }
  iniciarFlash();
  iniciarWiFi();

  bool porVigilante = (marcaVigilante == MARCA_VIGILANTE);
  marcaVigilante = 0;
  xTaskCreatePinnedToCore(tareaVigilante, "vigilante", 2048, nullptr, 2, nullptr, 0);
  if (porVigilante) {
    char l[64];
    snprintf(l, sizeof(l), "REINICIO por vigilante: trabado en '%s'", pasoGuardado);
    info(l);
  }
  info("VigIA listo: buscando rostros");
  enviarPosicion();
}

// Ciclo principal: foto, IA, elegir rostro nuevo, centrarlo / fotografiarlo o buscar, y mover los servos
void loop() {
  inicioCiclo = millis();
  revisarWiFi();
  if (!capturar()) { info("ERROR captura"); delay(500); return; }

  ei::signal_t senal;
  senal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  senal.get_data = &obtenerDatos;
  ei_impulse_result_t res = { 0 };
  paso("IA");
  if (run_classifier(&senal, &res, false) != EI_IMPULSE_OK) {
    info("ERROR al ejecutar el modelo");
    return;
  }

  paso("firmas");

  float mejorConf = 0;
  for (uint32_t i = 0; i < res.bounding_boxes_count; i++) {
    ei_impulse_result_bounding_box_t &b = res.bounding_boxes[i];
    if (enLugarEvitado(b.x + b.width / 2.0, b.y + b.height / 2.0)) continue;
    if (b.value > mejorConf) mejorConf = b.value;
  }
  float refX = (ultimoX < 0) ? EI_CLASSIFIER_INPUT_WIDTH / 2.0 : ultimoX;
  float refY = (ultimoY < 0) ? EI_CLASSIFIER_INPUT_HEIGHT / 2.0 : ultimoY;
  int rostros = 0, nuevos = 0, elegido = -1, ignorada = -1, evitados = 0;
  float mejorDist = 1e9, simCiclo = 0;
  for (uint32_t i = 0; i < res.bounding_boxes_count; i++) {
    ei_impulse_result_bounding_box_t &b = res.bounding_boxes[i];
    if (b.value < UMBRAL) continue;
    rostros++;
    float cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
    if (enLugarEvitado(cx, cy)) { evitados++; continue; }
    if (b.value < mejorConf - MARGEN_CONF) continue;
    float firma[NFIRMA], sim = 0;
    calcularFirma(cx, cy, firma);

    int p = buscarPersona(firma, UMBRAL_IGNORAR, &sim);
    if (sim > simCiclo) simCiclo = sim;

    bool pcDisponible = strlen(PC_IP) > 0 && hayWiFi && !pcCaida;
    if (p >= 0 && pcDisponible && millis() - personas[p].verificada > VERIF_MS) p = -1;
    if (p >= 0) { ignorada = p; continue; }
    nuevos++;

    float d = (cx - refX) * (cx - refX) + (cy - refY) * (cy - refY);
    if (d < mejorDist) { mejorDist = d; elegido = i; }
  }

  char linea[64];
  int ms = (int)(res.timing.dsp + res.timing.classification);
  if (elegido >= 0) {
    ei_impulse_result_bounding_box_t &b = res.bounding_boxes[elegido];
    snprintf(linea, sizeof(linea), "rostros %d (nuevos %d) | IA %d | x%d y%d c%d s%d", rostros, nuevos, ms,
             (int)(b.x + b.width / 2), (int)(b.y + b.height / 2), (int)(b.value * 100),
             (int)(simCiclo * 100));
  } else {
    snprintf(linea, sizeof(linea), "rostros %d (nuevos %d) | IA %d | s%d", rostros, nuevos, ms,
             (int)(simCiclo * 100));
  }
  info(linea);

  if (elegido < 0 && escanearConPC()) {

  } else if (elegido >= 0) {
    ciclosSinRostro = 0;
    ei_impulse_result_bounding_box_t &b = res.bounding_boxes[elegido];
    float cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
    ultimoX = cx; ultimoY = cy;

    registrar(cx, cy, b.value);

    float errX = cx - EI_CLASSIFIER_INPUT_WIDTH / 2.0;
    float errY = cy - EI_CLASSIFIER_INPUT_HEIGHT / 2.0;
    if (fabs(errX) > ZONA_MUERTA) {
      float paso = constrain(GANANCIA * errX * GRADOS_POR_PIXEL_MODELO, -PASO_MAX, PASO_MAX);
      panCmd += INVERTIR_PAN ? paso : -paso;
    }
    if (fabs(errY) > ZONA_MUERTA) {
      float paso = constrain(GANANCIA * errY * GRADOS_POR_PIXEL_MODELO, -PASO_MAX, PASO_MAX);
      tiltCmd += INVERTIR_TILT ? paso : -paso;
    }
    vistoX = cx;
    panVisto = panCmd;
  } else if (ignorada >= 0 || evitados > 0) {
    if (ignorada >= 0) avisarIgnorada(ignorada);
    ultimoX = ultimoY = -1;
    centradoSeguidas = 0;
    vistoX = -1;
    ciclosSinRostro = ESPERA_QUIETO + 3;
    panCmd += sentidoBusqueda * BUSQ_PASO;
    if (panCmd >= BUSQ_PAN_MAX) { panCmd = BUSQ_PAN_MAX; sentidoBusqueda = -1; }
    if (panCmd <= BUSQ_PAN_MIN) { panCmd = BUSQ_PAN_MIN; sentidoBusqueda = 1; }
    tiltCmd += (TILT_CENTRO - tiltCmd) * 0.3;
  } else {
    ciclosSinRostro++;
    ultimoX = ultimoY = -1;
    centradoSeguidas = 0;
    if (ciclosSinRostro >= CICLOS_FIN_SEGUIMIENTO) ultimaPersonaAvisada = -1;
    int haciaDerechaImagen = INVERTIR_PAN ? 1 : -1;

    if (ciclosSinRostro <= ESPERA_QUIETO) {

    } else if (ciclosSinRostro == ESPERA_QUIETO + 1) {
      if (vistoX >= 0 && vistoX > EI_CLASSIFIER_INPUT_WIDTH - BORDE_PX) {
        sentidoBusqueda = haciaDerechaImagen;
        panCmd = panVisto + sentidoBusqueda * PASO_BORDE;
      } else if (vistoX >= 0 && vistoX < BORDE_PX) {
        sentidoBusqueda = -haciaDerechaImagen;
        panCmd = panVisto + sentidoBusqueda * PASO_BORDE;
      } else {
        panCmd = panVisto + sentidoBusqueda * PASO_BORDE;
      }
    } else if (ciclosSinRostro == ESPERA_QUIETO + 2 &&
               vistoX >= BORDE_PX && vistoX <= EI_CLASSIFIER_INPUT_WIDTH - BORDE_PX) {
      panCmd = panVisto - sentidoBusqueda * PASO_BORDE;
      sentidoBusqueda = -sentidoBusqueda;
    } else {
      panCmd += sentidoBusqueda * BUSQ_PASO;
      if (panCmd >= BUSQ_PAN_MAX) { panCmd = BUSQ_PAN_MAX; sentidoBusqueda = -1; }
      if (panCmd <= BUSQ_PAN_MIN) { panCmd = BUSQ_PAN_MIN; sentidoBusqueda = 1; }
      tiltCmd += (TILT_CENTRO - tiltCmd) * 0.3;
      vistoX = -1;
    }
  }

  panCmd  = constrain(panCmd,  PAN_MIN,  PAN_MAX);
  tiltCmd = constrain(tiltCmd, TILT_MIN, TILT_MAX);
  enviarPosicion();
}
