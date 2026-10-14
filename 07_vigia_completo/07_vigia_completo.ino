/*
 * VigIA - Paso 7: Sistema completo
 * ------------------------------------------------------------
 *  1. Seguimiento de rostros con IA (igual que el Paso 5).
 *  La cámara NO sigue a nadie: recorre el lugar buscando rostros NUEVOS,
 *  los centra y les toma una foto; a los ya registrados los ignora.
 *  2. REGISTRO: toma UNA foto por persona cada 5 minutos.
 *       - Cuando un rostro queda centrado, calcula una "firma de color"
 *         (colores de la cara y la ropa, 64 números). NO es reconocimiento
 *         facial: no identifica a nadie, solo distingue "persona A" de
 *         "persona B" durante unos minutos (privacidad por diseño).
 *       - Si la firma se parece a alguien fotografiado hace menos de
 *         5 minutos -> no repite la foto.
 *       - Si es alguien nuevo -> guarda una foto 640x480.
 *  3. GALERÍA por WiFi: http://<IP>/ muestra las fotos con fecha y hora
 *     (se guardan en la memoria FLASH, máximo MAX_FOTOS, y NO se pierden al
 *     reiniciar o apagar; la microSD no funciona).
 *  4. (14 oct) RECONOCIMIENTO FACIAL EN LA PC: si en secretos.h está PC_IP y
 *     la PC corre pc_servidor\servidor_vigia.py, al centrar un rostro se le
 *     envía la foto a la PC, que decide con reconocimiento facial real
 *     (YuNet + SFace: ojos, nariz y boca) si es NUEVA o CONOCIDA. La PC guarda
 *     la foto y la muestra en su galería http://<IP de la PC>:8000/
 *     Si la PC no responde, se usa la firma de color de siempre (respaldo) y
 *     la foto va a la flash del ESP32.
 *
 * Conexiones (modo USO):
 *   ESP32 IO13 -> Arduino pin 2     ESP32 GND -> GND común
 *   ESP32 5V   -> + del CARGADOR (con WiFi, el 5V del Arduino no alcanza)
 *
 * Requiere:
 *   - Librería de Edge Impulse MUCKCITO-project-1_inferencing
 *   - secretos.h con el WiFi (pestaña junto a este archivo) y, para usar la
 *     PC, la línea:  #define PC_IP "192.168.18.50"  (la IP que muestra el
 *     servidor al arrancar)
 *
 * Mensajes en el Monitor Serie del Arduino (9600):
 *   [ESP32 info] rostros 1 | IA 700 | x48 y50 c95   -> seguimiento
 *   [ESP32 info] FOTO #3 persona 2 (PC)               -> la PC guardó la foto
 *   [ESP32 info] persona 2 ya registrada (hace 40s)   -> no repite
 *   [ESP32 info] no era un rostro (dice la PC)        -> FOMO se equivocó
 *   [ESP32 info] PC no responde: uso la firma de color -> modo respaldo
 *
 * Errores comunes (PC):
 *   "PC no responde" -> servidor apagado, otra red WiFi, IP de la PC cambió
 *       (actualizar PC_IP y volver a subir) o el firewall de Windows bloquea
 *       Python (permitirlo en "Redes privadas").
 */

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

// ---- Reconocimiento facial en la PC (pc_servidor\servidor_vigia.py) ----
// La IP se pone en secretos.h:  #define PC_IP "192.168.18.50"
// Si no está (secretos.h viejo), funciona como antes, solo con la firma de color.
#ifndef PC_IP
#define PC_IP ""
#endif
#define PC_PUERTO        8000
#define PC_TIMEOUT_MS    4000    // espera máxima de la respuesta de la PC
#define PC_REINTENTO_MS  30000   // si la PC falló, no volver a probar hasta 30 s después
// Respuestas de la PC (con #define y no enum: el preprocesador del .ino
// pone los prototipos antes del enum y no compila)
#define PC_FALLA      0
#define PC_NUEVA      1
#define PC_CONOCIDA   2
#define PC_SIN_ROSTRO 3
#define ID_SIN_ROSTRO 0          // "persona" 0 = algo que FOMO creyó rostro y no lo es

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
// 640x480: la foto del registro sale con buena calidad. Para la IA se
// recorta el centro cuadrado (480x480) y se reduce a 96x96, que equivale
// al mismo encuadre que antes (el campo de visión no cambia).
#define CAM_ANCHO 640
#define CAM_ALTO  480
#define RECORTE_X ((CAM_ANCHO - CAM_ALTO) / 2)                 // 80
#define ESCALA    ((float)CAM_ALTO / EI_CLASSIFIER_INPUT_WIDTH)  // 5
#define GRADOS_POR_PIXEL_MODELO (49.0 / EI_CLASSIFIER_INPUT_WIDTH)
#define JPG_MAX   (150 * 1024)   // tamaño máximo esperado de una foto JPEG

// ---- Seguimiento (valores probados el 13-14 oct) ----
#define INVERTIR_PAN   true
#define INVERTIR_TILT  true
#define GANANCIA       0.65
#define MARGEN_CONF    0.15
#define ZONA_MUERTA    4
#define PASO_MAX       15
#define UMBRAL         0.30

// ---- "Mover, esperar, mirar" (iguales al Arduino) ----
#define ARDUINO_VEL   50.0
#define ARDUINO_ACEL  60.0
#define ASENTAR_MS    120

// ---- Límites (iguales al Arduino) ----
#define PAN_MIN     50
#define PAN_MAX    160
#define PAN_CENTRO 110
#define TILT_MIN    10
#define TILT_MAX   120
#define TILT_CENTRO 70

// ---- Al perder el rostro ----
#define ESPERA_QUIETO   2
#define BORDE_PX       24
#define PASO_BORDE     20
#define BUSQ_PAN_MIN   60
#define BUSQ_PAN_MAX  155
#define BUSQ_PASO      25

// ---- Registro de fotos ----
#define VENTANA_MS     (5UL * 60UL * 1000UL)  // 5 minutos por persona
#define CENTRADO_PX    8       // el rostro debe estar a menos de esto del centro
#define CONFIRMACIONES 2       // detecciones centradas SEGUIDAS antes de tomar la foto
#define ESPERA_INICIO_MS 3000  // sin fotos al encender (la cámara ajusta la luz)
#define CONF_FOTO      0.60    // confianza mínima para tomar la foto
// Similitud mínima para decir "es la misma persona" (0..1).
//   Si en el lugar se REPITEN fotos de la misma persona -> bajarlo (0.65).
//   Si CONFUNDE a personas distintas (no les toma foto)  -> subirlo (0.75).
// Se prefiere estricto: es peor perder a alguien que repetir una foto.
// (14 oct, opción A "estricto") Medido en prueba: la misma persona daba
// s91-95 y otra persona distinta s80-89 (se cruzan). Se eligió no perder
// a nadie aunque se repitan fotos: casi nunca se asume "es la misma".
#define UMBRAL_MISMA   0.92
// Para IGNORAR un rostro visto de lejos / descentrado (sin ir a verlo) se
// exige una coincidencia mucho mayor: de lejos las firmas se parecen
// demasiado y confundía a personas nuevas con alguien ya registrado.
// Si hay duda, la cámara se acerca a verlo y compara ya centrado.
#define UMBRAL_IGNORAR 0.96
#define CICLOS_FIN_SEGUIMIENTO 4  // ciclos sin nadie para volver a avisar "ya registrada"
#define MAX_PERSONAS   30      // personas registradas a la vez (para lugares con gente)
#define MAX_FOTOS      40      // fotos guardadas en la FLASH (~20 KB c/u en ~1 MB; las más
                               // viejas se borran). Sobreviven a reinicios y apagados.

// ===================================================================
uint8_t *imagen  = nullptr;   // RGB888 640x480 (BGR) en PSRAM
uint8_t *entrada = nullptr;   // RGB888 96x96 para el modelo
uint8_t *ultimaJpg = nullptr; // copia del JPEG de la última captura
size_t   ultimaJpgLen = 0;

float panCmd = PAN_CENTRO, tiltCmd = TILT_CENTRO;
int ultimoPanEnviado = -1, ultimoTiltEnviado = -1;
int ciclosSinRostro = 0;
int sentidoBusqueda = 1;
float ultimoX = -1, ultimoY = -1;
float vistoX = -1;
float panVisto = PAN_CENTRO;

// ---- Personas vistas recientemente (solo firma de color + hora) ----
#define NFIRMA 64
struct Persona {
  bool activa;
  int id;
  float firma[NFIRMA];
  unsigned long ultimaFoto;   // millis() de su última foto
};
Persona personas[MAX_PERSONAS];
int siguienteIdPersona = 1;
int ultimaPersonaAvisada = -1;
int centradoSeguidas = 0;             // detecciones centradas consecutivas (para confirmar)

// ---- Fotos guardadas en la memoria FLASH (LittleFS) ----
// Cada foto es un archivo "/f<numero>_p<persona>_t<hora>.jpg". Como están en
// la flash, NO se pierden si el ESP32 se reinicia o se desenchufa. Aquí solo
// se guarda la información de cada una (la imagen se lee del archivo).
struct Foto {
  bool usada;
  int n;              // número de foto (sigue contando después de reiniciar)
  int persona;
  time_t hora;        // hora real (0 si no había NTP)
};
Foto fotos[MAX_FOTOS];
int totalFotos = 0;                 // próximo número de foto (guardado en Preferences)
SemaphoreHandle_t candado;          // protege fotos[] (lo usa también el servidor web)
Preferences prefs;
bool hayFlash = false;

String rutaFoto(const Foto &f) {
  return "/f" + String(f.n) + "_p" + String(f.persona) + "_t" + String((long)f.hora) + ".jpg";
}

WebServer servidor(80);
bool hayWiFi = false;
// Código único de cada arranque: va en la dirección de cada foto para que
// el navegador NO muestre fotos viejas guardadas en su caché (la numeración
// de fotos vuelve a empezar en 0 cada vez que el ESP32 se reinicia).
uint32_t idArranque = 0;

// ===================================================================
//  Cámara y modelo
// ===================================================================
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
  config.frame_size   = FRAMESIZE_VGA;     // 640x480
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&config) != ESP_OK) return false;

  // Montada de cabeza: girar la imagen 180°
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
  return true;
}

// Captura: guarda una copia del JPEG (por si hay que registrarlo),
// lo decodifica a 640x480 y prepara la entrada 96x96 del modelo
void reducirAModelo();

bool capturar() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;
  if (fb->len <= JPG_MAX) {
    memcpy(ultimaJpg, fb->buf, fb->len);
    ultimaJpgLen = fb->len;
  } else {
    ultimaJpgLen = 0;
  }
  bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, imagen);
  esp_camera_fb_return(fb);
  if (!ok) return false;
  reducirAModelo();
  return true;
}

// Recorta el centro 480x480 de la imagen 640x480 y lo reduce a 96x96
// promediando bloques de 5x5 píxeles. Escribe directo en 'entrada' (27 KB).
// OJO: no usar crop_and_interpolate_rgb888 de Edge Impulse con un buffer
// de salida distinto: primero copia el recorte COMPLETO (480x480x3 = 690 KB)
// en el destino y desborda la memoria (causaba "Guru Meditation" al arrancar).
void reducirAModelo() {
  const int lado = EI_CLASSIFIER_INPUT_WIDTH;    // 96
  const int bloque = (int)ESCALA;                // 5
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
      out[0] = sB / n; out[1] = sG / n; out[2] = sR / n;   // se mantiene el orden BGR
    }
  }
}

static int obtenerDatos(size_t offset, size_t largo, float *salida) {
  size_t ix = offset * 3;
  for (size_t i = 0; i < largo; i++, ix += 3) {
    // La cámara entrega BGR: se reordena a RGB
    salida[i] = (entrada[ix + 2] << 16) + (entrada[ix + 1] << 8) + entrada[ix];
  }
  return 0;
}

// ===================================================================
//  Movimiento
// ===================================================================
unsigned long tiempoMovimiento(float grados) {
  if (grados <= 0) return 0;
  float dAcel = ARDUINO_VEL * ARDUINO_VEL / ARDUINO_ACEL;
  float t = (grados <= dAcel)
              ? 2.0 * sqrt(grados / ARDUINO_ACEL)
              : grados / ARDUINO_VEL + ARDUINO_VEL / ARDUINO_ACEL;
  return (unsigned long)(t * 1000);
}

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
  camera_fb_t *fb = esp_camera_fb_get();   // descartar la imagen movida
  if (fb) esp_camera_fb_return(fb);
}

void info(const char *texto) {
  enlace.printf("#%s\n", texto);
  Serial.println(texto);
}

// ===================================================================
//  Registro: firma de color y fotos
// ===================================================================

// Firma de la persona centrada en la CABEZA (no en la ropa: en el
// laboratorio todos usan bata blanca, así que la ropa no sirve para
// distinguir). La cabeza se divide en 3 franjas:
//   franja 0: pelo / frente     franja 1: ojos / nariz     franja 2: boca / mentón
// Por franja: histograma de cromaticidad 4x4 (r = R/suma, g = G/suma)
// -> tono de piel, color de pelo, lentes, barba. Más un histograma de
// brillo (16 niveles) de toda la cabeza. Cada parte pesa 1/4 del total.
// (cx, cy) en coordenadas del modelo (96x96). 3*16 + 16 = 64 casillas.
void calcularFirma(float cx, float cy, float firma[NFIRMA]) {
  int fx = RECORTE_X + (int)(cx * ESCALA);
  int fy = (int)(cy * ESCALA);
  const int limites[4] = { fy - 70, fy - 25, fy + 15, fy + 55 };  // bordes de las franjas
  int x0 = max(0, fx - 45), x1 = min(CAM_ANCHO - 1, fx + 45);
  for (int i = 0; i < NFIRMA; i++) firma[i] = 0;
  float totalFranja[3] = {0, 0, 0}, totalBrillo = 0;

  for (int f = 0; f < 3; f++) {
    int y0 = max(0, limites[f]), y1 = min(CAM_ALTO - 1, limites[f + 1]);
    for (int y = y0; y < y1; y += 2) {
      for (int x = x0; x <= x1; x += 2) {
        uint8_t *px = imagen + (y * CAM_ANCHO + x) * 3;   // B, G, R
        int suma = px[0] + px[1] + px[2];
        firma[48 + min(15, suma / 48)] += 1;              // brillo (0..765 -> 16 niveles)
        totalBrillo += 1;
        if (suma < 60) continue;                          // muy oscuro: el color no es fiable
        int r = (px[2] * 4) / (suma + 1);                 // 0..3
        int g = (px[1] * 4) / (suma + 1);                 // 0..3
        firma[f * 16 + r * 4 + g] += 1;
        totalFranja[f] += 1;
      }
    }
  }
  // Normalizar: cada franja y el brillo suman 0.25 (total = 1)
  for (int f = 0; f < 3; f++)
    if (totalFranja[f] > 0) for (int k = 0; k < 16; k++) firma[f * 16 + k] *= 0.25 / totalFranja[f];
  if (totalBrillo > 0) for (int k = 48; k < 64; k++) firma[k] *= 0.25 / totalBrillo;
}

// 1.0 = colores idénticos, 0.0 = nada en común
float similitud(const float *a, const float *b) {
  float s = 0;
  for (int i = 0; i < NFIRMA; i++) s += min(a[i], b[i]);
  return s;
}

// Devuelve el índice de la persona registrada hace < 5 min que más se
// parece, si supera 'umbral' (o -1). En *simMax deja la mayor similitud
// encontrada (para mostrarla en los mensajes y poder ajustar los umbrales).
int buscarPersona(const float *firma, float umbral, float *simMax) {
  int mejor = -1;
  float mejorSim = 0;
  unsigned long ahora = millis();
  for (int i = 0; i < MAX_PERSONAS; i++) {
    if (!personas[i].activa) continue;
    if (ahora - personas[i].ultimaFoto > VENTANA_MS) {   // pasaron 5 min: se olvida
      personas[i].activa = false;
      continue;
    }
    float s = similitud(firma, personas[i].firma);
    if (s > mejorSim) { mejorSim = s; mejor = i; }
  }
  if (simMax) *simMax = mejorSim;
  return (mejorSim >= umbral) ? mejor : -1;
}

// Registra una persona con el número 'id' (lo da la PC, o siguienteIdPersona
// en modo respaldo). Devuelve su índice en personas[].
int nuevaPersona(const float *firma, int id) {
  int libre = -1;
  unsigned long masVieja = 0;
  for (int i = 0; i < MAX_PERSONAS; i++) {
    if (!personas[i].activa) { libre = i; break; }
    unsigned long edad = millis() - personas[i].ultimaFoto;
    if (edad >= masVieja) { masVieja = edad; libre = i; }   // si está lleno, reusar la más vieja
  }
  Persona &p = personas[libre];
  p.activa = true;
  p.id = id;
  if (id >= siguienteIdPersona) siguienteIdPersona = id + 1;  // no repetir números de la PC
  memcpy(p.firma, firma, sizeof(p.firma));
  p.ultimaFoto = millis();
  return libre;
}

// Índice de la persona con ese número (o -1)
int buscarPorId(int id) {
  for (int i = 0; i < MAX_PERSONAS; i++)
    if (personas[i].activa && personas[i].id == id) return i;
  return -1;
}

// Borra la foto más vieja que haya en la flash (para liberar espacio)
bool borrarMasVieja() {
  int idx = -1;
  for (int i = 0; i < MAX_FOTOS; i++)
    if (fotos[i].usada && (idx < 0 || fotos[i].n < fotos[idx].n)) idx = i;
  if (idx < 0) return false;
  LittleFS.remove(rutaFoto(fotos[idx]));
  fotos[idx].usada = false;
  return true;
}

bool guardarFoto(int idPersona) {
  if (ultimaJpgLen == 0 || !hayFlash) return false;
  xSemaphoreTake(candado, portMAX_DELAY);

  Foto &f = fotos[totalFotos % MAX_FOTOS];
  if (f.usada) { LittleFS.remove(rutaFoto(f)); f.usada = false; }   // el lugar de la más vieja
  // Si no queda espacio en la flash, borrar las más viejas
  while (LittleFS.totalBytes() - LittleFS.usedBytes() < ultimaJpgLen + 16 * 1024) {
    if (!borrarMasVieja()) break;
  }

  Foto nueva = { true, totalFotos, idPersona, time(nullptr) };
  if (nueva.hora < 1700000000) nueva.hora = 0;   // sin hora real
  File arch = LittleFS.open(rutaFoto(nueva), "w");
  bool ok = arch && arch.write(ultimaJpg, ultimaJpgLen) == ultimaJpgLen;
  if (arch) arch.close();
  if (ok) {
    f = nueva;
    totalFotos++;
    prefs.putInt("total", totalFotos);   // el número sigue después de reiniciar
  } else {
    LittleFS.remove(rutaFoto(nueva));
  }
  xSemaphoreGive(candado);
  return ok;
}

// Al arrancar: monta la flash y carga la lista de fotos que ya había
void iniciarFlash() {
  prefs.begin("vigia", false);
  totalFotos = prefs.getInt("total", 0);
  if (!LittleFS.begin(true)) {   // true = formatear si es la primera vez
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
      LittleFS.remove("/" + nombre);   // archivo viejo o desconocido
    }
    arch = raiz.openNextFile();
  }
  char linea[64];
  snprintf(linea, sizeof(linea), "Flash: %d fotos guardadas (total %d)", cargadas, totalFotos);
  info(linea);
}

// Avisa (una sola vez seguida) que se está ignorando a una persona registrada
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

// Envía la foto actual (ultimaJpg) a la PC y lee su respuesta.
// Devuelve PC_NUEVA / PC_CONOCIDA / PC_SIN_ROSTRO, o PC_FALLA si no hay
// PC configurada o no respondió. 'persona' y 'dato' (número de foto, o
// segundos desde su foto) se llenan con lo que diga la PC.
bool pcCaida = false;               // la última vez la PC no respondió
unsigned long pcCaidaDesde = 0;

int preguntarPC(int &persona, int &dato) {
  if (strlen(PC_IP) == 0 || !hayWiFi || WiFi.status() != WL_CONNECTED || ultimaJpgLen == 0)
    return PC_FALLA;
  if (pcCaida && millis() - pcCaidaDesde < PC_REINTENTO_MS) return PC_FALLA;  // no insistir

  int resp = PC_FALLA;
  HTTPClient http;
  http.setConnectTimeout(PC_TIMEOUT_MS);
  http.setTimeout(PC_TIMEOUT_MS);
  String url = String("http://") + PC_IP + ":" + String(PC_PUERTO) + "/rostro";
  if (http.begin(url)) {
    http.addHeader("Content-Type", "image/jpeg");
    if (http.POST(ultimaJpg, ultimaJpgLen) == 200) {
      String txt = http.getString();
      txt.trim();
      if (sscanf(txt.c_str(), "NUEVA %d %d", &persona, &dato) == 2)         resp = PC_NUEVA;
      else if (sscanf(txt.c_str(), "CONOCIDA %d %d", &persona, &dato) == 2) resp = PC_CONOCIDA;
      else if (txt == "SIN_ROSTRO")                                         resp = PC_SIN_ROSTRO;
    }
    http.end();
  }

  // Avisar solo cuando cambia (para no llenar el Monitor Serie)
  if (resp == PC_FALLA) {
    if (!pcCaida) info("PC no responde: uso la firma de color");
    pcCaida = true;
    pcCaidaDesde = millis();
  } else if (pcCaida) {
    info("PC conectada de nuevo");
    pcCaida = false;
  }
  return resp;
}

// Se llama con el rostro NUEVO que se está centrando: cuando queda centrado
// y confirmado, vuelve a comparar su firma (ahora bien encuadrada) y, si de
// verdad es nuevo, toma la foto.
void registrar(float cx, float cy, float conf) {
  float errX = cx - EI_CLASSIFIER_INPUT_WIDTH / 2.0;
  float errY = cy - EI_CLASSIFIER_INPUT_HEIGHT / 2.0;
  if (millis() < ESPERA_INICIO_MS) return;
  if (fabs(errX) > CENTRADO_PX || fabs(errY) > CENTRADO_PX || conf < CONF_FOTO) {
    centradoSeguidas = 0;              // aún no centrado o detección dudosa
    return;
  }
  if (++centradoSeguidas < CONFIRMACIONES) return;   // esperar a confirmarlo
  centradoSeguidas = 0;

  float firma[NFIRMA];
  calcularFirma(cx, cy, firma);
  char linea[64];

  // ---- 1) Preguntarle a la PC (reconocimiento facial real) ----
  int idPC = 0, dato = 0;
  int resp = preguntarPC(idPC, dato);
  if (resp == PC_NUEVA || resp == PC_CONOCIDA) {
    // Guardar su firma de color con el número que dio la PC: sirve para
    // ignorarla de lejos sin tener que preguntar otra vez
    int j = buscarPorId(idPC);
    if (j < 0) {
      j = nuevaPersona(firma, idPC);
    } else {
      for (int k = 0; k < NFIRMA; k++)
        personas[j].firma[k] = 0.6 * personas[j].firma[k] + 0.4 * firma[k];
    }
    // Misma hora de la foto que en la PC, así se "olvida" a la vez (5 min)
    unsigned long hace = (resp == PC_CONOCIDA) ? (unsigned long)dato * 1000UL : 0;
    personas[j].ultimaFoto = millis() - min(hace, millis());

    if (resp == PC_NUEVA) {
      snprintf(linea, sizeof(linea), "FOTO #%d persona %d (PC)", dato, idPC);
      info(linea);
      ultimaPersonaAvisada = idPC;
    } else {
      avisarIgnorada(j);
    }
    return;
  }
  if (resp == PC_SIN_ROSTRO) {
    // FOMO creyó ver un rostro (cuadro, monitor, mochila...): ignorarlo 5 min
    nuevaPersona(firma, ID_SIN_ROSTRO);
    if (ultimaPersonaAvisada != ID_SIN_ROSTRO) info("no era un rostro (dice la PC): lo ignoro");
    ultimaPersonaAvisada = ID_SIN_ROSTRO;
    return;
  }

  // ---- 2) Sin PC (respaldo): firma de color, como antes ----
  float sim = 0;
  int i = buscarPersona(firma, UMBRAL_MISMA, &sim);   // ya centrado: comparación normal

  if (i >= 0) {
    // De cerca resultó ser alguien ya registrado: se mejora su firma con
    // esta vista centrada (así la próxima vez se la reconoce de lejos)
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

// ===================================================================
//  Galería web (corre en el otro núcleo del ESP32)
// ===================================================================
void textoHora(const Foto &f, char *buf, size_t n) {
  if (f.hora > 1700000000) {   // hay hora real (NTP)
    struct tm t;
    localtime_r(&f.hora, &t);
    strftime(buf, n, "%d/%m %H:%M:%S", &t);
  } else {
    snprintf(buf, n, "sin hora (no habia internet)");
  }
}

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

void tareaServidor(void *) {
  for (;;) {
    servidor.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void iniciarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
  // (14 oct) Potencia de transmisión reducida: los picos de corriente del
  // WiFi a máxima potencia provocaban "Brownout detector was triggered"
  // (el ESP32 se reiniciaba y perdía las fotos). A pocos metros del router
  // alcanza con 8.5 dBm.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(300);
  if (WiFi.status() != WL_CONNECTED) {
    info("WiFi no conectado: sin galeria (fotos en flash)");
    return;
  }
  hayWiFi = true;
  configTime(-5 * 3600, 0, "pool.ntp.org", "time.google.com");   // hora de Perú (UTC-5)
  servidor.on("/", paginaGaleria);
  servidor.on("/f", enviarFotoWeb);
  servidor.begin();
  xTaskCreatePinnedToCore(tareaServidor, "web", 8192, nullptr, 1, nullptr, 0);
  char linea[64];
  snprintf(linea, sizeof(linea), "Galeria: http://%s/", WiFi.localIP().toString().c_str());
  info(linea);
}

// ===================================================================
void setup() {
  // (14 oct) Detector de "brownout" desactivado: con el cargador compartido
  // con los servos, las caídas breves de voltaje reiniciaban el ESP32.
  // Lo ideal igual es un capacitor de 470-1000 µF en 5V/GND o un cargador
  // aparte para el ESP32.
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
  info("VigIA listo: buscando rostros");
  enviarPosicion();
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

  // ---- Clasificar los rostros: NUEVOS o YA REGISTRADOS ----
  // La cámara no "sigue" a nadie: busca rostros NUEVOS, los fotografía una
  // vez y a los ya registrados (hace < 5 min) los ignora y sigue buscando.
  float mejorConf = 0;
  for (uint32_t i = 0; i < res.bounding_boxes_count; i++) {
    if (res.bounding_boxes[i].value > mejorConf) mejorConf = res.bounding_boxes[i].value;
  }
  float refX = (ultimoX < 0) ? EI_CLASSIFIER_INPUT_WIDTH / 2.0 : ultimoX;
  float refY = (ultimoY < 0) ? EI_CLASSIFIER_INPUT_HEIGHT / 2.0 : ultimoY;
  int rostros = 0, nuevos = 0, elegido = -1, ignorada = -1;
  float mejorDist = 1e9, simCiclo = 0;
  for (uint32_t i = 0; i < res.bounding_boxes_count; i++) {
    ei_impulse_result_bounding_box_t &b = res.bounding_boxes[i];
    if (b.value < UMBRAL) continue;
    rostros++;
    if (b.value < mejorConf - MARGEN_CONF) continue;
    float cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
    float firma[NFIRMA], sim = 0;
    calcularFirma(cx, cy, firma);
    // De lejos solo se ignora si la coincidencia es MUY alta; si hay duda
    // se trata como "posible nuevo" y la cámara se acerca a verlo
    int p = buscarPersona(firma, UMBRAL_IGNORAR, &sim);
    if (sim > simCiclo) simCiclo = sim;
    if (p >= 0) { ignorada = p; continue; }        // ya registrada: se ignora
    nuevos++;
    // Entre los nuevos, el más cercano al que se venía centrando (para
    // terminar con uno antes de pasar al siguiente)
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

  if (elegido >= 0) {
    // ---- HAY UN ROSTRO NUEVO: centrarlo y fotografiarlo ----
    ciclosSinRostro = 0;
    ei_impulse_result_bounding_box_t &b = res.bounding_boxes[elegido];
    float cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
    ultimoX = cx; ultimoY = cy;

    registrar(cx, cy, b.value);   // foto si está centrado y es alguien nuevo

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
  } else if (ignorada >= 0) {
    // ---- SOLO HAY PERSONAS YA REGISTRADAS: ignorarlas y seguir buscando ----
    avisarIgnorada(ignorada);
    ultimoX = ultimoY = -1;
    centradoSeguidas = 0;
    vistoX = -1;
    ciclosSinRostro = ESPERA_QUIETO + 3;   // si luego no ve a nadie, sigue el barrido sin esperar
    panCmd += sentidoBusqueda * BUSQ_PASO;
    if (panCmd >= BUSQ_PAN_MAX) { panCmd = BUSQ_PAN_MAX; sentidoBusqueda = -1; }
    if (panCmd <= BUSQ_PAN_MIN) { panCmd = BUSQ_PAN_MIN; sentidoBusqueda = 1; }
    tiltCmd += (TILT_CENTRO - tiltCmd) * 0.3;
  } else {
    // ---- NO HAY NADIE ----
    ciclosSinRostro++;
    ultimoX = ultimoY = -1;
    centradoSeguidas = 0;
    if (ciclosSinRostro >= CICLOS_FIN_SEGUIMIENTO) ultimaPersonaAvisada = -1;
    int haciaDerechaImagen = INVERTIR_PAN ? 1 : -1;

    if (ciclosSinRostro <= ESPERA_QUIETO) {
      // quieto: probablemente sigue ahí
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
